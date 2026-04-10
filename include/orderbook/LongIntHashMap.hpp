#pragma once

#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

/**
 * Zero-allocation open-addressing hash map: int64_t key -> int32_t value.
 *
 * Uses linear probing with Fibonacci hashing and tombstones for deletion.
 * All memory is allocated at construction; no allocations on the hot path.
 *
 * Keys must not equal kEmptyKey (int64_t min). A return value of kMissing
 * means the key was not found.
 */
class LongIntHashMap {
public:
    static constexpr int64_t kEmptyKey  = std::numeric_limits<int64_t>::min();
    static constexpr int64_t kTombstone = std::numeric_limits<int64_t>::min() + 1;
    static constexpr int32_t kMissing   = std::numeric_limits<int32_t>::min();

    explicit LongIntHashMap(int capacity)
        : keys_(capacity, kEmptyKey)
        , values_(capacity)
        , mask_(capacity - 1)
        , size_(0)
        , tombstone_count_(0) {
        if ((capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("capacity must be a power of two");
        }
    }

    void put(int64_t key, int32_t value) {
        int index = slotFor(key);
        int tombstone_index = -1;

        while (true) {
            int64_t existing_key = keys_[index];

            if (existing_key == kEmptyKey) {
                if (tombstone_index >= 0) {
                    keys_[tombstone_index]   = key;
                    values_[tombstone_index] = value;
                    --tombstone_count_;
                } else {
                    keys_[index]   = key;
                    values_[index] = value;
                }
                ++size_;
                return;
            }

            if (existing_key == kTombstone) {
                if (tombstone_index < 0) {
                    tombstone_index = index;
                }
            } else if (existing_key == key) {
                values_[index] = value;
                return;
            }

            index = (index + 1) & mask_;
        }
    }

    [[nodiscard]] int32_t get(int64_t key) const {
        int index = slotFor(key);

        while (true) {
            int64_t existing_key = keys_[index];
            if (existing_key == kEmptyKey) return kMissing;
            if (existing_key == key)       return values_[index];
            index = (index + 1) & mask_;
        }
    }

    int32_t remove(int64_t key) {
        int index = slotFor(key);

        while (true) {
            int64_t existing_key = keys_[index];
            if (existing_key == kEmptyKey) return kMissing;
            if (existing_key == key) {
                int32_t old_value = values_[index];
                keys_[index] = kTombstone;
                --size_;
                ++tombstone_count_;
                return old_value;
            }
            index = (index + 1) & mask_;
        }
    }

    [[nodiscard]] int size() const noexcept { return size_; }

private:
    [[nodiscard]] int slotFor(int64_t key) const noexcept {
        // Fibonacci hashing for uniform distribution
        const uint64_t hash = static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ULL;
        const int bits = std::countr_zero(static_cast<uint32_t>(mask_ + 1));
        return static_cast<int>(hash >> (64 - bits)) & mask_;
    }

    std::vector<int64_t> keys_;
    std::vector<int32_t> values_;
    int mask_;
    int size_;
    int tombstone_count_;
};
