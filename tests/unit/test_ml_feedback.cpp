#include "feedback_test_support.hpp"

#include <limits>
#include <google/protobuf/util/message_differencer.h>

using namespace feedback_test;
using harbinger::DLQReason;

TEST(FeedbackEvent, VersionOneFixtureMatchesGeneratedRecord) {
    std::ifstream input(ML_FEEDBACK_FIXTURE);
    ASSERT_TRUE(input.good());
    const std::string fixture{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto e = event(); e.collected_at = std::chrono::system_clock::from_time_t(0); e.elapsed_since_arrival_ms = 7;
    EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(
        parse(fixture), parse(feedback_json(e, "fixture-instance", 42, 100))));
}

TEST(FeedbackEvent, ShadowFallbackFixtureMatchesGeneratedRecord) {
    std::ifstream input(ML_FEEDBACK_SHADOW_FIXTURE);
    ASSERT_TRUE(input.good());
    const std::string fixture{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto e = event(); e.collected_at = std::chrono::system_clock::from_time_t(0); e.elapsed_since_arrival_ms = 7;
    auto context = *e.routing;
    context.mode = harbinger::ml::RoutingMode::Shadow;
    context.model_version = "per-key-v2;boundaries=3";
    context.fallback_reason = harbinger::ml::FallbackReason::ColdKey;
    context.inference_elapsed_ms = 0.25;
    context.predictor_key = "producer-0\x1fsecret-job";  // broker-internal: must not appear in the record
    e.routing = std::make_shared<const harbinger::ml::RoutingContext>(context);
    const auto json = feedback_json(e, "fixture-instance", 42, 100);
    EXPECT_EQ(json.find("secret-job"), std::string::npos);
    EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(parse(fixture), parse(json)));
}

TEST(FeedbackEvent, OrderedLabelRulesKeepActualOutcomeIndependent) {
    auto e = event();
    EXPECT_EQ(classify_label(e, 100), LabelStatus::Eligible);
    e.processing_time_ms.reset(); EXPECT_EQ(classify_label(e, 100), LabelStatus::Missing);
    e.processing_time_ms = -1; e.dlq_reason = DLQReason::TTL_EXPIRED;
    EXPECT_EQ(classify_label(e, 100), LabelStatus::Negative);
    e.processing_time_ms = 101; EXPECT_EQ(classify_label(e, 100), LabelStatus::OutOfRange);
    e.processing_time_ms = 100;
    auto context = std::make_shared<RoutingContext>(*e.routing);
    context->features.reset(); context->feature_validity = FeatureValidity::FeatureLimit;
    e.routing = context; EXPECT_EQ(classify_label(e, 100), LabelStatus::InvalidFeatures);
    context->features = FeatureSnapshot{}; context->feature_validity = FeatureValidity::Valid;
    EXPECT_EQ(classify_label(e, 100), LabelStatus::Censored);
    e.operation = FeedbackOperation::Nack; EXPECT_EQ(classify_label(e, 100), LabelStatus::Censored);
    e.dlq_reason.reset(); e.outcome = FeedbackOutcome::Retry;
    EXPECT_EQ(classify_label(e, 100), LabelStatus::Failure);
    context->features->headers["units"] = std::monostate{};
    context->features->missing_reasons["units"] = MissingReason::Absent;
    e.operation = FeedbackOperation::Ack; e.outcome = FeedbackOutcome::Ack;
    EXPECT_EQ(classify_label(e, 100), LabelStatus::Eligible);
}

TEST(FeedbackEvent, JsonHasExactIdentitiesNullsContextAndStructuredOutcomes) {
    auto e = event();
    e.attempt_id = std::numeric_limits<uint64_t>::max();
    auto json = feedback_json(e, "instance", std::numeric_limits<uint64_t>::max(), 100);
    const auto parsed = parse(json);
    EXPECT_EQ(parsed.fields().at("attempt_id").string_value(), "18446744073709551615");
    EXPECT_EQ(parsed.fields().at("event_id").string_value(), "instance:18446744073709551615");
    EXPECT_EQ(parsed.fields().at("label_status").string_value(), "eligible");
    const auto& routing = parsed.fields().at("routing").struct_value().fields();
    EXPECT_EQ(routing.at("features").struct_value().fields().at("payload_size_bytes").string_value(), "4096");
    EXPECT_EQ(routing.at("model_version").kind_case(), google::protobuf::Value::kNullValue);
    e.attempt_id.reset(); e.processing_time_ms.reset(); e.operation.reset();
    e.trigger = FeedbackTrigger::TtlSweep; e.outcome = FeedbackOutcome::Dlq; e.dlq_reason = DLQReason::TTL_EXPIRED;
    const auto swept = parse(feedback_json(e, "instance", 2, 100));
    EXPECT_EQ(swept.fields().at("attempt_id").kind_case(), google::protobuf::Value::kNullValue);
    EXPECT_EQ(swept.fields().at("processing_time_ms").kind_case(), google::protobuf::Value::kNullValue);
    EXPECT_EQ(swept.fields().at("dlq_reason").string_value(), "TTL_EXPIRED");
    EXPECT_EQ(swept.fields().at("label_status").string_value(), "missing");
}

TEST(FeedbackEvent, EncodingLimitsAndPrivacyUseOnlyImmutableSnapshot) {
    harbinger::Message m;
    m.id = "123-0"; m.arrival_time = std::chrono::steady_clock::now();
    m.payload = {'s', 'e', 'c', 'r', 'e', 't'};
    m.headers = {{"authorization", "private-sentinel"}, {"__producer_id", "producer-sentinel"}};
    m.routing_context = event().routing;
    auto e = capture_feedback(m, FeedbackTrigger::Submit);
    const auto json = feedback_json(e, "instance", 1, 100);
    EXPECT_EQ(json.find("secret"), std::string::npos);
    EXPECT_EQ(json.find("sentinel"), std::string::npos);
    EXPECT_EQ(feedback_json(e, "instance", 1, 100, json.size()), json);
    EXPECT_THROW((void)feedback_json(e, "instance", 1, 100, json.size() - 1), std::length_error);
    e.message_id = "quote\"\\\n";
    EXPECT_EQ(parse(feedback_json(e, "instance", 1, 100)).fields().at("message_id").string_value(), e.message_id);
    auto context = std::make_shared<RoutingContext>(*e.routing);
    context->mode = RoutingMode::Shadow; context->model_version = "model-v1";
    context->predicted_processing_time_ms = 1.25; context->predicted_bucket = 0;
    context->inference_elapsed_ms = .5; e.routing = context;
    EXPECT_EQ(parse(feedback_json(e, "instance", 1, 100)).fields().at("routing").struct_value()
        .fields().at("predicted_processing_time_ms").number_value(), 1.25);
    context->predicted_processing_time_ms = std::numeric_limits<double>::infinity();
    EXPECT_THROW((void)feedback_json(e, "instance", 1, 100), std::invalid_argument);
}
