#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>

/**
 * Concept for a price ordering callable: takes two int64_t prices and
 * returns bool, where comp(a, b) == true means a sorts before b.
 */
template<typename Comp>
concept PriceComparator = requires(Comp comp, int64_t a, int64_t b) {
    { comp(a, b) } -> std::convertible_to<bool>;
};

/**
 * Zero-allocation intrusive Red-Black Tree for price levels.
 *
 * "Intrusive" means this class operates on raw arrays owned by OrderBook.
 * Node identity is an integer slot index into those arrays.
 *
 * Red-Black Tree invariants (CLRS):
 *   1. Every node is RED or BLACK.
 *   2. The root is BLACK.
 *   3. Absent (nil) children are conceptually BLACK.
 *   4. Both children of a RED node are BLACK.
 *   5. All paths from a node to descendant nil leaves have the same black-height.
 *
 * The ordering is determined by the Comparator template parameter:
 *   - std::less<int64_t>    : ascending — min() returns the best ask price
 *   - std::greater<int64_t> : descending — min() returns the best bid price
 *
 * In both cases, min() returns the best price and successor() returns the
 * next worse price level, so OrderBook uses identical operations for both sides.
 *
 * All operations are O(log N). No memory is allocated after construction.
 */
template<PriceComparator Comparator = std::less<int64_t>>
class PriceLevelTree {
public:
    static constexpr int kNullSlot = std::numeric_limits<int>::min();
    static constexpr int kRed      = 0;
    static constexpr int kBlack    = 1;

    PriceLevelTree(Comparator comparator,
                   int64_t*   prices,
                   int*       left_children,
                   int*       right_children,
                   int*       parents,
                   int*       colors)
        : comparator_(std::move(comparator))
        , prices_(prices)
        , left_children_(left_children)
        , right_children_(right_children)
        , parents_(parents)
        , colors_(colors)
        , root_(kNullSlot) {}

    // --- Public operations ---

    /** Insert the given slot. Its price must be set before calling. O(log N). */
    void insert(int slot) {
        int parent_slot = kNullSlot;
        int current     = root_;

        while (current != kNullSlot) {
            parent_slot = current;
            current = comparator_(prices_[slot], prices_[current])
                    ? left_children_[current]
                    : right_children_[current];
        }

        parents_[slot] = parent_slot;

        if (parent_slot == kNullSlot) {
            root_ = slot;
        } else if (comparator_(prices_[slot], prices_[parent_slot])) {
            left_children_[parent_slot] = slot;
        } else {
            right_children_[parent_slot] = slot;
        }

        left_children_[slot]  = kNullSlot;
        right_children_[slot] = kNullSlot;
        colors_[slot]         = kRed;
        insertFixup(slot);
    }

    /** Remove the given slot from the tree. O(log N). */
    void erase(int slot) {
        int moved_node                = slot;
        int moved_node_original_color = colors_[moved_node];
        int fixup_node;
        int fixup_parent;

        if (left_children_[slot] == kNullSlot) {
            fixup_node   = right_children_[slot];
            fixup_parent = parents_[slot];
            transplant(slot, right_children_[slot]);
        } else if (right_children_[slot] == kNullSlot) {
            fixup_node   = left_children_[slot];
            fixup_parent = parents_[slot];
            transplant(slot, left_children_[slot]);
        } else {
            // Replace slot with its in-order successor (minimum of right subtree)
            moved_node                = treeMin(right_children_[slot]);
            moved_node_original_color = colors_[moved_node];
            fixup_node                = right_children_[moved_node];

            if (parents_[moved_node] == slot) {
                fixup_parent = moved_node;
            } else {
                fixup_parent = parents_[moved_node];
                transplant(moved_node, right_children_[moved_node]);
                right_children_[moved_node]              = right_children_[slot];
                parents_[right_children_[moved_node]]    = moved_node;
            }

            transplant(slot, moved_node);
            left_children_[moved_node]           = left_children_[slot];
            parents_[left_children_[moved_node]] = moved_node;
            colors_[moved_node]                  = colors_[slot];
        }

        if (moved_node_original_color == kBlack) {
            deleteFixup(fixup_node, fixup_parent);
        }
    }

    /**
     * Returns the slot with the minimum price according to the comparator,
     * or kNullSlot if empty. O(log N).
     */
    [[nodiscard]] int min() const {
        return root_ == kNullSlot ? kNullSlot : treeMin(root_);
    }

