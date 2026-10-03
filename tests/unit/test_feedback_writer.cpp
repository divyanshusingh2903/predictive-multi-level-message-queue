#include "feedback_test_support.hpp"

#include <future>
#include <barrier>
#include <set>

using namespace feedback_test;

TEST(FeedbackWriter, ConcurrentPublishersProduceUniqueWholeRecordsAndAccountForEveryAdmission) {
    Directory d; FeedbackWriter w(d.config(), 100);
    std::barrier start{5};
    std::vector<std::future<void>> producers;
    for (int thread = 0; thread < 4; ++thread) {
        producers.push_back(std::async(std::launch::async, [&, thread] {
            auto e = event(); start.arrive_and_wait();
            for (int i = 0; i < 100; ++i) {
                e.message_id = std::to_string(thread) + "-" + std::to_string(i);
                w.publish(e);
            }
        }));
    }
    start.arrive_and_wait(); for (auto& producer : producers) producer.get(); w.close();
    const auto s = w.stats();
    EXPECT_EQ(s.captured, 400u); EXPECT_GT(s.admitted, 0u);
    EXPECT_EQ(s.admitted + dropped(s, FeedbackDrop::Contention), s.captured);
    EXPECT_EQ(s.written, s.admitted); EXPECT_EQ(s.synced, s.written);
    const auto rows = records(d); EXPECT_EQ(rows.size(), s.written);
    std::set<std::string> ids;
    for (const auto& row : rows) EXPECT_TRUE(ids.insert(row.fields().at("event_id").string_value()).second);
}

TEST(FeedbackWriter, ConcurrentCloseAndPublicationSafelyFenceNewAdmissions) {
    Directory d; FeedbackWriter w(d.config(), 100);
    std::barrier start{4};
    auto producer = std::async(std::launch::async, [&] {
        auto e = event(); start.arrive_and_wait();
        for (int i = 0; i < 100; ++i) w.publish(e);
    });
    auto first = std::async(std::launch::async, [&] { start.arrive_and_wait(); w.close(); });
    auto second = std::async(std::launch::async, [&] { start.arrive_and_wait(); w.close(); });
    start.arrive_and_wait(); producer.get(); first.get(); second.get();
    const auto s = w.stats();
    EXPECT_EQ(s.captured, 100u);
    EXPECT_EQ(s.admitted + dropped(s, FeedbackDrop::Closed) + dropped(s, FeedbackDrop::Contention), s.captured);
    EXPECT_EQ(s.pending_records, 0u); EXPECT_EQ(s.written, s.admitted);
    EXPECT_EQ(records(d).size(), s.written);
}

TEST(FeedbackWriter, WritesSyncsSealsAndRestartsWithIndependentIdentity) {
    Directory d;
    std::string first;
    {
        FeedbackWriter w(d.config(), 100);
        first = w.instance_id();
        ASSERT_TRUE(admit(w, event())); w.close(); w.close();
        const auto s = w.stats();
        EXPECT_EQ(s.admitted, 1u); EXPECT_EQ(s.written, 1u); EXPECT_EQ(s.synced, 1u);
        EXPECT_EQ(s.pending_bytes, 0u); EXPECT_EQ(s.pending_records, 0u);
        ASSERT_TRUE(s.sync_age_ms);
        w.publish(event()); EXPECT_EQ(dropped(w.stats(), FeedbackDrop::Closed), 1u);
        EXPECT_THROW((FeedbackWriter{d.config(), 100}), std::runtime_error);
    }
    {
        FeedbackWriter w(d.config(), 100);
        EXPECT_NE(first, w.instance_id()); ASSERT_TRUE(admit(w, event())); w.close();
    }
    const auto rows = records(d); ASSERT_EQ(rows.size(), 2u);
    EXPECT_NE(rows[0].fields().at("broker_instance_id").string_value(), rows[1].fields().at("broker_instance_id").string_value());
}

