#include "ml/duration_predictor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace harbinger::ml;

namespace {

PerKeyPredictorConfig base() {
    PerKeyPredictorConfig c;
    c.min_samples = 5;
    c.global_min_samples = 30;
    c.boundary_refresh_every = 10;
    c.decay = 1.0;
    c.global_decay = 1.0;
    return c;
}

/// Three well-separated keys so global tercile boundaries sit between them.
void train_three_tiers(PerKeyPredictor& p, int rounds = 30) {
    for (int i = 0; i < rounds; ++i) {
        p.observe("fast", 2.0);
        p.observe("mid", 20.0);
        p.observe("slow", 200.0);
    }
}

} // namespace

TEST(DurationPredictor, RejectsInvalidConfig) {
    auto c = base();
    c.default_priority = 3;
    EXPECT_THROW(PerKeyPredictor{c}, std::invalid_argument);
    c = base(); c.decay = 0.0;            EXPECT_THROW(PerKeyPredictor{c}, std::invalid_argument);
    c = base(); c.decay = 1.5;            EXPECT_THROW(PerKeyPredictor{c}, std::invalid_argument);
    c = base(); c.min_samples = 0;        EXPECT_THROW(PerKeyPredictor{c}, std::invalid_argument);
    c = base(); c.max_ms = c.min_ms;      EXPECT_THROW(PerKeyPredictor{c}, std::invalid_argument);
    c = base(); c.max_keys = 1;           EXPECT_THROW(PerKeyPredictor{c}, std::invalid_argument);
    c = base(); c.num_levels = 1;         EXPECT_THROW(PerKeyPredictor{c}, std::invalid_argument);  // default_priority 1
    c = base(); c.hysteresis = 1.0;       EXPECT_THROW(PerKeyPredictor{c}, std::invalid_argument);
}

TEST(DurationPredictor, UnseenKeyAndNoSnapshotUseDefault) {
    PerKeyPredictor p{base()};
    auto cold = p.predict("anything");
    EXPECT_EQ(cold.status, PredictionStatus::Unready);
    EXPECT_FALSE(cold.estimate_ms);
    EXPECT_EQ(cold.bucket, 1);
    train_three_tiers(p);
    auto unseen = p.predict("never-observed");
    EXPECT_EQ(unseen.status, PredictionStatus::ColdKey);
    EXPECT_EQ(unseen.bucket, 1);
    EXPECT_EQ(p.predict("").status, PredictionStatus::InvalidKey);
    EXPECT_EQ(p.predict(std::string(kMaxPredictorKeyBytes + 1, 'k')).status, PredictionStatus::InvalidKey);
}

TEST(DurationPredictor, ColdKeyUsesDefaultUntilMinSamples) {
    PerKeyPredictor p{base()};
    train_three_tiers(p);
    for (int i = 0; i < 4; ++i) p.observe("new", 2.0);
    EXPECT_EQ(p.predict("new").status, PredictionStatus::ColdKey);
    EXPECT_EQ(p.predict("new").bucket, 1);
    p.observe("new", 2.0);
    auto warm = p.predict("new");
    EXPECT_EQ(warm.status, PredictionStatus::Predicted);
    EXPECT_EQ(warm.bucket, 0);
}

TEST(DurationPredictor, MapsKeysToTiersByGlobalQuantiles) {
    PerKeyPredictor p{base()};
    train_three_tiers(p);
    EXPECT_EQ(p.predict("fast").bucket, 0);
    EXPECT_EQ(p.predict("mid").bucket, 1);
    EXPECT_EQ(p.predict("slow").bucket, 2);
    const auto snap = p.snapshot();
    ASSERT_TRUE(snap);
    ASSERT_EQ(snap->boundaries_ms.size(), 2u);
    EXPECT_LT(snap->boundaries_ms[0], snap->boundaries_ms[1]);
    EXPECT_GT(snap->boundaries_ms[0], 0.0);
}

