#include "config_file.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace harbinger;

namespace {

void apply(const std::string& document, HarbingerConfig& config) {
    ServerSettings server;
    apply_config_document(document, config, server);
}

} // namespace

TEST(ConfigFile, FullDocumentMapsOntoExistingTypes) {
    HarbingerConfig config;
    ServerSettings server;
    apply_config_document(R"({
      "config_version": 1,
      "listen": "127.0.0.1:6000",
      "stats_interval_ms": 2500,
      "broker": {"num_levels": 4, "default_priority": 2, "default_max_retries": 5, "default_ttl_ms": 60000,
                 "max_pull_wait_ms": 1000, "aging": {"threshold_ms": 8000, "interval_ms": 250},
                 "delivery_lease_ms": 20000, "maintenance_batch_size": 64},
      "features": {"schema_version": "orders-v1", "routing_policy_version": "static-v1",
                   "headers": [{"name": "job_type", "type": "categorical", "vocabulary": ["email", "resize"]},
                               {"name": "units", "type": "numeric", "minimum": 0, "maximum": 1000000},
                               {"name": "tenant", "type": "categorical", "encoding": "hash"}]},
      "routing": {"mode": "predictive", "routing_policy_version": "per-key-orders-v1",
                  "key": {"job_header": "job_type", "scope_by_producer": false},
                  "predictor": {"summary": "p75", "min_samples": 30, "spread_quantile": 0.95,
                                "max_spread_ratio": 10, "decay": 0.99, "stale_after_ms": 600000, "max_keys": 512},
                  "snapshot": {"path": "/var/lib/harbinger/predictor.snap", "interval_ms": 30000}}
    })", config, server);
    EXPECT_EQ(server.listen_address, "127.0.0.1:6000");
    EXPECT_EQ(server.stats_interval.count(), 2500);
    EXPECT_EQ(config.num_levels, 4);
    EXPECT_EQ(config.default_priority, 2);
    EXPECT_EQ(config.default_max_retries, 5u);
    EXPECT_EQ(config.default_ttl.count(), 60000);
    ASSERT_TRUE(config.aging);
    EXPECT_EQ(config.aging->threshold.count(), 8000);
    EXPECT_EQ(config.delivery_lease.count(), 20000);
    ASSERT_TRUE(config.ingress_features);
    EXPECT_EQ(config.ingress_features->schema.headers.size(), 3u);
    EXPECT_EQ(config.ingress_features->schema.headers[2].encoding, ml::CategoricalEncoding::Hash);
    ASSERT_TRUE(config.predictive_routing);
    const auto& r = *config.predictive_routing;
    EXPECT_EQ(r.mode, ml::RoutingMode::Predictive);
    EXPECT_EQ(r.routing_policy_version, "per-key-orders-v1");
    EXPECT_FALSE(r.key.scope_by_producer);
    EXPECT_EQ(r.predictor.summary, ml::DurationSummary::P75);
    EXPECT_EQ(r.predictor.min_samples, 30u);
    EXPECT_DOUBLE_EQ(r.predictor.spread_quantile, 0.95);
    EXPECT_EQ(r.predictor.max_keys, 512u);
    EXPECT_EQ(r.snapshot_path, std::filesystem::path("/var/lib/harbinger/predictor.snap"));
    EXPECT_EQ(r.snapshot_interval.count(), 30000);
    const auto description = describe_config(config, server);
    EXPECT_NE(description.find("routing=predictive"), std::string::npos) << description;
    EXPECT_NE(description.find("features=orders-v1"), std::string::npos);
}

TEST(ConfigFile, AbsentSectionsKeepDefaultsAndNullDisables) {
    HarbingerConfig config;
    config.aging = AgingConfig{};
    apply(R"({"config_version": 1})", config);
    EXPECT_TRUE(config.aging);
    EXPECT_FALSE(config.predictive_routing);
    EXPECT_FALSE(config.feedback);
    apply(R"({"config_version": 1, "broker": {"aging": null}, "routing": {"mode": "shadow"}})", config);
    EXPECT_FALSE(config.aging);
    ASSERT_TRUE(config.predictive_routing);
    EXPECT_EQ(config.predictive_routing->mode, ml::RoutingMode::Shadow);
    apply(R"({"config_version": 1, "routing": null})", config);
    EXPECT_FALSE(config.predictive_routing);
}

