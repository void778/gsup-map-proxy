#include <gtest/gtest.h>
#include "proxy/TransactionManager.hpp"
#include <thread>

using namespace proxy;

TEST(TransactionManagerTest, AllocateReturnsTid) {
    TransactionManager mgr;
    uint32_t tid = mgr.allocate("262019876543210", 42);
    EXPECT_NE(tid, 0u);
    EXPECT_EQ(mgr.size(), 1u);
}

TEST(TransactionManagerTest, FindExistingTransaction) {
    TransactionManager mgr;
    uint32_t tid = mgr.allocate("262019876543210", 99);
    auto pending = mgr.find(tid);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->mapTransactionId, tid);
    EXPECT_EQ(pending->imsi, "262019876543210");
    EXPECT_EQ(pending->clientContext, 99u);
}

TEST(TransactionManagerTest, FindNonExistentReturnsNullopt) {
    TransactionManager mgr;
    EXPECT_FALSE(mgr.find(9999).has_value());
}

TEST(TransactionManagerTest, CompleteRemovesTransaction) {
    TransactionManager mgr;
    uint32_t tid = mgr.allocate("001010000000001", 1);
    EXPECT_EQ(mgr.size(), 1u);
    mgr.complete(tid);
    EXPECT_EQ(mgr.size(), 0u);
    EXPECT_FALSE(mgr.find(tid).has_value());
}

TEST(TransactionManagerTest, MultipleAllocations) {
    TransactionManager mgr;
    uint32_t t1 = mgr.allocate("IMSI1", 1);
    uint32_t t2 = mgr.allocate("IMSI2", 2);
    uint32_t t3 = mgr.allocate("IMSI3", 3);
    EXPECT_NE(t1, t2);
    EXPECT_NE(t2, t3);
    EXPECT_NE(t1, t3);
    EXPECT_EQ(mgr.size(), 3u);
}

TEST(TransactionManagerTest, AllocateAssignsInvokeId) {
    TransactionManager mgr;
    uint32_t tid = mgr.allocate("IMSI", 0);
    auto p = mgr.find(tid);
    ASSERT_TRUE(p.has_value());
    EXPECT_GE(p->mapInvokeId, 1u);
    EXPECT_LE(p->mapInvokeId, 127u);
}

TEST(TransactionManagerTest, InvokeIdIncrements) {
    TransactionManager mgr;
    uint32_t t1 = mgr.allocate("A", 0);
    uint32_t t2 = mgr.allocate("B", 0);
    auto p1 = mgr.find(t1);
    auto p2 = mgr.find(t2);
    ASSERT_TRUE(p1 && p2);
    EXPECT_NE(p1->mapInvokeId, p2->mapInvokeId);
}

TEST(TransactionManagerTest, ExpireStaleReturnsContexts) {
    // Use a very short timeout
    TransactionManager mgr(std::chrono::seconds{0});
    uint32_t t1 = mgr.allocate("A", 111);
    uint32_t t2 = mgr.allocate("B", 222);
    (void)t1; (void)t2;

    // Everything should be expired immediately
    auto expired = mgr.expireStale();
    EXPECT_EQ(expired.size(), 2u);
    EXPECT_EQ(mgr.size(), 0u);
    // Both contexts should be returned
    std::sort(expired.begin(), expired.end());
    EXPECT_EQ(expired[0], 111u);
    EXPECT_EQ(expired[1], 222u);
}

TEST(TransactionManagerTest, ExpireStaleDoesNotRemoveFresh) {
    TransactionManager mgr(std::chrono::seconds{60});
    mgr.allocate("A", 1);
    mgr.allocate("B", 2);
    auto expired = mgr.expireStale();
    EXPECT_TRUE(expired.empty());
    EXPECT_EQ(mgr.size(), 2u);
}

TEST(TransactionManagerTest, NextInvokeIdWrapsAt128) {
    TransactionManager mgr;
    // Allocate 130 transactions to force invoke ID wrap
    for (int i = 0; i < 130; ++i) {
        uint32_t tid = mgr.allocate("X", static_cast<uint64_t>(i));
        auto p = mgr.find(tid);
        ASSERT_TRUE(p.has_value());
        EXPECT_GE(p->mapInvokeId, 1u);
        EXPECT_LE(p->mapInvokeId, 127u);
    }
}

TEST(TransactionManagerTest, CompleteNonExistentIsNoop) {
    TransactionManager mgr;
    EXPECT_NO_THROW(mgr.complete(9999));
    EXPECT_EQ(mgr.size(), 0u);
}
