#include <gtest/gtest.h>
#include <orderbook/OrderBook.hpp>
#include <orderbook/LongIntHashMap.hpp>
#include <orderbook/PriceLevelTree.hpp>

// -----------------------------------------------------------------------
// Fixture
// -----------------------------------------------------------------------

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book{1024, 512};
};

// -----------------------------------------------------------------------
// Basic resting / query
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, EmptyBookHasNoSides) {
    EXPECT_FALSE(book.hasBids());
    EXPECT_FALSE(book.hasAsks());
    EXPECT_EQ(std::numeric_limits<int64_t>::min(), book.getBestBidPrice());
    EXPECT_EQ(std::numeric_limits<int64_t>::max(), book.getBestAskPrice());
    EXPECT_EQ(std::numeric_limits<int64_t>::max(), book.getSpread());
}

TEST_F(OrderBookTest, SingleBidRests) {
    EXPECT_EQ(0, book.addOrder(1, OrderBook::kBid, 100, 10));
    EXPECT_TRUE(book.hasBids());
    EXPECT_EQ(100, book.getBestBidPrice());
    EXPECT_EQ(10,  book.getBestBidQuantity());
    EXPECT_EQ(1,   book.getOrderCount());
    EXPECT_TRUE(book.containsOrder(1));
}

TEST_F(OrderBookTest, SingleAskRests) {
    EXPECT_EQ(0, book.addOrder(1, OrderBook::kAsk, 101, 5));
    EXPECT_TRUE(book.hasAsks());
    EXPECT_EQ(101, book.getBestAskPrice());
    EXPECT_EQ(5,   book.getBestAskQuantity());
}

TEST_F(OrderBookTest, SpreadComputedCorrectly) {
    book.addOrder(1, OrderBook::kBid, 100, 10);
    book.addOrder(2, OrderBook::kAsk, 103, 10);
    EXPECT_EQ(3, book.getSpread());
}

// -----------------------------------------------------------------------
// Price-level ordering
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, BidLevelsSortedDescending) {
    book.addOrder(1, OrderBook::kBid, 100, 2);
    book.addOrder(2, OrderBook::kBid, 105, 3);
    book.addOrder(3, OrderBook::kBid,  98, 4);
    EXPECT_EQ(105, book.getBestBidPrice());
    EXPECT_EQ(3,   book.getQuantityAtBidPrice(105));
    EXPECT_EQ(2,   book.getQuantityAtBidPrice(100));
    EXPECT_EQ(4,   book.getQuantityAtBidPrice(98));
}

TEST_F(OrderBookTest, AskLevelsSortedAscending) {
    book.addOrder(1, OrderBook::kAsk, 103, 2);
    book.addOrder(2, OrderBook::kAsk,  99, 3);
    book.addOrder(3, OrderBook::kAsk, 107, 4);
    EXPECT_EQ(99, book.getBestAskPrice());
    EXPECT_EQ(3,  book.getQuantityAtAskPrice(99));
}

// -----------------------------------------------------------------------
// Full match
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, BidFullyMatchesSingleAsk) {
    book.addOrder(1, OrderBook::kAsk, 100, 10);
    EXPECT_EQ(10, book.addOrder(2, OrderBook::kBid, 100, 10));
    EXPECT_FALSE(book.hasAsks());
    EXPECT_FALSE(book.hasBids());
    EXPECT_EQ(0, book.getOrderCount());
}

TEST_F(OrderBookTest, AskFullyMatchesSingleBid) {
    book.addOrder(1, OrderBook::kBid, 100, 5);
    EXPECT_EQ(5, book.addOrder(2, OrderBook::kAsk, 100, 5));
    EXPECT_FALSE(book.hasBids());
    EXPECT_EQ(0, book.getOrderCount());
}

