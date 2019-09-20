/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/**
 * Unit tests for Item Paging / Expiration.
 */

#include "../mock/mock_global_task.h"
#include "../mock/mock_paging_visitor.h"
#include "bgfetcher.h"
#include "checkpoint.h"
#include "ep_time.h"
#include "evp_store_single_threaded_test.h"
#include "item.h"
#include "item_eviction.h"
#include "memory_tracker.h"
#include "test_helpers.h"
#include "tests/mock/mock_synchronous_ep_engine.h"

#include <gtest/gtest.h>
#include <string_utilities.h>
#include <xattr/blob.h>
#include <xattr/utils.h>

/**
 * Test fixture for bucket quota tests. Sets quota (max_size) to 200KB and
 * enables the MemoryTracker.
 *
 * NOTE: All the tests using this (including subclasses) require memory
 * tracking to be enabled.
 */
class STBucketQuotaTest : public STParameterizedBucketTest {
public:
    static void SetUpTestCase() {
        // Setup the MemoryTracker.
        MemoryTracker::getInstance(*get_mock_server_api()->alloc_hooks);
    }

    static void TearDownTestCase() {
        MemoryTracker::destroyInstance();
    }

protected:
    void SetUp() override {
        // Set specific ht_size given we need to control expected memory usage.
        config_string += "ht_size=47;max_size=" + std::to_string(200 * 1024) +
                         ";mem_low_wat=" + std::to_string(120 * 1024) +
                         ";mem_high_wat=" + std::to_string(160 * 1024);
        STParameterizedBucketTest::SetUp();

        // How many nonIO tasks we expect initially
        // - 0 for persistent.
        // - 1 for Ephemeral (EphTombstoneHTCleaner).
        if (std::get<0>(GetParam()) == "ephemeral") {
            ++initialNonIoTasks;
        }

        // Sanity check - need memory tracker to be able to check our memory
        // usage.
        ASSERT_TRUE(MemoryTracker::trackingMemoryAllocations())
            << "Memory tracker not enabled - cannot continue";

        store->setVBucketState(vbid, vbucket_state_active, false);

        // Sanity check - to ensure memory usage doesn't increase without us
        // noticing.
        ASSERT_EQ(47, store->getVBucket(vbid)->ht.getSize())
                << "Expected to have a HashTable of size 47 (mem calculations "
                   "based on this).";
        auto& stats = engine->getEpStats();
        ASSERT_LE(stats.getEstimatedTotalMemoryUsed(), 20 * 1024)
                << "Expected to start with less than 20KB of memory used";
        ASSERT_LT(stats.getEstimatedTotalMemoryUsed(),
                  stats.getMaxDataSize() * 0.5)
                << "Expected to start below 50% of bucket quota";
    }

    ENGINE_ERROR_CODE storeItem(Item& item) {
        uint64_t cas = 0;
        return engine->store(cookie, &item, cas, OPERATION_SET);
    }

    /**
     * Write documents to the bucket until they fail with TMP_FAIL.
     * Note this stores via external API (epstore) so we trigger the
     * memoryCondition() code in the event of ENGINE_ENOMEM.
     *
     * @param vbid vBucket to write items to.
     * @param expiry value for items. 0 == no TTL.
     * @return number of documents written.
     */
    size_t populateUntilTmpFail(uint16_t vbid, rel_time_t ttl = 0) {
        size_t count = 0;
        const std::string value(512, 'x'); // 512B value to use for documents.
        ENGINE_ERROR_CODE result;
        const auto expiry =
                (ttl != 0) ? ep_abs_time(ep_reltime(ttl, cb::NoExpiryLimit))
                           : time_t(0);
        for (result = ENGINE_SUCCESS; result == ENGINE_SUCCESS; count++) {
            auto key = makeStoredDocKey("xxx_" + std::to_string(count));
            auto item = make_item(vbid, key, value, expiry);
            // Set NRU of item to maximum; so will be a candidate for paging out
            // straight away.
            item.setNRUValue(MAX_NRU_VALUE);
            item.setFreqCounterValue(0);
            result = storeItem(item);
        }
        EXPECT_EQ(ENGINE_TMPFAIL, result);
        // Fixup count for last loop iteration.
        --count;

        auto& stats = engine->getEpStats();
        EXPECT_GT(stats.getEstimatedTotalMemoryUsed(),
                  stats.getMaxDataSize() * 0.8)
                << "Expected to exceed 80% of bucket quota after hitting "
                   "TMPFAIL";
        EXPECT_GT(stats.getEstimatedTotalMemoryUsed(), stats.mem_low_wat.load())
                << "Expected to exceed low watermark after hitting TMPFAIL";

        // To ensure the Blobs can actually be removed from memory, they must have
        // a ref-count of 1. This will not be the case if there's any open
        // checkpoints hanging onto Items. Therefore force the creation of a new
        // checkpoint.
        store->getVBucket(vbid)->checkpointManager->createNewCheckpoint();

        // Ensure items are flushed to disk (so we can evict them).
        if (std::get<0>(GetParam()) == "persistent") {
            getEPBucket().flushVBucket(vbid);
        }

        return count;
    }

    void populateUntilAboveHighWaterMark(uint16_t vbid) {
        bool populate = true;
        int count = 0;
        auto& stats = engine->getEpStats();
        while (populate) {
            auto key = makeStoredDocKey("key_" + std::to_string(count++));
            auto item = make_item(vbid, key, {"x", 128}, 0 /*ttl*/);
            // Set NRU of item to maximum; so will be a candidate for paging out
            // straight away.
            item.setNRUValue(MAX_NRU_VALUE);
            EXPECT_EQ(ENGINE_SUCCESS, storeItem(item));
            populate = stats.getEstimatedTotalMemoryUsed() <=
                       stats.mem_high_wat.load();
        }
    }

    /// Count of nonIO tasks we should initially have.
    size_t initialNonIoTasks = 0;
};

/**
 * Test fixture for item pager tests - enables the Item Pager (in addition to
 * what the parent class does).
 */
class STItemPagerTest : public STBucketQuotaTest {
protected:
    void SetUp() override {
        STBucketQuotaTest::SetUp();

        // For Ephemeral fail_new_data buckets we have no item pager, instead
        // the Expiry pager is used.
        if (std::get<1>(GetParam()) == "fail_new_data") {
            initializeExpiryPager();
            ++initialNonIoTasks;
        } else {
            // Everyone else uses the ItemPager.
            scheduleItemPager();
            ++initialNonIoTasks;
            itemPagerScheduled = true;
        }

        // Sanity check - should be no nonIO tasks ready to run,
        // and expected number in futureQ.
        auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
        EXPECT_EQ(0, lpNonioQ.getReadyQueueSize());
        EXPECT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());