TEST(FeedbackWriter, RecordAndByteLimitsIncludeBlockedWriterOwnedRecord) {
    for (bool bytes : {false, true}) {
        Directory d; auto c = d.config();
        c.max_event_bytes = 2000; c.buffer_records = bytes ? 10 : 1;
        const auto size = feedback_json(event(), std::string(32, 'a'), 1, 100).size() + 1;
        c.buffer_bytes = bytes ? size * 2 : 4000;
        if (bytes) c.max_event_bytes = size - 1;
        auto storage = std::make_shared<Storage>(); storage->gate();
        auto w = FeedbackWriterTestAccess::create(c, 100, storage);
        ReleaseStorage release{storage};
        ASSERT_TRUE(admit(*w, event())); ASSERT_TRUE(storage->await_entry());
        if (bytes) { ASSERT_TRUE(admit(*w, event())); }
        w->publish(event());
        EXPECT_EQ(dropped(w->stats(), FeedbackDrop::BufferFull), 1u);
        EXPECT_EQ(w->stats().pending_records, bytes ? 2u : 1u);
        EXPECT_EQ(w->stats().pending_bytes, size * (bytes ? 2u : 1u));
        storage->release(); w->close();
        EXPECT_EQ(records(d).size(), bytes ? 2u : 1u);
    }
}

TEST(FeedbackWriter, ContentionEventLimitAndIdentityExhaustionAreObservable) {
    Directory d; FeedbackWriter w(d.config(), 100);
    {
        auto gate = FeedbackWriterTestAccess::gate_admission(w);
        w.publish(event());
    }
    EXPECT_EQ(dropped(w.stats(), FeedbackDrop::Contention), 1u);
    FeedbackWriterTestAccess::exhaust_sequence(w); w.publish(event());
    EXPECT_EQ(dropped(w.stats(), FeedbackDrop::IdentityExhausted), 1u);
    w.close();
    Directory small; auto c = small.config(); c.max_event_bytes = 100;
    FeedbackWriter limited(c, 100); limited.publish(event()); limited.close();
    EXPECT_EQ(dropped(limited.stats(), FeedbackDrop::EventLimit), 1u);
    EXPECT_TRUE(records(small).empty());
}

TEST(FeedbackWriter, ShortWritesAndEintrProduceOneCompleteRecord) {
    Directory d; auto storage = std::make_shared<Storage>();
    storage->short_write = true; storage->interrupt_write = true;
    auto w = FeedbackWriterTestAccess::create(d.config(), 100, storage);
    ASSERT_TRUE(admit(*w, event())); w->close();
    EXPECT_EQ(w->stats().written, 1u); EXPECT_EQ(w->stats().writer_failures, 0u);
    ASSERT_EQ(records(d).size(), 1u);
}

TEST(FeedbackWriter, PartialTailIsPreservedAsSuspectWithoutDuplicatingItsEventOnRecovery) {
    Directory d; auto c = d.config(); c.sync_interval = 5ms;
    auto storage = std::make_shared<Storage>();
    auto w = FeedbackWriterTestAccess::create(c, 100, storage);
    ASSERT_TRUE(admit(*w, event())); ASSERT_TRUE(wait_for([&] { return w->stats().synced == 1; }));
    storage->bytes_before_failure = 13;
    auto failed = event(); failed.message_id = "failed-1"; ASSERT_TRUE(admit(*w, failed));
    ASSERT_TRUE(wait_for([&] { return w->stats().writer_failures > 0; }));
    EXPECT_GE(w->stats().uncertain_records, 1u);
    storage->bytes_before_failure = -1;
    ASSERT_TRUE(wait_for([&] { return w->stats().storage_healthy; }));
    auto recovered = event(); recovered.message_id = "recovered-1"; ASSERT_TRUE(admit(*w, recovered)); w->close();
    const auto rows = records(d); ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].fields().at("message_id").string_value(), "recovered-1");
    int suspects = 0;
    for (const auto& entry : std::filesystem::directory_iterator(d.path)) {
        if (entry.path().extension() != ".suspect") continue;
        ++suspects; std::ifstream input(entry.path()); std::string prefix, tail;
        ASSERT_TRUE(static_cast<bool>(std::getline(input, prefix)));
        EXPECT_EQ(parse(prefix).fields().at("message_id").string_value(), "123456-0");
        std::getline(input, tail); EXPECT_EQ(tail.size(), 13u);
        google::protobuf::Struct invalid;
        EXPECT_FALSE(google::protobuf::util::JsonStringToMessage(tail, &invalid).ok());
    }
    EXPECT_EQ(suspects, 1);
}

