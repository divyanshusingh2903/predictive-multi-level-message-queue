#include "ml/features.hpp"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>

#include <bit>
#include <fstream>
#include <limits>
#include <sstream>

using namespace harbinger::ml;

namespace {
using Object = google::protobuf::Struct;
using Value = google::protobuf::Value;

const Value& field(const Object& object, const char* name) { return object.fields().at(name); }

Object corpus() {
    std::ifstream file(ML_FEATURE_FIXTURES);
    if (!file) throw std::runtime_error("cannot open shared ML fixtures");
    std::ostringstream data;
    data << file.rdbuf();
    Object result;
    if (!google::protobuf::util::JsonStringToMessage(data.str(), &result).ok())
        throw std::runtime_error("invalid shared ML fixtures");
    return result;
}

FeatureSchema schema(const Object& object) {
    FeatureSchema result{.version = field(object, "version").string_value()};
    for (const auto& item : field(object, "headers").list_value().values()) {
        const auto& spec = item.struct_value();
        HeaderFeature header{.name = field(spec, "name").string_value()};
        if (field(spec, "type").string_value() == "numeric") {
            header.minimum = field(spec, "minimum").number_value();
            header.maximum = field(spec, "maximum").number_value();
        } else {
            header.type = FeatureType::Categorical;
            if (spec.fields().contains("encoding") && field(spec, "encoding").string_value() == "hash")
                header.encoding = CategoricalEncoding::Hash;
            if (spec.fields().contains("vocabulary"))
                for (const auto& v : field(spec, "vocabulary").list_value().values())
                    header.vocabulary.push_back(v.string_value());
        }
        result.headers.push_back(std::move(header));
    }
    return result;
}

std::string from_hex(const std::string& hex) {
    std::string result;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        result.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return result;
}

FeatureSchema categorical_schema(std::size_t count = 1) {
    FeatureSchema result{.version = "test-v1"};
    for (std::size_t i = 0; i < count; ++i)
        result.headers.push_back(HeaderFeature{.name = "h" + std::to_string(i), .type = FeatureType::Categorical});
    return result;
}
}

TEST(MlFeatures, SharedIngressFixtures) {
    const auto fixtures = corpus();
    const auto& schemas = field(fixtures, "schemas").struct_value();
    for (const auto& item : field(fixtures, "cases").list_value().values()) {
        const auto& test = item.struct_value();
        SCOPED_TRACE(field(test, "id").string_value());
        FeatureExtractor extractor(schema(field(schemas, field(test, "schema").string_value().c_str()).struct_value()));
        std::unordered_map<std::string, std::string> headers;
        for (const auto& [key, value] : field(test, "headers").struct_value().fields()) headers[key] = value.string_value();
        if (test.fields().contains("raw_hex"))
            for (const auto& [key, value] : field(test, "raw_hex").struct_value().fields()) headers[key] = from_hex(value.string_value());
        const auto size = std::stoull(field(test, "payload_size_bytes").string_value());
        const auto extracted = extractor.extract(size, headers);
        ASSERT_TRUE(extracted.features);
        EXPECT_EQ(extracted.validity, FeatureValidity::Valid);
        EXPECT_EQ(extracted.features->payload_size_bytes, size);
        const auto json = snapshot_json(*extracted.features);
        Object actual;
        ASSERT_TRUE(google::protobuf::util::JsonStringToMessage(json, &actual).ok());
        EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(actual, field(test, "expected").struct_value()));
        if (test.fields().contains("json")) { EXPECT_EQ(json, field(test, "json").string_value()); }
        EXPECT_EQ(json.find("secret"), std::string::npos);
        EXPECT_EQ(json.find("__producer_id"), std::string::npos);
    }
}

TEST(MlFeatures, SharedBinary64Fixtures) {
    const auto fixtures = corpus();
    FeatureExtractor extractor(schema(field(field(fixtures, "schemas").struct_value(), "numbers").struct_value()));
    for (const auto& item : field(fixtures, "numeric_cases").list_value().values()) {
        const auto& test = item.struct_value();
        const auto raw = field(test, "input").string_value();
        SCOPED_TRACE(raw);
        const auto extracted = extractor.extract(0, {{"n", raw}});
        ASSERT_TRUE(extracted.features);
        const auto& snapshot = *extracted.features;
        if (test.fields().contains("reason")) {
            EXPECT_TRUE(std::holds_alternative<std::monostate>(snapshot.headers.at("n")));
            EXPECT_EQ(to_string(snapshot.missing_reasons.at("n")), field(test, "reason").string_value());
        } else {
            ASSERT_TRUE(std::holds_alternative<double>(snapshot.headers.at("n")));
            const double number = std::get<double>(snapshot.headers.at("n"));
            EXPECT_EQ(std::bit_cast<uint64_t>(number), std::stoull(field(test, "bits").string_value(), nullptr, 16));
            EXPECT_TRUE(snapshot.missing_reasons.empty());
            EXPECT_EQ(snapshot_json(snapshot), "{\"payload_size_bytes\":\"0\",\"headers\":{\"n\":" +
                field(test, "json_number").string_value() + "},\"missing_reasons\":{}}");
        }
    }
}