TEST(DurationPredictor, WideSpreadKeyFallsBackToDefault) {
    auto c = base();
    c.max_spread_ratio = 4.0;
    PerKeyPredictor p{c};
    train_three_tiers(p);
    for (int i = 0; i < 30; ++i) {
        p.observe("bimodal", 2.0);
        p.observe("bimodal", 2000.0);
    }
    const auto result = p.predict("bimodal");
    EXPECT_EQ(result.status, PredictionStatus::HighSpread);
    EXPECT_FALSE(result.estimate_ms);
    EXPECT_EQ(result.bucket, 1);
}

TEST(DurationPredictor, TierBoundaryEqualityEntersNextTier) {
    const std::vector<double> b{3.0, 10.0};
    EXPECT_EQ(tier_of(b, 2.999), 0);
    EXPECT_EQ(tier_of(b, 3.0), 1);
    EXPECT_EQ(tier_of(b, 9.999), 1);
    EXPECT_EQ(tier_of(b, 10.0), 2);
    EXPECT_EQ(tier_of(b, 1e9), 2);
    EXPECT_EQ(tier_of(b, 0.0), 0);
    EXPECT_EQ(tier_of({}, 5.0), 0);
}

TEST(DurationPredictor, FirstSnapshotPublishesAtGlobalMinSamples) {
    auto c = base();
    c.global_min_samples = 30;
    c.boundary_refresh_every = 1000;
    PerKeyPredictor p{c};
    for (int i = 0; i < 29; ++i) p.observe("k", 1.0 + i);
    EXPECT_FALSE(p.snapshot());
    p.observe("k", 30.0);
    ASSERT_TRUE(p.snapshot());
    EXPECT_EQ(p.snapshot()->sequence, 1u);
}

TEST(DurationPredictor, OneLevelAlwaysTierZero) {
    auto c = base();
    c.num_levels = 1;
    c.default_priority = 0;
    PerKeyPredictor p{c};
    for (int i = 0; i < 10; ++i) p.observe("a", 5.0 + i);
    const auto r = p.predict("a");
    EXPECT_EQ(r.status, PredictionStatus::Predicted);
    EXPECT_EQ(r.bucket, 0);
    EXPECT_TRUE(p.snapshot()->boundaries_ms.empty());
}

TEST(DurationPredictor, TwoLevelsSplitAtGlobalMedian) {
    auto c = base();
    c.num_levels = 2;
    c.default_priority = 0;
    PerKeyPredictor p{c};
    for (int i = 0; i < 40; ++i) { p.observe("short", 3.0); p.observe("long", 300.0); }
    EXPECT_EQ(p.snapshot()->boundaries_ms.size(), 1u);
    EXPECT_EQ(p.predict("short").bucket, 0);
    EXPECT_EQ(p.predict("long").bucket, 1);
}

TEST(DurationPredictor, ZeroRuntimeIsValid) {
    PerKeyPredictor p{base()};
    train_three_tiers(p);
    for (int i = 0; i < 10; ++i) EXPECT_TRUE(p.observe("instant", 0.0));
    const auto r = p.predict("instant");
    ASSERT_EQ(r.status, PredictionStatus::Predicted);
    EXPECT_GE(*r.estimate_ms, 0.0);
    EXPECT_LT(*r.estimate_ms, 0.2);
    EXPECT_EQ(r.bucket, 0);
}

TEST(DurationPredictor, RejectsInvalidObservations) {
    PerKeyPredictor p{base()};
    EXPECT_FALSE(p.observe("k", -1.0));
    EXPECT_FALSE(p.observe("k", std::nan("")));
    EXPECT_FALSE(p.observe("k", INFINITY));
    EXPECT_FALSE(p.observe("", 1.0));
    EXPECT_EQ(p.stats().rejected, 4u);
    EXPECT_EQ(p.stats().keys, 0u);
}

