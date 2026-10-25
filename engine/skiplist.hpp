#pragma once

#include <atomic>
#include <cstdint>
#include <cassert>
#include <random>
#include "arena.hpp"

namespace lsm {

template <typename Key, class Comparator>
class SkipList {
public:
    struct Node;

    explicit SkipList(Comparator cmp, Arena* arena);
    ~SkipList() = default;

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // Insert key into the list.
    // REQUIRES: nothing that compares equal to key is currently in the list.
    void Insert(const Key& key);

    // Returns true iff an entry that compares equal to key is in the list.
    bool Contains(const Key& key) const;

    // Iteration over the contents of a skip list
    class Iterator {
    public:
        explicit Iterator(const SkipList* list);

        bool Valid() const;
        const Key& key() const;
        void Next();
        void Prev();
        void Seek(const Key& target);
        void SeekToFirst();
        void SeekToLast();

    private:
        const SkipList* list_{nullptr};
        Node* node_{nullptr};
    };

private:
    enum { kMaxHeight = 12 };

    Comparator const compare_;
    Arena* const arena_;

    Node* const head_;

    std::atomic<int> max_height_{1};

    inline int GetMaxHeight() const {
        return max_height_.load(std::memory_order_relaxed);
    }

    Node* NewNode(const Key& key, int height);
    int RandomHeight();
    bool Equal(const Key& a, const Key& b) const { return (compare_(a, b) == 0); }

    // Return true if key is greater than the data stored in "n"
    bool KeyIsAfterNode(const Key& key, Node* n) const;

    // Return the earliest node that comes at or after key.
    // Return nullptr if there is no such node.
    Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

    // Return the latest node with a key < key.
    // Return head_ if there is no such node.
    Node* FindLessThan(const Key& key) const;

    // Return the last node in the list.
    // Return head_ if list is empty.
    Node* FindLast() const;
};

// Implementation details
template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
    explicit Node(const Key& k) : key(k) {}

    Key const key;

    // Accessors/mutators for links. Wrapped in methods to emphasize
    // that we need acquire/release ordering for access.
    Node* Next(int n) {
        assert(n >= 0);
        return next_[n].load(std::memory_order_acquire);
    }

    void SetNext(int n, Node* x) {
        assert(n >= 0);
        next_[n].store(x, std::memory_order_release);
    }

    // No-barrier variants that can be used when concurrent access is not possible
    Node* NoBarrier_Next(int n) {
        assert(n >= 0);
        return next_[n].load(std::memory_order_relaxed);
    }

    void NoBarrier_SetNext(int n, Node* x) {
        assert(n >= 0);
        next_[n].store(x, std::memory_order_relaxed);
    }

private:
    // Array of next pointers. We allocate this struct with extra trailing space
    // to hold 'height' pointers.
    std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::NewNode(const Key& key, int height) {
    char* const node_memory = arena_->AllocateAligned(
        sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1)
    );
    return new (node_memory) Node(key);
}

template <typename Key, class Comparator>
inline SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list) {
    list_ = list;
    node_ = nullptr;
}

template <typename Key, class Comparator>
inline bool SkipList<Key, Comparator>::Iterator::Valid() const {
    return node_ != nullptr;
}

template <typename Key, class Comparator>
inline const Key& SkipList<Key, Comparator>::Iterator::key() const {
    assert(Valid());
    return node_->key;
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Next() {
    assert(Valid());
    node_ = node_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Prev() {
    // Instead of using backward pointers, we find the node before 'node_'
    assert(Valid());
    node_ = list_->FindLessThan(node_->key);
    if (node_ == list_->head_) {
        node_ = nullptr;
    }
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Seek(const Key& target) {
    node_ = list_->FindGreaterOrEqual(target, nullptr);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToFirst() {
    node_ = list_->head_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToLast() {
    node_ = list_->FindLast();
    if (node_ == list_->head_) {
        node_ = nullptr;
    }
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
    // Geometric distribution with p = 1/4
    static thread_local std::mt19937 rng(std::random_device{}());
    static constexpr uint32_t kBranching = 4;
    int height = 1;
    while (height < kMaxHeight && ((rng() % kBranching) == 0)) {
        height++;
    }
    assert(height > 0);
    assert(height <= kMaxHeight);
    return height;
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::KeyIsAfterNode(const Key& key, Node* n) const {
    // nullptr is considered infinite
    return (n != nullptr) && (compare_(n->key, key) < 0);
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        Node* next = x->Next(level);
        if (KeyIsAfterNode(key, next)) {
            // Keep searching in this list
            x = next;
        } else {
            if (prev != nullptr) prev[level] = x;
            if (level == 0) {
                return next;
            } else {
                // Switch to next lower list
                level--;
            }
        }
    }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindLessThan(const Key& key) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        assert(x == head_ || compare_(x->key, key) < 0);
        Node* next = x->Next(level);
        if (next == nullptr || compare_(next->key, key) >= 0) {
            if (level == 0) {
                return x;
            } else {
                // Switch to next lower list
                level--;
            }
        } else {
            x = next;
        }
    }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindLast() const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        Node* next = x->Next(level);
        if (next == nullptr) {
            if (level == 0) {
                return x;
            } else {
                level--;
            }
        } else {
            x = next;
        }
    }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(Key{}, kMaxHeight)),
      max_height_(1) {
    for (int i = 0; i < kMaxHeight; ++i) {
        head_->SetNext(i, nullptr);
    }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);

    // Insertion requirement: no duplicate internal keys
    assert(x == nullptr || !Equal(key, x->key));
    (void)x;

    int height = RandomHeight();
    if (height > GetMaxHeight()) {
        for (int i = GetMaxHeight(); i < height; ++i) {
            prev[i] = head_;
        }
        // It is ok to mutate max_height_ without any lock because readers
        // will simply observe the new height through atomic acquire.
        max_height_.store(height, std::memory_order_relaxed);
    }

    x = NewNode(key, height);
    for (int i = 0; i < height; ++i) {
        // NoBarrier_SetNext is safe because x is not yet visible to readers
        x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
        prev[i]->SetNext(i, x);
    }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    if (x != nullptr && Equal(key, x->key)) {
        return true;
    } else {
        return false;
    }
}

} // namespace lsm