TEST(MlFeatures, SchemaValidationAndOwnership) {
    auto spec = categorical_schema();
    spec.headers[0].vocabulary = {"known", ""};
    FeatureExtractor extractor(spec);
    spec.version = "changed";
    spec.headers[0].name = "changed";
    EXPECT_EQ(extractor.schema().version, "test-v1");
    EXPECT_EQ(extractor.schema().headers[0].name, "h0");
    for (const auto& name : {std::string{}, std::string("__producer_id"), std::string(65, 'x'), std::string("\xff")}) {
        spec = categorical_schema();
        spec.headers[0].name = name;
        EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
    }
    for (const auto& version : {std::string{}, std::string(129, 'x'), std::string("\xed\xa0\x80")}) {
        spec = categorical_schema();
        spec.version = version;
        EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
    }
    spec = categorical_schema(17);
    EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
    spec = categorical_schema(2);
    spec.headers[1].name = spec.headers[0].name;
    EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
    spec = categorical_schema();
    spec.headers[0].vocabulary = {"duplicate", "duplicate"};
    EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
    spec.headers[0].vocabulary = {std::string(257, 'x')};
    EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
    spec.headers[0].vocabulary.clear();
    for (int i = 0; i < 1024; ++i) spec.headers[0].vocabulary.push_back(std::to_string(i));
    EXPECT_NO_THROW(FeatureExtractor{spec});
    spec.headers[0].vocabulary.push_back("too-many");
    EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
    spec = {.version = "numeric", .headers = {{.name = "n", .minimum = 2, .maximum = 1}}};
    EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
    spec.headers[0].minimum = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
    spec = categorical_schema();
    spec.headers[0].encoding = CategoricalEncoding::Hash;
    spec.headers[0].vocabulary = {"forbidden"};
    EXPECT_THROW(FeatureExtractor{spec}, std::invalid_argument);
}

TEST(MlFeatures, ExactLimitsAndEscapeExpansion) {
    auto spec = categorical_schema(16);
    spec.headers[0].name = std::string(64, 'k');
    FeatureExtractor extractor(spec);
    std::unordered_map<std::string, std::string> headers;
    for (const auto& h : spec.headers) headers[h.name] = std::string(256, 'x');
    auto result = extractor.extract(UINT64_MAX, headers);
    ASSERT_TRUE(result.features);
    EXPECT_EQ(result.features->payload_size_bytes, UINT64_MAX);
    headers[spec.headers[0].name] = std::string(257, '\xff');
    result = extractor.extract(0, headers);
    ASSERT_TRUE(result.features);
    EXPECT_EQ(result.features->missing_reasons.at(spec.headers[0].name), MissingReason::Oversized);
    for (auto& [key, value] : headers) value = std::string(256, '\0');
    result = extractor.extract(0, headers);
    EXPECT_FALSE(result.features);
    EXPECT_EQ(result.validity, FeatureValidity::FeatureLimit);

    // Seven escaped 192-byte values leave room to exercise the exact 8 KiB boundary.
    FeatureSnapshot boundary;
    for (int i = 0; i < 7; ++i) boundary.headers["h" + std::to_string(i)] = std::string(192, '\0');
    boundary.headers["tail"] = std::string{};
    const auto remaining = kMaxFeatureBytes - snapshot_json(boundary).size();
    ASSERT_LE(remaining, kMaxValueBytes);
    boundary.headers["tail"] = std::string(remaining, 'x');
    EXPECT_EQ(snapshot_json(boundary).size(), kMaxFeatureBytes);
    boundary.headers["tail"] = std::string(remaining + 1, 'x');
    EXPECT_THROW((void)snapshot_json(boundary), std::length_error);
}

TEST(MlFeatures, SharedEncodedSizeAdmissionBoundaries) {
    const auto fixtures = corpus();
    auto spec = categorical_schema(7);
    spec.headers.push_back({.name = "tail", .type = FeatureType::Categorical});
    FeatureExtractor extractor(spec);
    for (const auto& item : field(fixtures, "size_boundaries").list_value().values()) {
        const auto& test = item.struct_value();
        std::unordered_map<std::string, std::string> headers;
        for (int i = 0; i < 7; ++i) headers["h" + std::to_string(i)] = std::string(192, '\0');
        headers["tail"] = std::string(static_cast<std::size_t>(field(test, "tail_bytes").number_value()), 'x');
        const auto result = extractor.extract(0, headers);
        const bool accepted = field(test, "accepted").bool_value();
        EXPECT_EQ(result.features.has_value(), accepted);
        if (accepted) {
            ASSERT_TRUE(result.features);
            EXPECT_EQ(snapshot_json(*result.features).size(), field(test, "expected_size").number_value());
        } else {
            EXPECT_EQ(result.validity, FeatureValidity::FeatureLimit);
        }
    }
}

TEST(MlFeatures, SerializerRejectsMalformedSnapshots) {
    FeatureSnapshot snapshot;
    snapshot.headers["h"] = std::monostate{};
    EXPECT_THROW((void)snapshot_json(snapshot), std::invalid_argument);
    snapshot.missing_reasons["h"] = MissingReason::Absent;
    EXPECT_NO_THROW((void)snapshot_json(snapshot));
    snapshot.headers["h"] = std::string("\xff");
    EXPECT_THROW((void)snapshot_json(snapshot), std::invalid_argument);
    snapshot.missing_reasons.clear();
    snapshot.headers["h"] = std::numeric_limits<double>::infinity();
    EXPECT_THROW((void)snapshot_json(snapshot), std::invalid_argument);
}
