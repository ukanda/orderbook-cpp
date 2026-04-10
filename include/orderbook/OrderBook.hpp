#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

#include "LongIntHashMap.hpp"
#include "PriceLevelTree.hpp"

/**
 * Zero-allocation limit-order book backed by a Red-Black Tree per side.
 *
 * Design:
 *   - Orders and price levels are stored in struct-of-arrays pools pre-allocated
 *     at construction time. No heap allocation occurs on the hot path.
 *   - Orders within a price level form a doubly-linked FIFO queue (price-time priority).
 *   - Price levels per side are organised in a PriceLevelTree (RB-tree), giving
 *     O(log N) insert/erase and O(1) best-price access via a cached pointer.
 *   - Free slots are recycled through intrusive free-lists embedded in the same arrays.
 *   - Two LongIntHashMaps provide O(1) lookup: orderId to slot, price to level slot.
 *
 * Prices and quantities are raw int64_t (integer ticks and shares).
 * Not copyable or movable — the RB-trees hold raw pointers into the pool vectors.
 */
class OrderBook {
public:
    static constexpr int kBid = 0;
    static constexpr int kAsk = 1;

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&)                 = delete;
    OrderBook& operator=(OrderBook&&)      = delete;

    /**
     * @param max_orders maximum number of simultaneously resting orders (both sides)
     * @param max_levels maximum number of distinct price levels per side
     */
    OrderBook(int max_orders, int max_levels)
        : order_ids_(max_orders)
        , order_prices_(max_orders)
        , order_quantities_(max_orders)
        , order_sides_(max_orders)
        , order_next_in_level_(max_orders)
        , order_prev_in_level_(max_orders)
        , order_free_list_(max_orders)
        , order_free_head_(0)
        , order_count_(0)
        , level_prices_(max_levels)
        , level_quantities_(max_levels)
        , level_heads_(max_levels)
        , level_tails_(max_levels)
        , level_order_counts_(max_levels)
        , level_left_children_(max_levels)
        , level_right_children_(max_levels)
        , level_parents_(max_levels)
        , level_colors_(max_levels)
        , level_free_list_(max_levels)
        , level_free_head_(0)
        , best_bid_slot_(kNullSlot)
        , best_ask_slot_(kNullSlot)
        // Bids: descending — min() returns the best (highest) bid price.
        // Asks: ascending  — min() returns the best (lowest)  ask price.
        // In both cases successor() advances to the next worse price level.
        , bid_tree_(std::greater<int64_t>{},
                    level_prices_.data(),
                    level_left_children_.data(),
                    level_right_children_.data(),
                    level_parents_.data(),
                    level_colors_.data())
        , ask_tree_(std::less<int64_t>{},
                    level_prices_.data(),
                    level_left_children_.data(),
                    level_right_children_.data(),
                    level_parents_.data(),
                    level_colors_.data())
        , order_index_(nextPowerOfTwo(max_orders * 2))
        , bid_level_index_(nextPowerOfTwo(max_levels * 2))
        , ask_level_index_(nextPowerOfTwo(max_levels * 2)) {
        for (int i = 0; i < max_orders - 1; ++i) {
            order_free_list_[i] = i + 1;
        }
        order_free_list_[max_orders - 1] = kNullSlot;

        for (int i = 0; i < max_levels - 1; ++i) {
            level_free_list_[i] = i + 1;
        }
        level_free_list_[max_levels - 1] = kNullSlot;
    }

    // --- Public API ---

    /**
     * Submit a limit order. Matches against the opposite side at the given price
     * or better; any unfilled remainder rests in the book.
     *
     * @param order_id  unique identifier — must not already be present in the book
     * @param side      kBid or kAsk
     * @param price     limit price in integer ticks
     * @param quantity  order quantity, must be greater than zero
     * @return quantity filled immediately; zero if the order is fully resting
     */
    int64_t addOrder(int64_t order_id, int side, int64_t price, int64_t quantity) {
        int64_t filled    = (side == kBid) ? matchBid(price, quantity) : matchAsk(price, quantity);
        int64_t remaining = quantity - filled;
        if (remaining > 0) {
            restOrder(order_id, side, price, remaining);
        }
        return filled;
    }

    /**
     * Cancel a resting order by id.
     * @return true if found and cancelled; false if not in the book
     */
    bool cancelOrder(int64_t order_id) {
        int slot = order_index_.get(order_id);
        if (slot == kNullSlot) return false;

        int     side     = order_sides_[slot];
        int64_t price    = order_prices_[slot];
        int64_t quantity = order_quantities_[slot];

        LongIntHashMap& level_index = levelIndexFor(side);
        int level_slot = level_index.get(price);

        int prev_slot = order_prev_in_level_[slot];
        int next_slot = order_next_in_level_[slot];
        if (prev_slot != kNullSlot) {
            order_next_in_level_[prev_slot] = next_slot;
        } else {
            level_heads_[level_slot] = next_slot;
        }
        if (next_slot != kNullSlot) {
            order_prev_in_level_[next_slot] = prev_slot;
        } else {
            level_tails_[level_slot] = prev_slot;
        }

        level_quantities_[level_slot] -= quantity;
        --level_order_counts_[level_slot];

        if (level_order_counts_[level_slot] == 0) {
            removeEmptyLevel(level_slot, side, level_index);
        }

        order_index_.remove(order_id);
        freeOrderSlot(slot);
        return true;
    }

    /**
     * Reduce the resting quantity of an existing order (reduce-only).
     * The order keeps its queue position. Cancels the order if new_quantity is zero.
     *
     * @return true if found and modified;
     *         false if not in the book or new_quantity >= current quantity
     */
    bool reduceOrder(int64_t order_id, int64_t new_quantity) {
        int slot = order_index_.get(order_id);
        if (slot == kNullSlot) return false;
        if (new_quantity <= 0) return cancelOrder(order_id);
        if (new_quantity >= order_quantities_[slot]) return false;

        int level_slot = levelIndexFor(order_sides_[slot]).get(order_prices_[slot]);
        level_quantities_[level_slot] -= (order_quantities_[slot] - new_quantity);
        order_quantities_[slot] = new_quantity;
        return true;
    }

    // --- Query methods (all zero-allocation) ---

    [[nodiscard]] int64_t getBestBidPrice() const noexcept {
        return best_bid_slot_ == kNullSlot ? std::numeric_limits<int64_t>::min()
                                           : level_prices_[best_bid_slot_];
    }

    [[nodiscard]] int64_t getBestAskPrice() const noexcept {
        return best_ask_slot_ == kNullSlot ? std::numeric_limits<int64_t>::max()
                                           : level_prices_[best_ask_slot_];
    }

    [[nodiscard]] int64_t getBestBidQuantity() const noexcept {
        return best_bid_slot_ == kNullSlot ? 0 : level_quantities_[best_bid_slot_];
    }

    [[nodiscard]] int64_t getBestAskQuantity() const noexcept {
        return best_ask_slot_ == kNullSlot ? 0 : level_quantities_[best_ask_slot_];
    }

    [[nodiscard]] bool hasBids() const noexcept { return best_bid_slot_ != kNullSlot; }
    [[nodiscard]] bool hasAsks() const noexcept { return best_ask_slot_ != kNullSlot; }
    [[nodiscard]] int  getOrderCount() const noexcept { return order_count_; }

    [[nodiscard]] bool containsOrder(int64_t order_id) const {
        return order_index_.get(order_id) != kNullSlot;
    }

    [[nodiscard]] int64_t getQuantityAtBidPrice(int64_t price) const {
        int level_slot = bid_level_index_.get(price);
        return level_slot == kNullSlot ? 0 : level_quantities_[level_slot];
    }

    [[nodiscard]] int64_t getQuantityAtAskPrice(int64_t price) const {
        int level_slot = ask_level_index_.get(price);
        return level_slot == kNullSlot ? 0 : level_quantities_[level_slot];
    }

    /** Returns the spread in ticks, or int64_t max if either side is empty. */
    [[nodiscard]] int64_t getSpread() const noexcept {
        if (best_bid_slot_ == kNullSlot || best_ask_slot_ == kNullSlot) {
            return std::numeric_limits<int64_t>::max();
        }
        return level_prices_[best_ask_slot_] - level_prices_[best_bid_slot_];
    }