TEST(DurationPredictor, DecayForgetsOldRegime) {
    auto c = base();
    c.decay = 0.9;
    PerKeyPredictor p{c};
    for (int i = 0; i < 100; ++i) p.observe("k", 1000.0);
    for (int i = 0; i < 100; ++i) p.observe("k", 10.0);
    const auto fresh = p.predict("k");
    ASSERT_EQ(fresh.status, PredictionStatus::Predicted);
    EXPECT_LT(*fresh.estimate_ms, 50.0);

    auto flat = base();   // decay 1.0 keeps both regimes, so the median stays high or the key is spread
    PerKeyPredictor q{flat};
    for (int i = 0; i < 100; ++i) q.observe("k", 1000.0);
    for (int i = 0; i < 100; ++i) q.observe("k", 10.0);
    const auto r = q.predict("k");
    EXPECT_TRUE(r.status != PredictionStatus::Predicted || *r.estimate_ms > 50.0);
}

TEST(DurationPredictor, DecayRenormalizationKeepsEstimatesFinite) {
    auto c = base();
    c.decay = 0.5;  // scale doubles every sample, forcing repeated renormalization
    PerKeyPredictor p{c};
    for (int i = 0; i < 2000; ++i) p.observe("k", 7.0);
    const auto r = p.predict("k");
    ASSERT_EQ(r.status, PredictionStatus::Predicted);
    EXPECT_TRUE(std::isfinite(*r.estimate_ms));
    EXPECT_NEAR(*r.estimate_ms, 7.0, 3.0);
}

TEST(DurationPredictor, SummaryStatisticsOrdered) {
    auto make = [](DurationSummary s) {
        auto c = base();
        c.summary = s;
        PerKeyPredictor p{c};
        for (int i = 0; i < 90; ++i) p.observe("k", 10.0);
        for (int i = 0; i < 30; ++i) p.observe("k", 40.0);
        return p.predict("k");
    };
    const auto median = make(DurationSummary::Median);
    const auto p75 = make(DurationSummary::P75);
    const auto mean = make(DurationSummary::Mean);
    ASSERT_EQ(median.status, PredictionStatus::Predicted);
    ASSERT_EQ(p75.status, PredictionStatus::Predicted);
    ASSERT_EQ(mean.status, PredictionStatus::Predicted);
    EXPECT_LE(*median.estimate_ms, *p75.estimate_ms);
    EXPECT_NEAR(*mean.estimate_ms, 17.5, 0.01);
}

TEST(DurationPredictor, BoundaryHysteresisHoldsSmallShifts) {
    auto c = base();
    c.hysteresis = 0.5;
    PerKeyPredictor p{c};
    train_three_tiers(p);
    const auto before = p.snapshot();
    const auto sequence = before->sequence;
    // Small perturbation of the global distribution must not republish boundaries.
    for (int i = 0; i < 40; ++i) p.observe("fast", 2.1);
    EXPECT_EQ(p.snapshot()->sequence, sequence);
    EXPECT_EQ(p.snapshot()->boundaries_ms, before->boundaries_ms);
}

TEST(DurationPredictor, BoundariesRepublishOnLargeShift) {
    auto c = base();
    c.hysteresis = 0.05;
    c.global_decay = 0.99;
    PerKeyPredictor p{c};
    train_three_tiers(p);
    const auto first = p.snapshot()->boundaries_ms;
    for (int i = 0; i < 400; ++i) { p.observe("fast", 40.0); p.observe("mid", 400.0); p.observe("slow", 4000.0); }
    EXPECT_GT(p.snapshot()->boundaries_ms[0], first[0] * 2);
    EXPECT_GE(p.stats().snapshots, 2u);
}

TEST(DurationPredictor, KeyTierHysteresisPreventsFlipping) {
    auto c = base();
    c.hysteresis = 0.3;
    c.global_decay = 1.0;
    PerKeyPredictor p{c};
    train_three_tiers(p);
    const double edge = p.snapshot()->boundaries_ms[1];
    for (int i = 0; i < 40; ++i) p.observe("edge", edge * 0.7);
    const auto low = p.predict("edge").bucket;
    for (int i = 0; i < 8; ++i) p.observe("edge", edge * 1.05);
    // Estimate may move a little past the boundary but inside the band: tier must not flip.
    EXPECT_EQ(p.predict("edge").bucket, low);
}