    /**
     * Returns the slot with the maximum price according to the comparator,
     * or kNullSlot if empty. O(log N).
     */
    [[nodiscard]] int max() const {
        return root_ == kNullSlot ? kNullSlot : treeMax(root_);
    }

    /**
     * Returns the slot with the next larger price in comparator order
     * (i.e. the next worse price level), or kNullSlot if node is the maximum.
     * O(log N).
     */
    [[nodiscard]] int successor(int node) const {
        if (right_children_[node] != kNullSlot) {
            return treeMin(right_children_[node]);
        }
        int ancestor = parents_[node];
        while (ancestor != kNullSlot && node == right_children_[ancestor]) {
            node     = ancestor;
            ancestor = parents_[ancestor];
        }
        return ancestor;
    }

    /**
     * Returns the slot with the next smaller price in comparator order,
     * or kNullSlot if node is the minimum. O(log N).
     */
    [[nodiscard]] int predecessor(int node) const {
        if (left_children_[node] != kNullSlot) {
            return treeMax(left_children_[node]);
        }
        int ancestor = parents_[node];
        while (ancestor != kNullSlot && node == left_children_[ancestor]) {
            node     = ancestor;
            ancestor = parents_[ancestor];
        }
        return ancestor;
    }

    [[nodiscard]] bool empty() const noexcept { return root_ == kNullSlot; }

private:
    // --- Tree navigation ---

    [[nodiscard]] int treeMin(int node) const {
        while (left_children_[node] != kNullSlot) {
            node = left_children_[node];
        }
        return node;
    }

    [[nodiscard]] int treeMax(int node) const {
        while (right_children_[node] != kNullSlot) {
            node = right_children_[node];
        }
        return node;
    }

    /** Replace the subtree rooted at removed with the subtree rooted at replacement. */
    void transplant(int removed, int replacement) {
        if (parents_[removed] == kNullSlot) {
            root_ = replacement;
        } else if (removed == left_children_[parents_[removed]]) {
            left_children_[parents_[removed]]  = replacement;
        } else {
            right_children_[parents_[removed]] = replacement;
        }
        if (replacement != kNullSlot) {
            parents_[replacement] = parents_[removed];
        }
    }

    // --- Rotations ---

    void rotateLeft(int pivot) {
        int new_root              = right_children_[pivot];
        right_children_[pivot]    = left_children_[new_root];

        if (left_children_[new_root] != kNullSlot) {
            parents_[left_children_[new_root]] = pivot;
        }

        parents_[new_root] = parents_[pivot];

        if (parents_[pivot] == kNullSlot) {
            root_ = new_root;
        } else if (pivot == left_children_[parents_[pivot]]) {
            left_children_[parents_[pivot]]  = new_root;
        } else {
            right_children_[parents_[pivot]] = new_root;
        }

        left_children_[new_root] = pivot;
        parents_[pivot]          = new_root;
    }

    void rotateRight(int pivot) {
        int new_root             = left_children_[pivot];
        left_children_[pivot]    = right_children_[new_root];

        if (right_children_[new_root] != kNullSlot) {
            parents_[right_children_[new_root]] = pivot;
        }

        parents_[new_root] = parents_[pivot];

        if (parents_[pivot] == kNullSlot) {
            root_ = new_root;
        } else if (pivot == right_children_[parents_[pivot]]) {
            right_children_[parents_[pivot]] = new_root;
        } else {
            left_children_[parents_[pivot]]  = new_root;
        }

        right_children_[new_root] = pivot;
        parents_[pivot]           = new_root;
    }

    // --- Insert fixup: restore Red-Black invariants after BST insert ---