        // We shouldn't be able to schedule the Item Pager task yet as it's not
        // ready.
        try {
            SCOPED_TRACE("");
            runNextTask(lpNonioQ, "Paging out items.");
            FAIL() << "Unexpectedly managed to run Item Pager";
        } catch (std::logic_error&) {
        }
    }

    /**
     * Run the pager which is scheduled when the high watermark is reached
     * (memoryCondition). This is either the ItemPager (for buckets where
     * items can be paged out - Persistent or Ephemeral-auto_delete), or
     * the Expiry pager (Ephemeral-fail_new_data).
     * @param online_vb_count How many vBuckets are online (and hence should
     *                        have ItemPager tasks run for each).
     */
    void runHighMemoryPager(size_t online_vb_count = 1) {
        auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
        ASSERT_EQ(0, lpNonioQ.getReadyQueueSize());
        ASSERT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());

        if (itemPagerScheduled) {
            // Item pager consists of two Tasks - the parent ItemPager task,
            // and then a per-vBucket task (via VCVBAdapter) - which there is
            // just one of as we only have one vBucket online.
            runNextTask(lpNonioQ, "Paging out items.");
            ASSERT_EQ(0, lpNonioQ.getReadyQueueSize());
            ASSERT_EQ(initialNonIoTasks + 1, lpNonioQ.getFutureQueueSize());
            for (size_t ii = 0; ii < online_vb_count; ii++) {
                runNextTask(lpNonioQ, "Item pager on vb 0");
            }
        } else {
            runNextTask(lpNonioQ, "Paging expired items.");
            for (size_t ii = 0; ii < online_vb_count; ii++) {
                runNextTask(lpNonioQ, "Expired item remover on vb 0");
            }
        }
        // Once complete, should have the same number of tasks we initially
        // had.
        ASSERT_EQ(0, lpNonioQ.getReadyQueueSize());
        ASSERT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());

        // Ensure any deletes are flushed to disk (so item counts are accurate).
        if (std::get<0>(GetParam()) == "persistent") {
            getEPBucket().flushVBucket(vbid);
        }
    }

    /// Has the item pager been scheduled to run?
    bool itemPagerScheduled = false;
};

// Test that the ItemPager is scheduled when the Server Quota is reached, and
// that items are successfully paged out.
TEST_P(STItemPagerTest, ServerQuotaReached) {

    size_t count = populateUntilTmpFail(vbid);
    ASSERT_GE(count, 50) << "Too few documents stored";

    runHighMemoryPager();

    // For all configurations except ephemeral fail_new_data, memory usage
    // should have dropped.
    auto& stats = engine->getEpStats();
    auto vb = engine->getVBucket(vbid);
    if (std::get<1>(GetParam()) == "fail_new_data") {
        EXPECT_GT(stats.getEstimatedTotalMemoryUsed(), stats.mem_low_wat.load())
                << "Expected still to exceed low watermark after hitting "
                   "TMPFAIL with fail_new_data bucket";
        EXPECT_EQ(count, vb->getNumItems());
    } else {
        EXPECT_LT(stats.getEstimatedTotalMemoryUsed(), stats.mem_low_wat.load())
                << "Expected to be below low watermark after running item "
                   "pager";
        const auto numResidentItems =
                vb->getNumItems() - vb->getNumNonResidentItems();
        EXPECT_LT(numResidentItems, count);
    }
}

TEST_P(STItemPagerTest, HighWaterMarkTriggersPager) {
    // Fill to just over HWM
    populateUntilAboveHighWaterMark(vbid);
    // Success if the pager is now ready
    runHighMemoryPager();
}

// Tests that for the hifi_mfu eviction algorithm we visit replica vbuckets
// first.
TEST_P(STItemPagerTest, ReplicaItemsVisitedFirst) {
    // For the Expiry Pager we do not enforce the visiting of replica buckets
    // first.
    if ((std::get<1>(GetParam()) == "fail_new_data")) {
        return;
    }
    auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];

    const uint16_t activeVB = 0;
    const uint16_t pendingVB = 1;
    const uint16_t replicaVB = 2;
    // Set pendingVB online, initially as active (so we can populate it).
    store->setVBucketState(pendingVB, vbucket_state_active, false);
    // Set replicaVB online, initially as active (so we can populate it).
    store->setVBucketState(replicaVB, vbucket_state_active, false);

    // Add a document to both the active and pending vbucket.
    const std::string value(512, 'x'); // 512B value to use for documents.
    for (int ii = 0; ii < 10; ii++) {
        auto key = makeStoredDocKey("key_" + std::to_string(ii));
        auto activeItem = make_item(activeVB, key, value);
        auto pendingItem = make_item(pendingVB, key, value);
        ASSERT_EQ(ENGINE_SUCCESS, storeItem(activeItem));
        ASSERT_EQ(ENGINE_SUCCESS, storeItem(pendingItem));
    }

    store->setVBucketState(pendingVB, vbucket_state_pending, false);

    auto count = populateUntilTmpFail(replicaVB);
    store->setVBucketState(replicaVB, vbucket_state_replica, false);

    runNextTask(lpNonioQ, "Paging out items.");
    runNextTask(lpNonioQ, "Item pager on vb 0");

    if (std::get<0>(GetParam()) == "ephemeral") {
        // For ephemeral we do not evict from replicas and so they are
        // not visited first.  This means there will be another Item
        // pager task to run.
        runNextTask(lpNonioQ, "Item pager on vb 0");
        // We should have not evicted from replica vbuckets
        EXPECT_EQ(count, store->getVBucket(replicaVB)->getNumItems());
        // We should have evicted from the active/pending vbuckets
        auto activeAndPendingItems =
                store->getVBucket(activeVB)->getNumItems() +
                store->getVBucket(pendingVB)->getNumItems();
        EXPECT_NE(20, activeAndPendingItems);

    } else {
        // We should have evicted from replica vbuckets
        EXPECT_NE(0, store->getVBucket(replicaVB)->getNumNonResidentItems());
        auto evictedActiveAndPendingItems =
                store->getVBucket(activeVB)->getNumNonResidentItems() +
                store->getVBucket(pendingVB)->getNumNonResidentItems();
        // We should not have evicted from active or pending vbuckets
        EXPECT_EQ(0, evictedActiveAndPendingItems);
    }
    ASSERT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());
}