// -----------------------------------------------------------------------
// Partial match
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, IncomingBidPartiallyFilled_RemainderRests) {
    book.addOrder(1, OrderBook::kAsk, 100, 3);
    EXPECT_EQ(3, book.addOrder(2, OrderBook::kBid, 100, 10));
    EXPECT_FALSE(book.hasAsks());
    EXPECT_TRUE(book.hasBids());
    EXPECT_EQ(7,   book.getBestBidQuantity());
    EXPECT_EQ(100, book.getBestBidPrice());
    EXPECT_EQ(1,   book.getOrderCount());
}

TEST_F(OrderBookTest, IncomingBidPartiallyFillsRestingAsk) {
    book.addOrder(1, OrderBook::kAsk, 100, 10);
    EXPECT_EQ(4, book.addOrder(2, OrderBook::kBid, 100, 4));
    EXPECT_TRUE(book.hasAsks());
    EXPECT_EQ(6, book.getBestAskQuantity());
    EXPECT_EQ(1, book.getOrderCount());
    EXPECT_FALSE(book.containsOrder(2));
}

// -----------------------------------------------------------------------
// Multi-level sweep
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, BidSweepsMultipleAskLevels) {
    book.addOrder(1, OrderBook::kAsk, 100, 5);
    book.addOrder(2, OrderBook::kAsk, 101, 5);
    book.addOrder(3, OrderBook::kAsk, 102, 5);

    EXPECT_EQ(10, book.addOrder(4, OrderBook::kBid, 101, 20));
    EXPECT_TRUE(book.hasAsks());
    EXPECT_EQ(102, book.getBestAskPrice());
    EXPECT_EQ(5,   book.getBestAskQuantity());
}

TEST_F(OrderBookTest, AskSweepsMultipleBidLevels) {
    book.addOrder(1, OrderBook::kBid, 103, 4);
    book.addOrder(2, OrderBook::kBid, 102, 4);
    book.addOrder(3, OrderBook::kBid, 100, 4);

    EXPECT_EQ(8, book.addOrder(4, OrderBook::kAsk, 102, 20));
    EXPECT_TRUE(book.hasBids());
    EXPECT_EQ(100, book.getBestBidPrice());
}

// -----------------------------------------------------------------------
// FIFO within a level
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, FifoWithinLevel) {
    book.addOrder(1, OrderBook::kAsk, 100, 3);
    book.addOrder(2, OrderBook::kAsk, 100, 3);
    book.addOrder(3, OrderBook::kAsk, 100, 3);

    EXPECT_EQ(4, book.addOrder(4, OrderBook::kBid, 100, 4));
    EXPECT_EQ(5, book.getQuantityAtAskPrice(100)); // 2 + 3
    EXPECT_FALSE(book.containsOrder(1));
    EXPECT_TRUE(book.containsOrder(2));
    EXPECT_TRUE(book.containsOrder(3));
}

// -----------------------------------------------------------------------
// Cancel
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, CancelRestingOrder) {
    book.addOrder(1, OrderBook::kBid, 100, 10);
    book.addOrder(2, OrderBook::kBid, 100, 5);
    EXPECT_TRUE(book.cancelOrder(1));
    EXPECT_FALSE(book.containsOrder(1));
    EXPECT_EQ(5, book.getBestBidQuantity());
    EXPECT_EQ(1, book.getOrderCount());
}

TEST_F(OrderBookTest, CancelLastOrderAtLevelRemovesLevel) {
    book.addOrder(1, OrderBook::kBid, 100, 10);
    book.cancelOrder(1);
    EXPECT_FALSE(book.hasBids());
    EXPECT_EQ(0, book.getQuantityAtBidPrice(100));
}

TEST_F(OrderBookTest, CancelNonExistentReturnsFalse) {
    EXPECT_FALSE(book.cancelOrder(999));
}

TEST_F(OrderBookTest, CancelBestBidUpdatesBestPrice) {
    book.addOrder(1, OrderBook::kBid, 105, 1);
    book.addOrder(2, OrderBook::kBid, 100, 1);
    book.cancelOrder(1);
    EXPECT_EQ(100, book.getBestBidPrice());
}