TEST(ConfigFile, RejectsInvalidDocumentsWithoutPartialApplication) {
    const std::vector<std::string> invalid{
        "",
        "{}",
        R"({"config_version": 2})",
        R"({"config_version": 1, "unknown": 1})",
        R"({"config_version": 1, "broker": {"num_levels": 3, "num_levels": 4}})",
        R"({"config_version": 1, "broker": {"num_levels": "3"}})",
        R"({"config_version": 1, "broker": {"num_levels": 0}})",
        R"({"config_version": 1, "broker": {"delivery_lease_ms": -5}})",
        R"({"config_version": 1, "broker": {"aging": {"threshold_ms": 1, "speed": 2}}})",
        R"({"config_version": 1, "routing": {"mode": "disabled"}})",
        R"({"config_version": 1, "routing": {"mode": "shadow", "predictor": {"decay": 2}}})",
        R"({"config_version": 1, "routing": {"mode": "shadow", "predictor": {"summary": "p99"}}})",
        R"({"config_version": 1, "routing": {"mode": "shadow", "key": {"scope_by_producer": "yes"}}})",
        R"({"config_version": 1, "routing": {"mode": "shadow", "key": {"size_source": "bytes"}}})",
        R"({"config_version": 1, "broker": {"level_weights": [8, 0, 1]}})",
        R"({"config_version": 1, "broker": {"level_weights": [8, 3.5, 1]}})",
        R"({"config_version": 1, "broker": {"level_weights": []}})",
        R"({"config_version": 1, "broker": {"level_weights": "8,3,1"}})",
        R"({"config_version": 1, "broker": {"aging": {"pause_when_behind": "yes"}}})",
        R"({"config_version": 1, "routing": {"mode": "shadow", "key": {"size_header": 5}}})",
        R"({"config_version": 1, "features": {"schema_version": "s", "routing_policy_version": "p",
            "headers": [{"name": "__producer_id", "type": "numeric"}]}})",
        R"({"config_version": 1, "features": {"schema_version": "s", "routing_policy_version": "p",
            "headers": [{"name": "a", "type": "categorical", "encoding": "hash", "vocabulary": ["x"]}]}})",
        R"({"config_version": 1, "feedback": {}})",
        R"({"config_version": 1} trailing)",
    };
    for (const auto& document : invalid) {
        SCOPED_TRACE(document);
        HarbingerConfig config;
        config.num_levels = 3;
        EXPECT_ANY_THROW(apply(document, config));
        EXPECT_EQ(config.num_levels, 3);  // nothing applied
        EXPECT_FALSE(config.predictive_routing);
    }
}

TEST(ConfigFile, SizeBinnedKeysAreOptInAndValidatedByTheBroker) {
    HarbingerConfig config;
    ServerSettings server;
    apply(R"({"config_version": 1, "routing": {"mode": "shadow"}})", config);
    EXPECT_EQ(config.predictive_routing->key.size_source, ml::PredictorKeyPolicy::SizeSource::None);
    apply(R"({"config_version": 1, "routing": {"mode": "shadow", "key": {"size_source": "payload"}}})", config);
    EXPECT_EQ(config.predictive_routing->key.size_source, ml::PredictorKeyPolicy::SizeSource::Payload);
    EXPECT_NE(describe_config(config, server).find("+size(payload)"), std::string::npos);
    apply(R"({"config_version": 1, "routing": {"mode": "predictive",
              "key": {"size_source": "header", "size_header": "pixels"}}})", config);
    EXPECT_EQ(config.predictive_routing->key.size_source, ml::PredictorKeyPolicy::SizeSource::Header);
    EXPECT_EQ(config.predictive_routing->key.size_header, "pixels");
    EXPECT_NE(describe_config(config, server).find("+size(pixels)"), std::string::npos);
    EXPECT_NO_THROW(HarbingerService{config});
    apply(R"({"config_version": 1, "routing": {"mode": "shadow", "key": {"size_source": "header"}}})", config);
    EXPECT_THROW(HarbingerService{config}, std::invalid_argument);  // header source without a header name
}

TEST(ConfigFile, LevelWeightsAndPausingAging) {
    HarbingerConfig config;
    ServerSettings server;
    config.aging = AgingConfig{};
    apply(R"({"config_version": 1, "broker": {"level_weights": [8, 3, 1]}})", config);
    ASSERT_TRUE(config.level_weights);
    EXPECT_EQ(*config.level_weights, (std::vector<uint32_t>{8, 3, 1}));
    EXPECT_FALSE(config.aging->pause_when_behind);  // unset: the broker resolves it (on with weights)
    auto description = describe_config(config, server);
    EXPECT_NE(description.find("level_weights=8/3/1"), std::string::npos) << description;
    EXPECT_NE(description.find("(pausing)"), std::string::npos) << description;
    apply(R"({"config_version": 1, "broker": {"aging": {"pause_when_behind": false}}})", config);
    EXPECT_EQ(config.aging->pause_when_behind, std::optional<bool>{false});
    EXPECT_EQ(describe_config(config, server).find("(pausing)"), std::string::npos);
    EXPECT_NO_THROW(HarbingerService{config});
    apply(R"({"config_version": 1, "broker": {"level_weights": null}})", config);
    EXPECT_FALSE(config.level_weights);
    EXPECT_NE(describe_config(config, server).find("level_weights=off"), std::string::npos);
    apply(R"({"config_version": 1, "broker": {"level_weights": [8, 3]}})", config);
    EXPECT_THROW(HarbingerService{config}, std::invalid_argument);  // two weights for three levels
}

TEST(ConfigFile, FeedbackWithoutFeaturesIsRejectedByTheBroker) {
    HarbingerConfig config;
    apply(R"({"config_version": 1, "feedback": {"directory": "/tmp"}})", config);
    EXPECT_THROW(HarbingerService{config}, std::invalid_argument);
}

TEST(ConfigFile, FileReaderBoundsSizeAndReportsPath) {
    const auto path = std::filesystem::temp_directory_path() / ("harbinger-config-" + std::to_string(::getpid()) + ".json");
    HarbingerConfig config;
    ServerSettings server;
    EXPECT_THROW(apply_config_file(path, config, server), std::invalid_argument);  // missing
    {
        std::ofstream out(path);
        out << std::string(kMaxConfigFileBytes + 1, ' ');
    }
    EXPECT_THROW(apply_config_file(path, config, server), std::invalid_argument);
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({"config_version": 1, "broker": {"bogus": true}})";
    }
    try {
        apply_config_file(path, config, server);
        FAIL() << "expected rejection";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find(path.string()), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("bogus"), std::string::npos);
    }
    std::filesystem::remove(path);
}