// Test that when the server quota is reached, we delete items which have
// expired before any other items.
TEST_P(STItemPagerTest, ExpiredItemsDeletedFirst) {
    // Test only works for the only 2-bit LRU eviction algorithm
    // @todo Investigate converting the test to work with the new hifi_mfu
    // eviction algorithm.
    if (engine->getConfiguration().getHtEvictionPolicy() == "hifi_mfu") {
        return;
    }

    // Populate bucket with non-expiring items until we reach the low
    // watermark.
    size_t countA = 0;
    const std::string value(512, 'x'); // 512B value to use for documents.
    auto& stats = engine->getEpStats();
    do {
        auto key = makeStoredDocKey("key_" + std::to_string(countA));
        auto item = make_item(vbid, key, value);
        ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));
        countA++;
    } while (stats.getEstimatedTotalMemoryUsed() < stats.mem_low_wat.load());

    ASSERT_GE(countA, 10)
            << "Expected at least 10 items before hitting low watermark";

    // Fill bucket with items with a TTL of 1s until we hit ENOMEM. When
    // we run the pager, we expect these items to be deleted first.
    auto countB = populateUntilTmpFail(vbid, 1);

    ASSERT_GE(countB, 50)
        << "Expected at least 50 documents total before hitting high watermark";

    // Advance time so when the pager runs it will find expired items.
    TimeTraveller billSPrestonEsq(2);

    EXPECT_EQ(countA + countB, store->getVBucket(vbid)->getNumItems());

    runHighMemoryPager();

    // Ensure deletes are flushed to disk (so any temp items removed from
    // HashTable).
    if (std::get<0>(GetParam()) == "persistent") {
        getEPBucket().flushVBucket(vbid);
    }

    // Check which items remain. We should have deleted all of the items with
    // a TTL, as they should have been considered first).

    // Initial documents should still exist. Note we need to use getMetaData
    // here as get() would expire the item on access.
    for (size_t ii = 0; ii < countA; ii++) {
        auto key = makeStoredDocKey("key_" + std::to_string(ii));
        auto result = store->get(key, vbid, cookie, get_options_t());
        EXPECT_EQ(ENGINE_SUCCESS, result.getStatus()) << "For key:" << key;
    }

    // Documents which had a TTL should be deleted. Note it's hard to check
    // the specific keys without triggering an expire-on-access (and hence
    // doing the item pager's job for it). Therefore just check the count of
    // items still existing (should have decreased by the number of items
    // with TTLs) and expiry statistics.
    EXPECT_EQ(countA, store->getVBucket(vbid)->getNumItems());
    EXPECT_EQ(countB, stats.expired_pager);
    EXPECT_EQ(0, stats.expired_access);
    EXPECT_EQ(0, stats.expired_compactor);
}

// Test migrated and mutated from from ep_testsuite_basic so that it's less
// racey
TEST_P(STItemPagerTest, test_memory_limit) {
    // Test only works for the only 2-bit LRU eviction algorithm
    // @todo Investigate converting the test to work with the new hifi_mfu
    // eviction algorithm.
    if (engine->getConfiguration().getHtEvictionPolicy() == "hifi_mfu") {
        return;
    }

    // Now set max_size to be 10MiB
    std::string msg;
    EXPECT_EQ(
            PROTOCOL_BINARY_RESPONSE_SUCCESS,
            engine->setFlushParam(
                    "max_size", std::to_string(10 * 1024 * 1204).c_str(), msg));

    // Store a large document 4MiB
    std::string value(4 * 1024 * 1204, 'a');
    {
        auto item = make_item(
                vbid, {"key", DocNamespace::DefaultCollection}, value);
        // ensure this is eligible for eviction on the first pass of the pager
        item.setNRUValue(MAX_NRU_VALUE);
        ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));
    }

    if (std::get<0>(GetParam()) == "persistent") {
        // flush so the HT item becomes clean
        getEPBucket().flushVBucket(vbid);

        // Now do some steps which will remove the checkpoint, all of these
        // steps are needed
        auto vb = engine->getVBucket(vbid);

        // Force close the current checkpoint
        vb->checkpointManager->createNewCheckpoint();
        // Reflush
        getEPBucket().flushVBucket(vbid);
        bool newCheckpointCreated = false;
        auto removed = vb->checkpointManager->removeClosedUnrefCheckpoints(
                *vb, newCheckpointCreated);
        EXPECT_EQ(1, removed);
    }

    // Now set max_size to be mem_used + 10% (we need some headroom)
    auto& stats = engine->getEpStats();
    EXPECT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS,
              engine->setFlushParam(
                      "max_size",
                      std::to_string(stats.getEstimatedTotalMemoryUsed() * 1.10)
                              .c_str(),
                      msg));

    // The next tests use itemAllocate (as per a real SET)
    EXPECT_EQ(ENGINE_TMPFAIL,
              engine->itemAllocate(nullptr,
                                   {"key2", DocNamespace::DefaultCollection},
                                   value.size(),
                                   0,
                                   0,
                                   0,
                                   0,
                                   vbid));

    // item_pager should be notified and ready to run
    runHighMemoryPager();

    if (std::get<0>(GetParam()) == "persistent") {
        EXPECT_EQ(1, stats.numValueEjects);
    }

    if (std::get<1>(GetParam()) != "fail_new_data") {
        // Enough should of been freed so itemAllocate can succeed
        item* itm = nullptr;
        EXPECT_EQ(
                ENGINE_SUCCESS,
                engine->itemAllocate(&itm,
                                     {"key2", DocNamespace::DefaultCollection},
                                     value.size(),
                                     0,
                                     0,
                                     0,
                                     0,
                                     vbid));
        engine->itemRelease(itm);
    }
}

/**
 * MB-29236: Test that if an item is eligible to be evicted but exceeding the
 * eviction threshold we do not add the maximum value (255) to the
 * ItemEviction histogram.
 */
TEST_P(STItemPagerTest, isEligible) {
    populateUntilTmpFail(vbid);

    EventuallyPersistentEngine* epe =
            ObjectRegistry::onSwitchThread(NULL, true);
    get_options_t options = static_cast<get_options_t>(
            QUEUE_BG_FETCH | HONOR_STATES | TRACK_REFERENCE | DELETE_TEMP |
            HIDE_LOCKED_CAS | TRACK_STATISTICS);

    for (int ii = 0; ii < 10; ii++) {
        auto key = makeStoredDocKey("xxx_0");
        store->get(key, vbid, cookie, options);
        ObjectRegistry::onSwitchThread(epe);
    }
    std::shared_ptr<std::atomic<bool>> available;
    std::atomic<item_pager_phase> phase;
    Configuration& cfg = engine->getConfiguration();
    bool isEphemeral = std::get<0>(GetParam()) == "ephemeral";
    PagingVisitor::EvictionPolicy evictionPolicy =
            (cfg.getHtEvictionPolicy() == "2-bit_lru")
                    ? PagingVisitor::EvictionPolicy::lru2Bit
                    : PagingVisitor::EvictionPolicy::hifi_mfu;
    std::unique_ptr<MockPagingVisitor> pv = std::make_unique<MockPagingVisitor>(
            *engine->getKVBucket(),
            engine->getEpStats(),
            1.0,
            available,
            ITEM_PAGER,
            false,
            0.5,
            VBucketFilter(),
            &phase,
            isEphemeral,
            cfg.getItemEvictionAgePercentage(),
            cfg.getItemEvictionFreqCounterAgeThreshold(),
            evictionPolicy);

    VBucketPtr vb = store->getVBucket(vbid);
    pv->visitBucket(vb);
    auto initialCount = ItemEviction::initialFreqCount;
    EXPECT_NE(initialCount,
              pv->getItemEviction().getThresholds(100.0, 0.0).first);
    EXPECT_NE(255, pv->getItemEviction().getThresholds(100.0, 0.0).first);
}