TEST(FeedbackWriter, SegmentCapNeverSplitsRecordsAndCountCapIncludesActiveFiles) {
    Directory d; auto c = d.config();
    const auto size = feedback_json(event(), std::string(32, 'a'), 1, 100).size() + 1;
    c.max_event_bytes = size - 1; c.segment_bytes = size * 2;
    c.retention_bytes = size * 20; c.max_segments = 1;
    FeedbackWriter w(c, 100);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(admit(w, event())); ASSERT_TRUE(wait_for([&] { return w.stats().pending_records == 0; }));
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(d.path)) {
            if (!entry.path().filename().string().starts_with("feedback-")) continue;
            ++count; EXPECT_LE(entry.file_size(), c.segment_bytes);
        }
        EXPECT_LE(count, c.max_segments);
    }
    w.close();
    EXPECT_GT(w.stats().retention_segments, 0u);
    ASSERT_EQ(records(d).size(), 2u);
}

TEST(FeedbackWriter, RuntimeStorageFailuresAreBoundedAndRecoverWithoutRetryingAmbiguousRecord) {
    for (const auto fault : {Storage::Fault::Write, Storage::Fault::Sync, Storage::Fault::DirectorySync,
                             Storage::Fault::Rename, Storage::Fault::Close, Storage::Fault::Open}) {
        Directory d; auto c = d.config(); c.sync_interval = 10ms;
        c.max_event_bytes = 2000; c.segment_bytes = 2001; c.retention_bytes = 8000;
        auto storage = std::make_shared<Storage>();
        auto w = FeedbackWriterTestAccess::create(c, 100, storage);
        if (fault == Storage::Fault::Write || fault == Storage::Fault::Sync) storage->fault = fault;
        ASSERT_TRUE(admit(*w, event())); ASSERT_TRUE(wait_for([&] { return w->stats().pending_records == 0; }));
        if (fault != Storage::Fault::Write && fault != Storage::Fault::Sync) {
            storage->fault = fault;
            // Rotation requires another record after filling the current segment.
            w->publish(event()); ASSERT_TRUE(wait_for([&] { return w->stats().pending_records == 0; }));
            w->publish(event());
        }
        ASSERT_TRUE(wait_for([&] { return w->stats().writer_failures > 0; })) << static_cast<int>(fault);
        EXPECT_LE(w->stats().pending_bytes, c.buffer_bytes);
        storage->fault = Storage::Fault::None;
        ASSERT_TRUE(wait_for([&] { return w->stats().storage_healthy; }));
        auto e = event(); e.message_id = "recovered-1";
        ASSERT_TRUE(admit(*w, e)); w->close();
        const auto rows = records(d);
        EXPECT_EQ(std::count_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.fields().at("message_id").string_value() == "recovered-1";
        }), 1);
        EXPECT_EQ(w->stats().pending_records, 0u);
    }
}

TEST(FeedbackWriter, RestartQuarantinesOldTailsAndLeavesUnrelatedFilesUntouched) {
    Directory d;
    const auto tail = d.path / ("feedback-" + std::string(32, 'a') + "-00000000000000000001.active");
    const auto complete = feedback_json(event(), std::string(32, 'a'), 1, 100);
    { std::ofstream output(tail); output << complete << '\n' << "{truncated"; }
    { std::ofstream output(d.path / "unrelated.txt"); output << "keep"; }
    { FeedbackWriter w(d.config(), 100); ASSERT_TRUE(admit(w, event())); w.close(); }
    auto suspect = tail; suspect.replace_extension(".suspect");
    EXPECT_FALSE(std::filesystem::exists(tail)); EXPECT_TRUE(std::filesystem::exists(suspect));
    std::ifstream input(suspect); std::string prefix; std::getline(input, prefix); EXPECT_EQ(prefix, complete);
    EXPECT_TRUE(std::filesystem::exists(d.path / "unrelated.txt"));
    ASSERT_EQ(records(d).size(), 1u);
}