private:
    static constexpr int kNullSlot = LongIntHashMap::kMissing;

    // --- Matching ---

    /** Match an incoming bid against resting asks (ascending price, lowest first). */
    int64_t matchBid(int64_t bid_price, int64_t quantity) {
        int64_t remaining = quantity;

        while (remaining > 0
               && best_ask_slot_ != kNullSlot
               && level_prices_[best_ask_slot_] <= bid_price) {
            remaining -= drainLevel(best_ask_slot_, remaining);
            if (level_heads_[best_ask_slot_] == kNullSlot) {
                int next_level = ask_tree_.successor(best_ask_slot_);
                ask_level_index_.remove(level_prices_[best_ask_slot_]);
                ask_tree_.erase(best_ask_slot_);
                freeLevelSlot(best_ask_slot_);
                best_ask_slot_ = next_level;
            }
        }

        return quantity - remaining;
    }

    /** Match an incoming ask against resting bids (descending price, highest first). */
    int64_t matchAsk(int64_t ask_price, int64_t quantity) {
        int64_t remaining = quantity;

        while (remaining > 0
               && best_bid_slot_ != kNullSlot
               && level_prices_[best_bid_slot_] >= ask_price) {
            remaining -= drainLevel(best_bid_slot_, remaining);
            if (level_heads_[best_bid_slot_] == kNullSlot) {
                int next_level = bid_tree_.successor(best_bid_slot_);
                bid_level_index_.remove(level_prices_[best_bid_slot_]);
                bid_tree_.erase(best_bid_slot_);
                freeLevelSlot(best_bid_slot_);
                best_bid_slot_ = next_level;
            }
        }

        return quantity - remaining;
    }

    /**
     * Fill up to wanted_quantity from the head of the given level.
     * Fully filled orders are removed and their slots recycled.
     * A partially filled order remains at the head with reduced quantity.
     *
     * @return the quantity actually filled from this level
     */
    int64_t drainLevel(int level_slot, int64_t wanted_quantity) {
        int64_t total_filled = 0;

        while (wanted_quantity > 0 && level_heads_[level_slot] != kNullSlot) {
            int     slot      = level_heads_[level_slot];
            int64_t available = order_quantities_[slot];
            int64_t fill      = std::min(available, wanted_quantity);

            order_quantities_[slot]       -= fill;
            level_quantities_[level_slot] -= fill;
            wanted_quantity               -= fill;
            total_filled                  += fill;

            if (order_quantities_[slot] == 0) {
                int next_slot = order_next_in_level_[slot];
                level_heads_[level_slot] = next_slot;
                if (next_slot == kNullSlot) {
                    level_tails_[level_slot] = kNullSlot;
                } else {
                    order_prev_in_level_[next_slot] = kNullSlot;
                }
                --level_order_counts_[level_slot];
                order_index_.remove(order_ids_[slot]);
                freeOrderSlot(slot);
            } else {
                break; // partial fill — order stays at head with reduced quantity
            }
        }

        return total_filled;
    }

    // --- Resting an order ---

    void restOrder(int64_t order_id, int side, int64_t price, int64_t quantity) {
        int slot = allocOrderSlot();
        order_ids_[slot]           = order_id;
        order_prices_[slot]        = price;
        order_quantities_[slot]    = quantity;
        order_sides_[slot]         = side;
        order_next_in_level_[slot] = kNullSlot;
        order_prev_in_level_[slot] = kNullSlot;
        order_index_.put(order_id, slot);
        ++order_count_;

        LongIntHashMap& level_index = levelIndexFor(side);
        int level_slot = level_index.get(price);

        if (level_slot == kNullSlot) {
            level_slot                     = allocLevelSlot();
            level_prices_[level_slot]      = price;
            level_quantities_[level_slot]  = 0;
            level_heads_[level_slot]       = kNullSlot;
            level_tails_[level_slot]       = kNullSlot;
            level_order_counts_[level_slot] = 0;
            level_index.put(price, level_slot);

            if (side == kBid) {
                bid_tree_.insert(level_slot);
                if (best_bid_slot_ == kNullSlot || price > level_prices_[best_bid_slot_]) {
                    best_bid_slot_ = level_slot;
                }
            } else {
                ask_tree_.insert(level_slot);
                if (best_ask_slot_ == kNullSlot || price < level_prices_[best_ask_slot_]) {
                    best_ask_slot_ = level_slot;
                }
            }
        }

        // Append to the tail of the level's FIFO queue
        int tail_slot = level_tails_[level_slot];
        if (tail_slot == kNullSlot) {
            level_heads_[level_slot] = slot;
        } else {
            order_next_in_level_[tail_slot] = slot;
        }
        order_prev_in_level_[slot]  = tail_slot;
        level_tails_[level_slot]    = slot;
        level_quantities_[level_slot] += quantity;
        ++level_order_counts_[level_slot];
    }

    // --- Level removal ---

    /**
     * Remove an empty level from its RB-tree, update the cached best pointer
     * if needed, and free the level slot.
     */
    void removeEmptyLevel(int level_slot, int side, LongIntHashMap& level_index) {
        if (side == kBid) {
            int next_best = (level_slot == best_bid_slot_)
                          ? bid_tree_.successor(level_slot)
                          : kNullSlot;
            bid_tree_.erase(level_slot);
            level_index.remove(level_prices_[level_slot]);
            if (level_slot == best_bid_slot_) {
                best_bid_slot_ = next_best;
            }
        } else {
            int next_best = (level_slot == best_ask_slot_)
                          ? ask_tree_.successor(level_slot)
                          : kNullSlot;
            ask_tree_.erase(level_slot);
            level_index.remove(level_prices_[level_slot]);
            if (level_slot == best_ask_slot_) {
                best_ask_slot_ = next_best;
            }
        }
        freeLevelSlot(level_slot);
    }

    // --- Pool management ---

    int allocOrderSlot() {
        if (order_free_head_ == kNullSlot) {
            throw std::runtime_error("order pool exhausted");
        }
        int slot         = order_free_head_;
        order_free_head_ = order_free_list_[slot];
        return slot;
    }

    void freeOrderSlot(int slot) {
        order_free_list_[slot] = order_free_head_;
        order_free_head_       = slot;
        --order_count_;
    }

    int allocLevelSlot() {
        if (level_free_head_ == kNullSlot) {
            throw std::runtime_error("level pool exhausted");
        }
        int slot         = level_free_head_;
        level_free_head_ = level_free_list_[slot];
        return slot;
    }

    void freeLevelSlot(int slot) {
        level_free_list_[slot] = level_free_head_;
        level_free_head_       = slot;
    }

    // --- Utility ---

    LongIntHashMap& levelIndexFor(int side) {
        return side == kBid ? bid_level_index_ : ask_level_index_;
    }

    const LongIntHashMap& levelIndexFor(int side) const {
        return side == kBid ? bid_level_index_ : ask_level_index_;
    }

    static int nextPowerOfTwo(int n) {
        int p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    // --- Order pool (struct-of-arrays) ---
    std::vector<int64_t> order_ids_;
    std::vector<int64_t> order_prices_;
    std::vector<int64_t> order_quantities_;
    std::vector<int>     order_sides_;
    std::vector<int>     order_next_in_level_;   // next order toward tail of FIFO queue
    std::vector<int>     order_prev_in_level_;   // prev order toward head of FIFO queue
    std::vector<int>     order_free_list_;
    int                  order_free_head_;
    int                  order_count_;

    // --- Level pool (struct-of-arrays) ---
    // level_left/right/parents/colors are the shared RB-tree node fields.
    std::vector<int64_t> level_prices_;
    std::vector<int64_t> level_quantities_;
    std::vector<int>     level_heads_;            // head (oldest) order slot
    std::vector<int>     level_tails_;            // tail (newest) order slot
    std::vector<int>     level_order_counts_;
    std::vector<int>     level_left_children_;    // RB-tree left child
    std::vector<int>     level_right_children_;   // RB-tree right child
    std::vector<int>     level_parents_;          // RB-tree parent
    std::vector<int>     level_colors_;           // RB-tree color: kRed=0, kBlack=1
    std::vector<int>     level_free_list_;
    int                  level_free_head_;

    // --- Book state ---
    int best_bid_slot_;   // cached level slot with the highest bid price, or kNullSlot
    int best_ask_slot_;   // cached level slot with the lowest  ask price, or kNullSlot

    using BidTree = PriceLevelTree<std::greater<int64_t>>;
    using AskTree = PriceLevelTree<std::less<int64_t>>;

    BidTree bid_tree_;
    AskTree ask_tree_;

    LongIntHashMap order_index_;       // order_id -> order slot
    LongIntHashMap bid_level_index_;   // price    -> level slot (bid side)
    LongIntHashMap ask_level_index_;   // price    -> level slot (ask side)
};