/**
 * MB-29333:  Test that if a vbucket contains a single document with an
 * execution frequency of ItemEviction::initialFreqCount, the document will
 * be evicted if the paging visitor is run a sufficient number of times.
 */
TEST_P(STItemPagerTest, decayByOne) {
    const std::string value(512, 'x'); // 512B value to use for documents.
    auto key = makeStoredDocKey("xxx_0");
    auto item = make_item(vbid, key, value, time_t(0));
    storeItem(item);

    std::shared_ptr<std::atomic<bool>> available;
    std::atomic<item_pager_phase> phase{ACTIVE_AND_PENDING_ONLY};
    Configuration& cfg = engine->getConfiguration();
    bool isEphemeral = std::get<0>(GetParam()) == "ephemeral";
    PagingVisitor::EvictionPolicy evictionPolicy =
            (cfg.getHtEvictionPolicy() == "2-bit_lru")
                    ? PagingVisitor::EvictionPolicy::lru2Bit
                    : PagingVisitor::EvictionPolicy::hifi_mfu;
    std::unique_ptr<MockPagingVisitor> pv = std::make_unique<MockPagingVisitor>(
            *engine->getKVBucket(),
            engine->getEpStats(),
            10.0,
            available,
            ITEM_PAGER,
            false,
            0.5,
            VBucketFilter(),
            &phase,
            isEphemeral,
            cfg.getItemEvictionAgePercentage(),
            cfg.getItemEvictionFreqCounterAgeThreshold(),
            evictionPolicy);

    pv->setCurrentBucket(engine->getKVBucket()->getVBucket(vbid));
    if (std::get<0>(GetParam()) == "persistent") {
        getEPBucket().flushVBucket(vbid);
    }
    int iterationCount = 0;
    while ((pv->getEjected() == 0) &&
           iterationCount <= ItemEviction::initialFreqCount) {
        pv->setFreqCounterThreshold(0);
        VBucketPtr vb = store->getVBucket(vbid);
        vb->ht.visit(*pv);
        iterationCount++;
    }
    EXPECT_EQ(1, pv->getEjected());
}

/**
 * MB-29333:  Test that if a vbucket contains a single document with an
 * execution frequency of ItemEviction::initialFreqCount, but the document
 * is not eligible for eviction (due to being replica in ephemeral case and
 * not flushed in the persistent case) check that its frequency count is not
 * decremented.
 */
TEST_P(STItemPagerTest, doNotDecayIfCannotEvict) {
    const std::string value(512, 'x'); // 512B value to use for documents.
    auto key = makeStoredDocKey("xxx_0");
    auto item = make_item(vbid, key, value, time_t(0));
    storeItem(item);

    std::shared_ptr<std::atomic<bool>> available;
    std::atomic<item_pager_phase> phase{ACTIVE_AND_PENDING_ONLY};
    Configuration& cfg = engine->getConfiguration();
    bool isEphemeral = std::get<0>(GetParam()) == "ephemeral";
    PagingVisitor::EvictionPolicy evictionPolicy =
            (cfg.getHtEvictionPolicy() == "2-bit_lru")
                    ? PagingVisitor::EvictionPolicy::lru2Bit
                    : PagingVisitor::EvictionPolicy::hifi_mfu;
    std::unique_ptr<MockPagingVisitor> pv = std::make_unique<MockPagingVisitor>(
            *engine->getKVBucket(),
            engine->getEpStats(),
            10.0,
            available,
            ITEM_PAGER,
            false,
            0.5,
            VBucketFilter(),
            &phase,
            isEphemeral,
            cfg.getItemEvictionAgePercentage(),
            cfg.getItemEvictionFreqCounterAgeThreshold(),
            evictionPolicy);

    pv->setCurrentBucket(engine->getKVBucket()->getVBucket(vbid));
    store->setVBucketState(vbid, vbucket_state_replica, false);
    for (int ii = 0; ii <= ItemEviction::initialFreqCount; ii++) {
        pv->setFreqCounterThreshold(0);
        pv->getItemEviction().reset();
        VBucketPtr vb = store->getVBucket(vbid);
        vb->ht.visit(*pv);
    }

    // Now make the document eligible for eviction.
    store->setVBucketState(vbid, vbucket_state_active, false);
    if (std::get<0>(GetParam()) == "persistent") {
        getEPBucket().flushVBucket(vbid);
    }

    // Check still not be able to evict, because the frequency count is still
    // at ItemEviction::initialFreqCount
    pv->setFreqCounterThreshold(0);
    pv->getItemEviction().reset();
    VBucketPtr vb = store->getVBucket(vbid);
    vb->ht.visit(*pv);
    auto initialFreqCount = ItemEviction::initialFreqCount;
    EXPECT_EQ(initialFreqCount,
              pv->getItemEviction().getThresholds(100.0, 0.0).first);
    EXPECT_EQ(0, pv->getEjected());

}

/**
 * Test fixture for Ephemeral-only item pager tests.
 */
class STEphemeralItemPagerTest : public STItemPagerTest {
};

// For Ephemeral buckets, replica items should not be paged out (deleted) -
// as that would cause the replica to have a diverging history from the active.
TEST_P(STEphemeralItemPagerTest, ReplicaNotPaged) {
    const uint16_t active_vb = 0;
    const uint16_t replica_vb = 1;
    // Set vBucket 1 online, initially as active (so we can populate it).
    store->setVBucketState(replica_vb, vbucket_state_active, false);

    auto& stats = engine->getEpStats();
    ASSERT_LE(stats.getEstimatedTotalMemoryUsed(), 40 * 1024)
            << "Expected to start with less than 40KB of memory used";
    ASSERT_LT(stats.getEstimatedTotalMemoryUsed(), stats.mem_low_wat.load())
            << "Expected to start below low watermark";

    // Populate vbid 0 (active) until we reach the low watermark.
    size_t active_count = 0;
    const std::string value(1024, 'x'); // 1KB value to use for documents.
    do {
        auto key = makeStoredDocKey("key_" + std::to_string(active_count));
        auto item = make_item(active_vb, key, value);
        // Set NRU of item to maximum; so will be a candidate for paging out
        // straight away.
        item.setNRUValue(MAX_NRU_VALUE);
        item.setFreqCounterValue(0);
        ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));
        active_count++;
    } while (stats.getEstimatedTotalMemoryUsed() < stats.mem_low_wat.load());

    ASSERT_GE(active_count, 10)
            << "Expected at least 10 active items before hitting low watermark";

    // Populate vbid 1 (replica) until we reach the high watermark.
    size_t replica_count = populateUntilTmpFail(replica_vb);
    ASSERT_GE(replica_count, 10)
        << "Expected at least 10 replica items before hitting high watermark";

    // Flip vb 1 to be a replica (and hence should not be a candidate for
    // any paging out.
    store->setVBucketState(replica_vb, vbucket_state_replica, false);
    //  If ephemeral and not running the expiry Pager then only run for one
    // vbucket (as we are skipping the replica vbucket).
    auto vbCount = ((std::get<0>(GetParam()) == "ephemeral") &&
                    (std::get<1>(GetParam()) != "fail_new_data"))
                           ? 1
                           : 2;
    runHighMemoryPager(vbCount);

    EXPECT_EQ(replica_count, store->getVBucket(replica_vb)->getNumItems())
        << "Replica count should be unchanged after Item Pager";

    // Expected active vb behaviour depends on the full policy:
    if (std::get<1>(GetParam()) == "fail_new_data") {
        EXPECT_GT(stats.getEstimatedTotalMemoryUsed(),
                  stats.mem_high_wat.load())
                << "Expected to be above high watermark after running item "
                   "pager (fail_new_data)";
        EXPECT_EQ(store->getVBucket(active_vb)->getNumItems(), active_count)
                << "Active count should be the same after Item Pager "
                   "(fail_new_data)";
    } else {
        EXPECT_LT(stats.getEstimatedTotalMemoryUsed(), stats.mem_low_wat.load())
                << "Expected to be below low watermark after running item "
                   "pager";
        EXPECT_LT(store->getVBucket(active_vb)->getNumItems(), active_count)
                << "Active count should have decreased after Item Pager";
    }
}