TEST_F(OrderBookTest, CancelBestAskUpdatesBestPrice) {
    book.addOrder(1, OrderBook::kAsk,  99, 1);
    book.addOrder(2, OrderBook::kAsk, 103, 1);
    book.cancelOrder(1);
    EXPECT_EQ(103, book.getBestAskPrice());
}

// -----------------------------------------------------------------------
// Reduce
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, ReduceOrder) {
    book.addOrder(1, OrderBook::kAsk, 100, 10);
    EXPECT_TRUE(book.reduceOrder(1, 6));
    EXPECT_EQ(6, book.getBestAskQuantity());
}

TEST_F(OrderBookTest, ReduceToZeroCancels) {
    book.addOrder(1, OrderBook::kBid, 100, 5);
    EXPECT_TRUE(book.reduceOrder(1, 0));
    EXPECT_FALSE(book.hasBids());
}

TEST_F(OrderBookTest, ReduceWithHigherQuantityIgnored) {
    book.addOrder(1, OrderBook::kBid, 100, 5);
    EXPECT_FALSE(book.reduceOrder(1, 10));
    EXPECT_EQ(5, book.getBestBidQuantity());
}

// -----------------------------------------------------------------------
// Many levels — RB-tree correctness
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, ManyBidLevels_BestBidCorrect) {
    for (int i = 1; i <= 200; ++i) {
        book.addOrder(i, OrderBook::kBid, i * 10LL, 1);
    }
    EXPECT_EQ(200 * 10LL, book.getBestBidPrice());
    EXPECT_EQ(200, book.getOrderCount());
}

TEST_F(OrderBookTest, ManyAskLevels_BestAskCorrect) {
    for (int i = 200; i >= 1; --i) {
        book.addOrder(201 - i, OrderBook::kAsk, i * 10LL, 1);
    }
    EXPECT_EQ(10LL, book.getBestAskPrice());
}

TEST_F(OrderBookTest, ManyLevels_SweepAll) {
    for (int i = 1; i <= 100; ++i) {
        book.addOrder(i, OrderBook::kAsk, i * 10LL, 5);
    }
    int64_t filled = book.addOrder(101, OrderBook::kBid, 100 * 10LL, 100 * 5LL);
    EXPECT_EQ(100 * 5LL, filled);
    EXPECT_FALSE(book.hasAsks());
    EXPECT_FALSE(book.hasBids());
    EXPECT_EQ(0, book.getOrderCount());
}

TEST_F(OrderBookTest, ManyLevels_CancelBestRepeatedly) {
    for (int i = 1; i <= 50; ++i) {
        book.addOrder(i, OrderBook::kBid, i * 10LL, 1);
    }
    for (int i = 50; i >= 1; --i) {
        EXPECT_EQ(i * 10LL, book.getBestBidPrice());
        book.cancelOrder(i);
    }
    EXPECT_FALSE(book.hasBids());
}

// -----------------------------------------------------------------------
// Pool recycling
// -----------------------------------------------------------------------

TEST_F(OrderBookTest, PoolSlotRecycling) {
    for (int i = 0; i < 100; ++i) {
        book.addOrder(i, OrderBook::kBid, 100, 1);
    }
    for (int i = 0; i < 100; ++i) {
        book.cancelOrder(i);
    }
    EXPECT_EQ(0, book.getOrderCount());
    EXPECT_FALSE(book.hasBids());

    for (int i = 200; i < 300; ++i) {
        book.addOrder(i, OrderBook::kAsk, 105, 1);
    }
    EXPECT_EQ(100, book.getOrderCount());
    EXPECT_EQ(105, book.getBestAskPrice());
}

TEST_F(OrderBookTest, MatchingRecyclesSlots) {
    for (int round = 0; round < 50; ++round) {
        book.addOrder(round * 2LL,     OrderBook::kAsk, 100, 5);
        EXPECT_EQ(5, book.addOrder(round * 2LL + 1, OrderBook::kBid, 100, 5));
    }
    EXPECT_EQ(0, book.getOrderCount());
    EXPECT_FALSE(book.hasAsks());
    EXPECT_FALSE(book.hasBids());
}

