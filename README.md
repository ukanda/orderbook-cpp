# orderbook-cpp

A zero-allocation limit-order book for a price-time priority matching engine, written in C++20.

## Features

- **Zero allocation on the hot path** — all memory is pre-allocated at construction time; no heap allocation during order add, cancel, match, or query operations
- **Price-time priority** — orders at the same price level are matched FIFO
- **O(log N) price level management** — each side is backed by a Red-Black Tree templated on a `std::less` / `std::greater` comparator, supporting tens of thousands of distinct price levels efficiently
- **O(1) top-of-book access** — best bid and ask prices are cached
- **O(1) cancel and reduce** — orders are looked up by ID via a zero-allocation open-addressing hash map

## Data structures

| Structure | Purpose |
|---|---|
| Struct-of-arrays pool | Stores orders and price levels as flat primitive arrays — no object headers, cache-friendly |
| Intrusive free-list | Recycles order and level slots with zero allocation |
| `PriceLevelTree<Comparator>` | Zero-allocation intrusive Red-Black Tree; one instance per side, ordering controlled by a `PriceComparator` concept |
| `LongIntHashMap` | Zero-allocation open-addressing hash map (`int64_t` → `int32_t`) with Fibonacci hashing |

## API

```cpp
OrderBook book(/* max_orders */ 1024, /* max_levels */ 256);

// Add a limit order — returns quantity filled immediately
int64_t filled = book.addOrder(orderId, OrderBook::kBid, price, quantity);

// Cancel a resting order
book.cancelOrder(orderId);

// Reduce quantity of a resting order (keeps queue position)
book.reduceOrder(orderId, newQuantity);

// Query
book.getBestBidPrice();
book.getBestAskPrice();
book.getBestBidQuantity();
book.getBestAskQuantity();
book.getSpread();
book.getQuantityAtBidPrice(price);
book.getQuantityAtAskPrice(price);
```

## Building and testing

Requires CMake 3.20+ and a C++20-capable compiler (Clang 13+, GCC 11+).
GoogleTest is fetched automatically via CMake's `FetchContent`.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