TEST(DurationPredictor, KeyCapEvictsIdleAndOverflowsOtherwise) {
    auto c = base();
    c.shards = 1;
    c.max_keys = 4;
    c.idle_eviction_observations = 50;
    PerKeyPredictor p{c};
    for (int k = 0; k < 4; ++k) p.observe("key" + std::to_string(k), 5.0);
    EXPECT_EQ(p.stats().keys, 4u);
    // Fresh keys are not idle yet: new key goes to overflow, existing keys survive.
    EXPECT_TRUE(p.observe("intruder", 5.0));
    EXPECT_EQ(p.stats().keys, 4u);
    EXPECT_EQ(p.stats().overflow_observations, 1u);
    EXPECT_EQ(p.stats().evictions, 0u);
    // After enough activity on one key the others become idle and are evicted for newcomers.
    for (int i = 0; i < 60; ++i) p.observe("key0", 5.0);
    EXPECT_TRUE(p.observe("newcomer", 5.0));
    EXPECT_EQ(p.stats().keys, 4u);
    EXPECT_EQ(p.stats().evictions, 1u);
}

TEST(DurationPredictor, AdversarialCardinalityStaysBounded) {
    auto c = base();
    c.shards = 4;
    c.max_keys = 64;
    c.idle_eviction_observations = 1000;
    PerKeyPredictor p{c};
    for (int i = 0; i < 100000; ++i) p.observe("adversary-" + std::to_string(i), 1.0 + (i % 50));
    const auto s = p.stats();
    EXPECT_LE(s.keys, 64u);
    EXPECT_GT(s.overflow_observations + s.evictions, 0u);
    EXPECT_EQ(s.observations, 100000u);
    EXPECT_LT(p.memory_bound_bytes(), 64u * 1024 * 1024);
}

TEST(DurationPredictor, OverflowBucketOnlyPredictsWhenReadyAndTight) {
    auto c = base();
    c.shards = 1;
    c.max_keys = 2;
    c.idle_eviction_observations = 1u << 30;
    PerKeyPredictor p{c};
    train_three_tiers(p);          // fills 2 of 3 keys' worth of capacity; "slow" overflows
    EXPECT_GT(p.stats().overflow_observations, 0u);
    // Overflow holds only slow samples and is tight, so an untracked key is predicted from it.
    const auto r = p.predict("untracked-key");
    EXPECT_EQ(r.status, PredictionStatus::Predicted);
}

TEST(DurationPredictor, ConcurrentPredictObserveAndRefresh) {
    auto c = base();
    c.boundary_refresh_every = 7;
    c.max_keys = 128;
    c.shards = 8;
    PerKeyPredictor p{c};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> predictions{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 5000; ++i) p.observe("k" + std::to_string((i + t) % 40), 1.0 + (i % 100));
        });
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            while (!stop.load()) {
                const auto r = p.predict("k" + std::to_string(t * 7 % 40));
                EXPECT_LT(r.bucket, 3);
                if (r.estimate_ms) { EXPECT_TRUE(std::isfinite(*r.estimate_ms)); }
                predictions.fetch_add(1);
                (void)p.snapshot();
                (void)p.stats();
            }
        });
    for (int t = 0; t < 4; ++t) threads[t].join();
    stop = true;
    for (int t = 4; t < 8; ++t) threads[t].join();
    EXPECT_EQ(p.stats().observations, 20000u);
    EXPECT_GT(predictions.load(), 0u);
}

TEST(DurationPredictor, PredictionIsRepeatableForIdenticalHistory) {
    auto run = [] {
        PerKeyPredictor p{base()};
        train_three_tiers(p);
        return std::make_tuple(p.predict("fast").bucket, p.predict("slow").bucket,
                               *p.predict("mid").estimate_ms, p.snapshot()->boundaries_ms);
    };
    EXPECT_EQ(run(), run());
}