/**
 * Test fixture for expiry pager tests - enables the Expiry Pager (in addition
 * to what the parent class does).
 */
class STExpiryPagerTest : public STBucketQuotaTest {
protected:
    void SetUp() override {
        STBucketQuotaTest::SetUp();

        // Setup expiry pager - this adds one to the number of nonIO tasks
        initializeExpiryPager();
        ++initialNonIoTasks;

        // Sanity check - should be no nonIO tasks ready to run, and initial
        // count in futureQ.
        auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
        EXPECT_EQ(0, lpNonioQ.getReadyQueueSize());
        EXPECT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());
    }

    void wakeUpExpiryPager() {
        store->disableExpiryPager();
        store->enableExpiryPager();
        // Expiry pager consists of two Tasks - the parent ExpiryPager task,
        // and then a per-vBucket task (via VCVBAdapter) - which there is
        // just one of as we only have one vBucket online.
        // Trigger expiry pager - note the main task just spawns individual
        // tasks per vBucket - we also need to execute one of them.
        auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
        runNextTask(lpNonioQ, "Paging expired items.");
        EXPECT_EQ(0, lpNonioQ.getReadyQueueSize());
        EXPECT_EQ(initialNonIoTasks + 1, lpNonioQ.getFutureQueueSize());
        runNextTask(lpNonioQ, "Expired item remover on vb 0");
        EXPECT_EQ(0, lpNonioQ.getReadyQueueSize());
        EXPECT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());
    }

    void expiredItemsDeleted();
};

void STExpiryPagerTest::expiredItemsDeleted() {
    // Populate bucket with three documents - one with no expiry, one with an
    // expiry in 10 seconds, and one with an expiry in 20 seconds.
    std::string value = createXattrValue("body");
    for (size_t ii = 0; ii < 3; ii++) {
        auto key = makeStoredDocKey("key_" + std::to_string(ii));
        const uint32_t expiry =
                ii > 0 ? ep_abs_time(ep_current_time() + ii * 10) : 0;
        auto item = make_item(
                vbid,
                key,
                value,
                expiry,
                PROTOCOL_BINARY_DATATYPE_JSON | PROTOCOL_BINARY_DATATYPE_XATTR);
        ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));
    }

    if (std::get<0>(GetParam()) == "persistent") {
        EXPECT_EQ(std::make_pair(false, size_t(3)),
                  getEPBucket().flushVBucket(vbid));
    }

    // Sanity check - should have not hit high watermark (otherwise the
    // item pager will run automatically and aggressively delete items).
    auto& stats = engine->getEpStats();
    EXPECT_LE(stats.getEstimatedTotalMemoryUsed(), stats.getMaxDataSize() * 0.8)
            << "Expected to not have exceeded 80% of bucket quota";

    // Move time forward by 11s, so key_1 should be expired.
    TimeTraveller tedTheodoreLogan(11);

    // Sanity check - should still have all items present in VBucket.
    ASSERT_EQ(3, engine->getVBucket(vbid)->getNumItems());

    wakeUpExpiryPager();
    if (std::get<0>(GetParam()) == "persistent") {
        EXPECT_EQ(std::make_pair(false, size_t(1)),
                  getEPBucket().flushVBucket(vbid));
    }

    EXPECT_EQ(2, engine->getVBucket(vbid)->getNumItems())
        << "Should only have 2 items after running expiry pager";

    // Check our items.
    auto key_0 = makeStoredDocKey("key_0");
    auto result = store->get(key_0, vbid, cookie, get_options_t());
    EXPECT_EQ(ENGINE_SUCCESS, result.getStatus())
        << "Key without TTL should still exist.";

    auto key_1 = makeStoredDocKey("key_1");
    EXPECT_EQ(ENGINE_KEY_ENOENT,
              store->get(key_1, vbid, cookie, get_options_t()).getStatus())
            << "Key with TTL:10 should be removed.";

    auto key_2 = makeStoredDocKey("key_2");
    result = store->get(key_2, vbid, cookie, get_options_t());
    EXPECT_EQ(ENGINE_SUCCESS, result.getStatus())
         << "Key with TTL:20 should still exist.";

    // Move time forward by +10s, so key_2 should also be expired.
    TimeTraveller philConners(10);

    // Sanity check - should still have 2 items present in VBucket.
    ASSERT_EQ(2, engine->getVBucket(vbid)->getNumItems())
        << "Should still have 2 items after time-travelling";

    wakeUpExpiryPager();
    if (std::get<0>(GetParam()) == "persistent") {
        EXPECT_EQ(std::make_pair(false, size_t(1)),
                  getEPBucket().flushVBucket(vbid));
    }

    // Should only be 1 item remaining.
    EXPECT_EQ(1, engine->getVBucket(vbid)->getNumItems());

    // Check our items.
    result = store->get(key_0, vbid, cookie, get_options_t());
    EXPECT_EQ(ENGINE_SUCCESS, result.getStatus())
        << "Key without TTL should still exist.";

    EXPECT_EQ(ENGINE_KEY_ENOENT,
              store->get(key_1, vbid, cookie, get_options_t()).getStatus())
            << "Key with TTL:10 should be removed.";

    EXPECT_EQ(ENGINE_KEY_ENOENT,
              store->get(key_2, vbid, cookie, get_options_t()).getStatus())
            << "Key with TTL:20 should be removed.";
}