TEST(FeedbackWriter, RetentionAndDeletionFailureKeepDiskBoundedAndReportLoss) {
    Directory d; auto c = d.config();
    c.max_event_bytes = 2000; c.segment_bytes = 2001; c.retention_bytes = 2100; c.max_segments = 2;
    auto storage = std::make_shared<Storage>();
    auto w = FeedbackWriterTestAccess::create(c, 100, storage);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(admit(*w, event())); ASSERT_TRUE(wait_for([&] { return w->stats().pending_records == 0; }));
    }
    EXPECT_GT(w->stats().retention_segments, 0u); EXPECT_GT(w->stats().retention_records, 0u);
    storage->fault = Storage::Fault::Delete;
    for (int i = 0; i < 4; ++i) {
        w->publish(event()); ASSERT_TRUE(wait_for([&] { return w->stats().pending_records == 0; }));
    }
    ASSERT_TRUE(wait_for([&] { return !w->stats().storage_healthy; }));
    std::size_t bytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(d.path))
        if (entry.path().filename().string().starts_with("feedback-")) bytes += entry.file_size();
    EXPECT_LE(bytes, c.retention_bytes);
    storage->fault = Storage::Fault::None; ASSERT_TRUE(wait_for([&] { return w->stats().storage_healthy; }));
    w->close();
}

TEST(FeedbackWriter, AgeRetentionToleratesBackwardClockAndExpiresSealedFilesOnForwardJump) {
    Directory d;
    { FeedbackWriter w(d.config(), 100); ASSERT_TRUE(admit(w, event())); w.close(); }
    auto c = d.config(); c.retention_age = 1h; c.sync_interval = 10ms;
    auto storage = std::make_shared<Storage>(); storage->wall_offset_ms = -7200000;
    auto w = FeedbackWriterTestAccess::create(c, 100, storage);
    EXPECT_EQ(w->stats().retention_segments, 0u);
    storage->wall_offset_ms = 7200000;
    ASSERT_TRUE(wait_for([&] { return w->stats().retention_segments == 1; }));
    w->close();
}

TEST(FeedbackWriter, DrainBudgetDropsQueuedWorkButJoinsBlockedSyscallSafely) {
    Directory d; auto c = d.config(); c.shutdown_drain = 10ms;
    auto storage = std::make_shared<Storage>(); storage->gate();
    auto w = FeedbackWriterTestAccess::create(c, 100, storage);
    ReleaseStorage release{storage};
    ASSERT_TRUE(admit(*w, event())); ASSERT_TRUE(storage->await_entry()); ASSERT_TRUE(admit(*w, event()));
    auto stopping = std::async(std::launch::async, [&] { w->close(); });
    EXPECT_EQ(stopping.wait_for(30ms), std::future_status::timeout);
    storage->release(); stopping.get();
    EXPECT_EQ(w->stats().written, 1u); EXPECT_EQ(dropped(w->stats(), FeedbackDrop::Shutdown), 1u);
    EXPECT_EQ(w->stats().pending_records, 0u);
}

TEST(FeedbackWriter, RejectsInvalidConfigUnsafeSegmentsAndFailedStartupWithoutLeakingOwnership) {
    Directory d;
    for (int i = 0; i < 7; ++i) {
        auto c = d.config();
        switch (i) {
            case 0: c.buffer_records = 0; break;
            case 1: c.buffer_bytes = c.max_event_bytes; break;
            case 2: c.segment_bytes = c.max_event_bytes; break;
            case 3: c.retention_bytes = c.segment_bytes - 1; break;
            case 4: c.max_segments = 0; break;
            case 5: c.sync_interval = 0ms; break;
            case 6: c.retention_age = std::chrono::milliseconds::max(); break;
        }
        EXPECT_THROW((FeedbackWriter{c, 100}), std::invalid_argument);
    }
    auto storage = std::make_shared<Storage>(); storage->fault = Storage::Fault::Open;
    EXPECT_THROW(FeedbackWriterTestAccess::create(d.config(), 100, storage), std::runtime_error);
    { FeedbackWriter w(d.config(), 100); w.close(); }
    const auto unsafe = d.path / ("feedback-" + std::string(32, 'b') + "-00000000000000000001.jsonl");
    std::filesystem::create_symlink(d.path / "outside", unsafe);
    EXPECT_THROW((FeedbackWriter{d.config(), 100}), std::runtime_error);
}
