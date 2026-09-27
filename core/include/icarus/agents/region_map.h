#pragma once
// Survey region labels by position: a flat open-addressing hash table (region ids start
// at 1, so an id of 0 marks an empty slot). Much faster than std::unordered_map for the
// hundreds of thousands of positions a survey labels; iteration order is fixed by the
// insertion history, so it stays deterministic.
#include <vector>

#include "icarus/util/types.h"

namespace icarus {

class RegionMap {
public:
    struct Entry {
        Vec3i first;
        u16 second = 0;
    };

    const Entry* find(const Vec3i& p) const {
        if (slots_.empty()) return nullptr;
        for (size_t i = slot_of(p);; i = (i + 1) & mask_) {
            const Entry& e = slots_[i];
            if (e.second == 0) return nullptr;
            if (e.first == p) return &e;
        }
    }
    const Entry* end() const { return nullptr; }
    size_t count(const Vec3i& p) const { return find(p) ? 1 : 0; }
    // Labels p unless it already has a label; true if it was new.
    bool emplace(const Vec3i& p, u16 id) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        for (size_t i = slot_of(p);; i = (i + 1) & mask_) {
            Entry& e = slots_[i];
            if (e.second == 0) {
                e.first = p;
                e.second = id;
                ++size_;
                return true;
            }
            if (e.first == p) return false;
        }
    }
    void set(const Vec3i& p, u16 id) {
        if (!emplace(p, id))
            for (size_t i = slot_of(p);; i = (i + 1) & mask_)
                if (slots_[i].first == p) {
                    slots_[i].second = id;
                    return;
                }
    }
    void clear() {
        slots_.clear();
        size_ = 0;
        mask_ = 0;
    }
    void reserve(size_t n) {
        size_t cap = 16;
        while (cap < n * 2) cap <<= 1;
        if (cap > slots_.size()) rehash(cap);
    }
    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }
    template <class F>
    void for_each(F&& f) const {
        for (const Entry& e : slots_)
            if (e.second) f(e.first, e.second);
    }

private:
    std::vector<Entry> slots_;
    size_t size_ = 0;
    size_t mask_ = 0;

    size_t slot_of(const Vec3i& p) const {
        u64 k = ((u64)(u32)(p.x + 32768) << 32) ^ ((u64)(u32)(p.y + 32768) << 16) ^ (u64)(u32)(p.z + 32768);
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return (size_t)k & mask_;
    }
    void grow() { rehash(slots_.empty() ? 1024 : slots_.size() * 2); }
    void rehash(size_t cap) {
        std::vector<Entry> old;
        old.swap(slots_);
        slots_.assign(cap, Entry{});
        mask_ = cap - 1;
        size_ = 0;
        for (const Entry& e : old)
            if (e.second) emplace(e.first, e.second);
    }
};

}  // namespace icarus