// Test that when the expiry pager runs, all expired items are deleted.
TEST_P(STExpiryPagerTest, ExpiredItemsDeleted) {
    expiredItemsDeleted();
}

// Test that when an expired system-xattr document is fetched with getMeta
// it can be successfully expired again
TEST_P(STExpiryPagerTest, MB_25650) {
    expiredItemsDeleted();

    auto vb = store->getVBucket(0);

    auto key_1 = makeStoredDocKey("key_1");
    ItemMetaData metadata;
    uint32_t deleted;
    uint8_t datatype;

    ENGINE_ERROR_CODE err = std::get<0>(GetParam()) == "persistent"
                                    ? ENGINE_EWOULDBLOCK
                                    : ENGINE_SUCCESS;

    // Bring document meta back into memory and run expiry on it
    EXPECT_EQ(err,
              store->getMetaData(
                      key_1, vbid, cookie, metadata, deleted, datatype));
    if (std::get<0>(GetParam()) == "persistent") {
        // Manually run the bgfetch task.
        MockGlobalTask mockTask(engine->getTaskable(),
                                TaskId::MultiBGFetcherTask);
        store->getVBucket(vbid)->getShard()->getBgFetcher()->run(&mockTask);
        EXPECT_EQ(ENGINE_SUCCESS,
                  store->getMetaData(
                          key_1, vbid, cookie, metadata, deleted, datatype));
    }

    // Original bug is that we would segfault running the pager here
    wakeUpExpiryPager();

    get_options_t options =
            static_cast<get_options_t>(QUEUE_BG_FETCH | GET_DELETED_VALUE);
    EXPECT_EQ(err, store->get(key_1, vbid, cookie, options).getStatus())
            << "Key with TTL:10 should be removed.";

    // Verify that the xattr body still exists.
    if (std::get<0>(GetParam()) == "persistent") {
        // Manually run the bgfetch task.
        MockGlobalTask mockTask(engine->getTaskable(),
                                TaskId::MultiBGFetcherTask);
        store->getVBucket(vbid)->getShard()->getBgFetcher()->run(&mockTask);
    }
    auto item = store->get(key_1, vbid, cookie, GET_DELETED_VALUE);

    ASSERT_EQ(ENGINE_SUCCESS, item.getStatus());
    EXPECT_TRUE(mcbp::datatype::is_xattr(item.item->getDataType()));
    ASSERT_NE(0, item.item->getNBytes());
    cb::xattr::Blob blob(
            {const_cast<char*>(item.item->getData()), item.item->getNBytes()},
            false);

    EXPECT_EQ(0, blob.get("user").size());
    EXPECT_EQ(0, blob.get("meta").size());
    ASSERT_NE(0, blob.get("_sync").size());
    EXPECT_STREQ("{\"cas\":\"0xdeadbeefcafefeed\"}",
                 reinterpret_cast<char*>(blob.get("_sync").data()));
}

// Test that when an expired system-xattr document is fetched with getMeta
// deleteWithMeta can be successfully invoked
TEST_P(STExpiryPagerTest, MB_25671) {
    expiredItemsDeleted();
    auto vb = store->getVBucket(vbid);

    // key_1 has been expired
    auto key_1 = makeStoredDocKey("key_1");
    ItemMetaData metadata;
    uint32_t deleted = 0;
    uint8_t datatype = 0;

    // ephemeral will succeed as there is nothing to fetch.
    ENGINE_ERROR_CODE err = std::get<0>(GetParam()) == "persistent"
                                    ? ENGINE_EWOULDBLOCK
                                    : ENGINE_SUCCESS;

    // Bring the deleted key back with a getMeta call
    EXPECT_EQ(err,
              store->getMetaData(
                      key_1, vbid, cookie, metadata, deleted, datatype));
    if (std::get<0>(GetParam()) == "persistent") {
        runBGFetcherTask();
        EXPECT_EQ(ENGINE_SUCCESS,
                  store->getMetaData(
                          key_1, vbid, cookie, metadata, deleted, datatype));
    }

    uint64_t cas = -1;
    metadata.flags = 0xf00f0088;
    metadata.cas = 0xbeeff00dcafe1234ull;
    metadata.revSeqno = 0xdad;
    metadata.exptime = 0xfeedface;
    PermittedVBStates vbstates(vbucket_state_active);
    auto deleteWithMeta = std::bind(&KVBucketIface::deleteWithMeta,
                                    store,
                                    key_1,
                                    cas,
                                    nullptr,
                                    vbid,
                                    cookie,
                                    vbstates,
                                    CheckConflicts::No,
                                    metadata,
                                    false,
                                    GenerateBySeqno::No,
                                    GenerateCas::No,
                                    0,
                                    nullptr,
                                    false);
    // Prior to the MB fix - this would crash.
    EXPECT_EQ(err, deleteWithMeta());

    get_options_t options =
            static_cast<get_options_t>(QUEUE_BG_FETCH | GET_DELETED_VALUE);
    if (std::get<0>(GetParam()) == "persistent") {
        runBGFetcherTask();
        EXPECT_EQ(ENGINE_SUCCESS, deleteWithMeta());
    }

    auto item = store->get(key_1, vbid, cookie, options);
    ASSERT_EQ(ENGINE_SUCCESS, item.getStatus());
    EXPECT_TRUE(item.item->isDeleted()) << "Not deleted " << *item.item;
    ASSERT_NE(0, item.item->getNBytes()) << "No value " << *item.item;

    cb::xattr::Blob blob(
            {const_cast<char*>(item.item->getData()), item.item->getNBytes()},
            false);

    EXPECT_EQ(0, blob.get("user").size());
    EXPECT_EQ(0, blob.get("meta").size());
    ASSERT_NE(0, blob.get("_sync").size());
    EXPECT_STREQ("{\"cas\":\"0xdeadbeefcafefeed\"}",
                 reinterpret_cast<char*>(blob.get("_sync").data()));
    EXPECT_EQ(metadata.flags, item.item->getFlags());
    EXPECT_EQ(metadata.exptime, item.item->getExptime());
    EXPECT_EQ(metadata.cas, item.item->getCas());
    EXPECT_EQ(metadata.revSeqno, item.item->getRevSeqno());
}

/// Subclass for expiry tests only applicable to persistent buckets.
class STPersistentExpiryPagerTest : public STExpiryPagerTest {};