    void insertFixup(int node) {
        while (parents_[node] != kNullSlot && colors_[parents_[node]] == kRed) {
            int node_parent  = parents_[node];
            int grand_parent = parents_[node_parent];

            if (node_parent == left_children_[grand_parent]) {
                int uncle = right_children_[grand_parent];

                if (uncle != kNullSlot && colors_[uncle] == kRed) {
                    // Case 1: uncle is RED — recolor and move up
                    colors_[node_parent]  = kBlack;
                    colors_[uncle]        = kBlack;
                    colors_[grand_parent] = kRed;
                    node = grand_parent;
                } else {
                    if (node == right_children_[node_parent]) {
                        // Case 2: node is right child — left-rotate to reach Case 3
                        node        = node_parent;
                        rotateLeft(node);
                        node_parent  = parents_[node];
                        grand_parent = parents_[node_parent];
                    }
                    // Case 3: node is left child
                    colors_[node_parent]  = kBlack;
                    colors_[grand_parent] = kRed;
                    rotateRight(grand_parent);
                }
            } else {
                // Mirror: node_parent is the right child of grand_parent
                int uncle = left_children_[grand_parent];

                if (uncle != kNullSlot && colors_[uncle] == kRed) {
                    colors_[node_parent]  = kBlack;
                    colors_[uncle]        = kBlack;
                    colors_[grand_parent] = kRed;
                    node = grand_parent;
                } else {
                    if (node == left_children_[node_parent]) {
                        node        = node_parent;
                        rotateRight(node);
                        node_parent  = parents_[node];
                        grand_parent = parents_[node_parent];
                    }
                    colors_[node_parent]  = kBlack;
                    colors_[grand_parent] = kRed;
                    rotateLeft(grand_parent);
                }
            }
        }
        colors_[root_] = kBlack;
    }

    // --- Delete fixup: restore Red-Black invariants after BST delete ---
    //
    // node        - the node that replaced the deleted/moved node (may be kNullSlot,
    //               representing a double-black nil leaf)
    // node_parent - parent of node, tracked explicitly because node may be kNullSlot

    void deleteFixup(int node, int node_parent) {
        while (node != root_ && isBlack(node)) {
            if (node == left_children_[node_parent]) {
                int sibling = right_children_[node_parent];

                if (colors_[sibling] == kRed) {
                    // Case 1: sibling is RED — rotate to make sibling BLACK
                    colors_[sibling]      = kBlack;
                    colors_[node_parent]  = kRed;
                    rotateLeft(node_parent);
                    sibling = right_children_[node_parent];
                }

                if (isBlack(left_children_[sibling]) && isBlack(right_children_[sibling])) {
                    // Case 2: both of sibling's children are BLACK — push double-black up
                    colors_[sibling] = kRed;
                    node        = node_parent;
                    node_parent = parents_[node];
                } else {
                    if (isBlack(right_children_[sibling])) {
                        // Case 3: sibling's right child is BLACK — rotate to reach Case 4
                        if (left_children_[sibling] != kNullSlot) {
                            colors_[left_children_[sibling]] = kBlack;
                        }
                        colors_[sibling] = kRed;
                        rotateRight(sibling);
                        sibling = right_children_[node_parent];
                    }
                    // Case 4: sibling's right child is RED
                    colors_[sibling]     = colors_[node_parent];
                    colors_[node_parent] = kBlack;
                    if (right_children_[sibling] != kNullSlot) {
                        colors_[right_children_[sibling]] = kBlack;
                    }
                    rotateLeft(node_parent);
                    node = root_;
                }
            } else {
                // Mirror: node is the right child of node_parent
                int sibling = left_children_[node_parent];

                if (colors_[sibling] == kRed) {
                    colors_[sibling]     = kBlack;
                    colors_[node_parent] = kRed;
                    rotateRight(node_parent);
                    sibling = left_children_[node_parent];
                }

                if (isBlack(right_children_[sibling]) && isBlack(left_children_[sibling])) {
                    colors_[sibling] = kRed;
                    node        = node_parent;
                    node_parent = parents_[node];
                } else {
                    if (isBlack(left_children_[sibling])) {
                        if (right_children_[sibling] != kNullSlot) {
                            colors_[right_children_[sibling]] = kBlack;
                        }
                        colors_[sibling] = kRed;
                        rotateLeft(sibling);
                        sibling = left_children_[node_parent];
                    }
                    colors_[sibling]     = colors_[node_parent];
                    colors_[node_parent] = kBlack;
                    if (left_children_[sibling] != kNullSlot) {
                        colors_[left_children_[sibling]] = kBlack;
                    }
                    rotateRight(node_parent);
                    node = root_;
                }
            }
        }
        if (node != kNullSlot) {
            colors_[node] = kBlack;
        }
    }

    /** Absent (nil) nodes are conceptually BLACK. */
    [[nodiscard]] bool isBlack(int node) const noexcept {
        return node == kNullSlot || colors_[node] == kBlack;
    }

    Comparator comparator_;
    int64_t*   prices_;
    int*       left_children_;
    int*       right_children_;
    int*       parents_;
    int*       colors_;
    int        root_;
};