// -----------------------------------------------------------------------
// LongIntHashMap
// -----------------------------------------------------------------------

TEST(LongIntHashMapTest, PutGetRemove) {
    LongIntHashMap map(16);
    map.put(42LL, 7);
    EXPECT_EQ(7, map.get(42LL));
    EXPECT_EQ(1, map.size());

    map.remove(42LL);
    EXPECT_EQ(LongIntHashMap::kMissing, map.get(42LL));
    EXPECT_EQ(0, map.size());
}

TEST(LongIntHashMapTest, UpdateExistingKey) {
    LongIntHashMap map(16);
    map.put(1LL, 10);
    map.put(1LL, 20);
    EXPECT_EQ(20, map.get(1LL));
    EXPECT_EQ(1,  map.size());
}

TEST(LongIntHashMapTest, TombstonesWorkAfterRemove) {
    LongIntHashMap map(16);
    map.put(1LL, 10);
    map.put(2LL, 20);
    map.remove(1LL);
    map.put(3LL, 30);
    EXPECT_EQ(LongIntHashMap::kMissing, map.get(1LL));
    EXPECT_EQ(20, map.get(2LL));
    EXPECT_EQ(30, map.get(3LL));
}

// -----------------------------------------------------------------------
// PriceLevelTree
// -----------------------------------------------------------------------

TEST(PriceLevelTreeTest, InsertAndMinMax) {
    constexpr int N = 32;
    int64_t prices[N]{};
    int left[N]{}, right[N]{}, parents[N]{}, colors[N]{};

    PriceLevelTree tree(std::less<int64_t>{}, prices, left, right, parents, colors);

    int64_t vals[] = {50, 30, 70, 20, 40};
    for (int i = 0; i < 5; ++i) {
        prices[i] = vals[i];
        tree.insert(i);
    }

    EXPECT_EQ(20, prices[tree.min()]);
    EXPECT_EQ(70, prices[tree.max()]);
}

TEST(PriceLevelTreeTest, SuccessorAndPredecessor) {
    constexpr int N = 32;
    int64_t prices[N]{};
    int left[N]{}, right[N]{}, parents[N]{}, colors[N]{};

    PriceLevelTree tree(std::less<int64_t>{}, prices, left, right, parents, colors);

    for (int i = 0; i < 5; ++i) {
        prices[i] = (i + 1) * 10LL;
        tree.insert(i);
    }

    int min_slot = tree.min();
    EXPECT_EQ(20, prices[tree.successor(min_slot)]);

    int max_slot = tree.max();
    EXPECT_EQ(40, prices[tree.predecessor(max_slot)]);

    EXPECT_EQ(PriceLevelTree<>::kNullSlot, tree.successor(max_slot));
    EXPECT_EQ(PriceLevelTree<>::kNullSlot, tree.predecessor(min_slot));
}

TEST(PriceLevelTreeTest, DeleteAndReinsert) {
    constexpr int N = 32;
    int64_t prices[N]{};
    int left[N]{}, right[N]{}, parents[N]{}, colors[N]{};

    PriceLevelTree tree(std::less<int64_t>{}, prices, left, right, parents, colors);

    for (int i = 0; i < 10; ++i) {
        prices[i] = i * 10LL;
        tree.insert(i);
    }

    for (int i = 0; i < 10; i += 2) {
        tree.erase(i);
    }
    EXPECT_EQ(10,  prices[tree.min()]);
    EXPECT_EQ(90,  prices[tree.max()]);

    for (int i = 0; i < 10; i += 2) {
        tree.insert(i);
    }
    EXPECT_EQ(0,  prices[tree.min()]);
    EXPECT_EQ(90, prices[tree.max()]);
}