// Test that when a xattr value is ejected, we can still expire it. Previous
// to the fix we crash because the item has no value in memory
TEST_P(STPersistentExpiryPagerTest, MB_25931) {
    std::string value = createXattrValue("body");
    auto key = makeStoredDocKey("key_1");
    auto item = make_item(
            vbid,
            key,
            value,
            ep_abs_time(ep_current_time() + 10),
            PROTOCOL_BINARY_DATATYPE_JSON | PROTOCOL_BINARY_DATATYPE_XATTR);
    ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));

    EXPECT_EQ(std::make_pair(false, size_t(1)),
              getEPBucket().flushVBucket(vbid));

    const char* msg;
    EXPECT_EQ(ENGINE_SUCCESS, store->evictKey(key, vbid, &msg));
    EXPECT_STREQ("Ejected.", msg);

    // Manually run the bgfetch task.
    MockGlobalTask mockTask(engine->getTaskable(), TaskId::MultiBGFetcherTask);
    store->getVBucket(vbid)->getShard()->getBgFetcher()->run(&mockTask);

    TimeTraveller docBrown(15);

    wakeUpExpiryPager();

    EXPECT_EQ(std::make_pair(false, size_t(1)),
              getEPBucket().flushVBucket(vbid));
}

// Test that expiring a non-resident item works (and item counts are correct).
TEST_P(STPersistentExpiryPagerTest, MB_25991_ExpiryNonResident) {
    // Populate bucket with a TTL'd document, and then evict that document.
    auto key = makeStoredDocKey("key");
    auto expiry = ep_abs_time(ep_current_time() + 5);
    auto item = make_item(vbid, key, "value", expiry);
    ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));

    if (std::get<0>(GetParam()) == "persistent") {
        EXPECT_EQ(std::make_pair(false, size_t(1)),
                  getEPBucket().flushVBucket(vbid));
    }

    // Sanity check - should have not hit high watermark (otherwise the
    // item pager will run automatically and aggressively delete items).
    auto& stats = engine->getEpStats();
    EXPECT_LE(stats.getEstimatedTotalMemoryUsed(), stats.getMaxDataSize() * 0.8)
            << "Expected to not have exceeded 80% of bucket quota";

    // Evict key so it is no longer resident.
    evict_key(vbid, key);

    // Move time forward by 11s, so key should be expired.
    TimeTraveller tedTheodoreLogan(11);

    // Sanity check - should still have item present (and non-resident)
    // in VBucket.
    ASSERT_EQ(1, engine->getVBucket(vbid)->getNumItems());
    ASSERT_EQ(1, engine->getVBucket(vbid)->getNumNonResidentItems());

    wakeUpExpiryPager();
    if (std::get<0>(GetParam()) == "persistent") {
        EXPECT_EQ(std::make_pair(false, size_t(1)),
                  getEPBucket().flushVBucket(vbid));
    }

    EXPECT_EQ(0, engine->getVBucket(vbid)->getNumItems())
            << "Should have 0 items after running expiry pager";
    EXPECT_EQ(0, engine->getVBucket(vbid)->getNumNonResidentItems())
            << "Should have 0 non-resident items after running expiry pager";

    // Check our item - should not exist.
    auto result = store->get(key, vbid, cookie, get_options_t());
    EXPECT_EQ(ENGINE_KEY_ENOENT, result.getStatus());
}

// Test that if the eviction policy changes we re-initialise the item pager
// phase to the correct value.
TEST_P(STItemPagerTest, phaseWhenPolicyChange) {
    ASSERT_EQ("hifi_mfu", engine->getConfiguration().getHtEvictionPolicy());
    ItemPager it(*engine, engine->getEpStats());
    if (std::get<0>(GetParam()) == "persistent") {
        ASSERT_EQ(REPLICA_ONLY, it.getPhase());
    } else {
        ASSERT_EQ(ACTIVE_AND_PENDING_ONLY, it.getPhase());
    }
    // Change from hifi_mfu policy to 2-bit_lru
    engine->getConfiguration().setHtEvictionPolicy("2-bit_lru");
    it.run();
    // The item pager run method should have re-initialised the phase
    EXPECT_EQ(PAGING_UNREFERENCED, it.getPhase());

    // change from 2-bit_lru policy to hifi_mfu
    engine->getConfiguration().setHtEvictionPolicy("hifi_mfu");
    it.run();
    // The item pager run method should have re-initialised the phase
    if (std::get<0>(GetParam()) == "persistent") {
        EXPECT_EQ(REPLICA_ONLY, it.getPhase());
    } else {
        EXPECT_EQ(ACTIVE_AND_PENDING_ONLY, it.getPhase());
    }
}

class MB_32669 : public STPersistentExpiryPagerTest {
public:
    void SetUp() override {
        config_string += "compression_mode=active;";
        STPersistentExpiryPagerTest::SetUp();
        store->enableItemCompressor();
        initialNonIoTasks++;
    }

    void runItemCompressor() {
        auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
        runNextTask(lpNonioQ, "Item Compressor");
    }
};

// Test that an xattr value which is compressed, evicted and then expired
// doesn't trigger an exception
TEST_P(MB_32669, expire_a_compressed_and_evicted_xattr_document) {
    // 1) Add bucket a TTL'd xattr document
    auto key = makeStoredDocKey("key");
    auto expiry = ep_abs_time(ep_current_time() + 5);
    auto value = createXattrValue(std::string(100, 'a'), true /*sys xattrs*/);
    auto item =
            make_item(vbid, key, value, expiry, PROTOCOL_BINARY_DATATYPE_XATTR);
    ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));

    if (std::get<0>(GetParam()) == "persistent") {
        EXPECT_EQ(std::make_pair(false, size_t(1)),
                  getEPBucket().flushVBucket(vbid));
    }

    // Sanity check - should have not hit high watermark (otherwise the
    // item pager will run automatically and aggressively delete items).
    auto& stats = engine->getEpStats();
    ASSERT_LE(stats.getEstimatedTotalMemoryUsed(), stats.getMaxDataSize() * 0.8)
            << "Expected to not have exceeded 80% of bucket quota";

    // 2) Run the compressor
    runItemCompressor();

    // 2.1) And validate the document is now snappy
    ItemMetaData metadata;
    uint32_t deleted;
    uint8_t datatype;

    EXPECT_EQ(
            ENGINE_SUCCESS,
            store->getMetaData(key, vbid, cookie, metadata, deleted, datatype));
    ASSERT_EQ(PROTOCOL_BINARY_DATATYPE_SNAPPY,
              datatype & PROTOCOL_BINARY_DATATYPE_SNAPPY);

    // 3) Evict key so it is no longer resident.
    evict_key(vbid, key);

    // 4) Move time forward by 11s, so key should be expired.
    TimeTraveller wyldStallyns(11);

    // Sanity check - should still have item present (and non-resident)
    // in VBucket.
    ASSERT_EQ(1, engine->getVBucket(vbid)->getNumItems());
    ASSERT_EQ(1, engine->getVBucket(vbid)->getNumNonResidentItems());

    wakeUpExpiryPager();

    if (std::get<0>(GetParam()) == "persistent") {
        EXPECT_EQ(std::make_pair(false, size_t(1)),
                  getEPBucket().flushVBucket(vbid));
    }

    EXPECT_EQ(0, engine->getVBucket(vbid)->getNumItems())
            << "Should have 0 items after running expiry pager";
    EXPECT_EQ(0, engine->getVBucket(vbid)->getNumNonResidentItems())
            << "Should have 0 non-resident items after running expiry pager";

    // Check our item has been deleted and the xattrs pruned
    get_options_t options = static_cast<get_options_t>(
            QUEUE_BG_FETCH | HONOR_STATES | TRACK_REFERENCE | DELETE_TEMP |
            HIDE_LOCKED_CAS | TRACK_STATISTICS | GET_DELETED_VALUE);
    GetValue gv = store->get(key, vbid, cookie, options);
    EXPECT_EQ(ENGINE_EWOULDBLOCK, gv.getStatus());

    runBGFetcherTask();
    gv = store->get(key, vbid, cookie, options);
    ASSERT_EQ(ENGINE_SUCCESS, gv.getStatus());

    EXPECT_TRUE(gv.item->isDeleted());
    auto get_itm = gv.item.get();
    auto get_data = const_cast<char*>(get_itm->getData());

    cb::char_buffer value_buf{get_data, get_itm->getNBytes()};
    cb::xattr::Blob new_blob(value_buf, false);

    // expect sys attributes to remain
    const std::string& cas_str{"{\"cas\":\"0xdeadbeefcafefeed\"}"};
    const std::string& sync_str = to_string(new_blob.get("_sync"));

    EXPECT_EQ(cas_str, sync_str) << "Unexpected system xattrs";
    EXPECT_TRUE(new_blob.get("user").empty())
            << "The user attribute should be gone";
    EXPECT_TRUE(new_blob.get("meta").empty())
            << "The meta attribute should be gone";
}

// Just for alice branch specialise this one test so we can run in the desired
// mode - later branches have full/value variants which will be utilised
class MB_36087 : public STPersistentExpiryPagerTest {};

// Test for MB-36087 - simply check that an evicted xattr item doesn't crash
// when a winning del-with-meta arrives.
TEST_P(MB_36087, DelWithMeta_EvictedKey) {
    ASSERT_TRUE(persistent());
    std::string value = createXattrValue("body");
    auto key = makeStoredDocKey("k1");
    auto item = make_item(
            vbid,
            key,
            value,
            0,
            PROTOCOL_BINARY_DATATYPE_JSON | PROTOCOL_BINARY_DATATYPE_XATTR);
    ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));

    getEPBucket().flushVBucket(vbid);

    // 1) Store k1
    auto vb = store->getVBucket(vbid);

    // 2) Evict k1
    evict_key(vbid, key);

    // 3) A winning delWithMeta - system must bgFetch and not crash...
    ItemMetaData metadata;

    uint64_t cas = -1;
    metadata.flags = 0xf00f0088;
    metadata.cas = 0xbeeff00dcafe1234ull;
    metadata.revSeqno = 0xdad;
    metadata.exptime = 0xfeedface;
    PermittedVBStates vbstates(vbucket_state_active);
    auto deleteWithMeta = std::bind(&KVBucketIface::deleteWithMeta,
                                    store,
                                    key,
                                    cas,
                                    nullptr,
                                    vbid,
                                    cookie,
                                    vbstates,
                                    CheckConflicts::Yes,
                                    metadata,
                                    false,
                                    GenerateBySeqno::Yes,
                                    GenerateCas::No,
                                    0,
                                    nullptr,
                                    false);
    // A bgfetch is required for full or value eviction because we need the
    // xattr value
    EXPECT_EQ(ENGINE_EWOULDBLOCK, deleteWithMeta());
    runBGFetcherTask();

    // Full eviction first did a meta-fetch, now has todo a full fetch
    auto err = std::get<1>(GetParam()) == "full_eviction" ? ENGINE_EWOULDBLOCK
                                                          : ENGINE_SUCCESS;
    EXPECT_EQ(err, deleteWithMeta());

    if (std::get<1>(GetParam()) == "full_eviction") {
        runBGFetcherTask();
        EXPECT_EQ(ENGINE_SUCCESS, deleteWithMeta());
    }

    get_options_t options =
            static_cast<get_options_t>(QUEUE_BG_FETCH | GET_DELETED_VALUE);
    auto gv = store->get(key, vbid, cookie, options);
    ASSERT_EQ(ENGINE_SUCCESS, gv.getStatus());
    EXPECT_TRUE(gv.item->isDeleted()) << "Not deleted " << *gv.item;
    ASSERT_NE(0, gv.item->getNBytes()) << "No value " << *gv.item;

    cb::xattr::Blob blob(
            {const_cast<char*>(gv.item->getData()), gv.item->getNBytes()},
            false);

    EXPECT_EQ(0, blob.get("user").size());
    EXPECT_EQ(0, blob.get("meta").size());
    ASSERT_NE(0, blob.get("_sync").size());
    EXPECT_STREQ("{\"cas\":\"0xdeadbeefcafefeed\"}",
                 reinterpret_cast<char*>(blob.get("_sync").data()));
    EXPECT_EQ(metadata.flags, gv.item->getFlags());
    EXPECT_EQ(metadata.exptime, gv.item->getExptime());
    EXPECT_EQ(metadata.cas, gv.item->getCas());
    EXPECT_EQ(metadata.revSeqno, gv.item->getRevSeqno());
}

// TODO: Ideally all of these tests should run with or without jemalloc,
// however we currently rely on jemalloc for accurate memory tracking; and
// hence it is required currently.
#if defined(HAVE_JEMALLOC)

static auto ephConfigValues = ::testing::Values(
        std::make_tuple(std::string("ephemeral"), std::string("auto_delete")),
        std::make_tuple(std::string("ephemeral"),
                        std::string("fail_new_data")));

static auto allConfigValues = ::testing::Values(
        std::make_tuple(std::string("ephemeral"), std::string("auto_delete")),
        std::make_tuple(std::string("ephemeral"), std::string("fail_new_data")),
        std::make_tuple(std::string("persistent"), std::string{}));

static auto persistentConfigValues = ::testing::Values(
        std::make_tuple(std::string("persistent"), std::string{}));

static auto persistentAllEvictionConfigValues = ::testing::Values(
        std::make_tuple(std::string("persistent"), std::string("value_only")),
        std::make_tuple(std::string("persistent"), std::string("full_eviction"))

);

INSTANTIATE_TEST_CASE_P(EphemeralOrPersistent,
                        STItemPagerTest,
                        allConfigValues, );

INSTANTIATE_TEST_CASE_P(EphemeralOrPersistent,
                        STExpiryPagerTest,
                        allConfigValues, );

INSTANTIATE_TEST_CASE_P(Persistent,
                        STPersistentExpiryPagerTest,
                        persistentConfigValues, );

INSTANTIATE_TEST_CASE_P(Persistent, MB_32669, persistentConfigValues, );

INSTANTIATE_TEST_CASE_P(PersistentAllEviction,
                        MB_36087,
                        persistentAllEvictionConfigValues, );

INSTANTIATE_TEST_CASE_P(Ephemeral, STEphemeralItemPagerTest, ephConfigValues, );

#endif
