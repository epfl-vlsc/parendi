// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Merge and balance BSP partitions
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2023 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//

#ifndef VERILATOR_V3BSPMERGER_H_
#define VERILATOR_V3BSPMERGER_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3BspGraph.h"
#include "V3FunctionTraits.h"
#include "V3Sched.h"

#include <climits>
#include <limits>
namespace V3BspSched {

class VlBitSet final {
private:
    using WordType = uint64_t;
    const size_t m_maxElems;
    std::vector<uint64_t> m_bits;
    static constexpr size_t BitsPerWord = sizeof(WordType) * CHAR_BIT;
    static_assert(CHAR_BIT == 8, "CHAR_BIT not 8");
    inline std::pair<size_t, size_t> indexTuple(size_t ix) const {
        UASSERT(ix < m_maxElems, "value out of bounds");
        const std::pair<size_t, size_t> index{ix / BitsPerWord, ix % BitsPerWord};
        UASSERT(index.first < m_maxElems && index.second < BitsPerWord, "out of range");
        return index;
    }

public:
    explicit VlBitSet(size_t maxElems)
        : m_maxElems{maxElems} {
        UASSERT(m_maxElems > 0, "can not construct empty bitset");
        const uint64_t numWords = (maxElems - 1) / BitsPerWord + 1;
        m_bits.resize(numWords);
    }

    explicit VlBitSet(size_t maxElems, std::initializer_list<size_t> ilist)
        : VlBitSet{maxElems} {
        for (const auto v : ilist) insert(v);
    }

    VlBitSet(VlBitSet&& other)
        : m_maxElems{other.m_maxElems}
        , m_bits{std::move(other.m_bits)} {}

    VlBitSet& operator=(VlBitSet&& other) {
        UASSERT(other.m_maxElems == m_maxElems, "invalid assignment");
        m_bits = std::move(other.m_bits);
        return *this;
    }

    VlBitSet(const VlBitSet& other)
        : m_maxElems{other.m_maxElems}
        , m_bits{other.m_bits} {}

    VlBitSet& operator=(const VlBitSet& other) {
        UASSERT(other.m_maxElems == m_maxElems, "invalid assignment");
        m_bits = other.m_bits;
        return *this;
    }

    inline void insert(size_t v) {
        const auto index = indexTuple(v);
        m_bits[index.first] |= (1ULL << index.second);
    }

    inline bool contains(size_t v) const {
        const auto index = indexTuple(v);
        return m_bits[index.first] & (1ULL << index.second);
    }

    inline void intersectInPlace(const VlBitSet& other) {
        UASSERT(other.m_bits.size() == m_bits.size(), "VlBitSet of different size");
        for (size_t ix = 0; ix < m_bits.size(); ix++) { m_bits[ix] &= other.m_bits[ix]; }
    }

    inline void unionInPlace(const VlBitSet& other) {
        UASSERT(other.m_bits.size() == m_bits.size(), "VlBitSet of different size");
        for (size_t ix = 0; ix < m_bits.size(); ix++) { m_bits[ix] |= other.m_bits[ix]; }
    }
    inline size_t maxElems() const { return m_maxElems; }
    inline size_t size() const {
        // based on "Counting bits set, in parallel" from
        // https://graphics.stanford.edu/~seander/bithacks.html
        auto bitsSet = [](WordType v) {
            constexpr WordType mask = std::numeric_limits<WordType>::max();
            v = v - ((v >> 1ULL) & (mask / 3ULL));
            v = (v & (mask / 15ULL * 3ULL)) + ((v >> 2ULL) & (mask / 15ULL * 3ULL));
            v = (v + (v >> 4ULL)) & (mask / 255ULL * 15ULL);
            WordType c = (v * (mask / 255ULL))
                         >> static_cast<WordType>((sizeof(WordType) - 1ULL) * CHAR_BIT);
            return static_cast<size_t>(c);
        };
        size_t sum = 0;
        for (const auto v : m_bits) {
            const auto c = bitsSet(v);
            std::cout << c << std::endl;
            sum += bitsSet(v);
        }
        UASSERT(sum <= m_maxElems, "invalid size");
        return sum;
    }
    static VlBitSet doIntersect(const VlBitSet& set1, const VlBitSet& set2) {
        VlBitSet res{set1};
        res.intersectInPlace(set2);
        return res;
    }

    static VlBitSet doUnion(const VlBitSet& set1, const VlBitSet& set2) {
        VlBitSet res{set1};
        res.unionInPlace(set2);
        return res;
    }

    std::string toString() const {
        std::stringstream ss;
        bool first = true;
        ss << "{";
        for (size_t v = 0; v < m_maxElems; v++) {
            if (contains(v)) {
                if (!first) { ss << ", "; }
                ss << v;
                first = false;
            }
        }
        ss << "}";
        return ss.str();
    }
    template <typename Callable>
    void foreach(Callable&& fn) {
        size_t ix = 0;
        for (size_t ix = 0; ix < m_bits.size(); ix++) {
            WordType v = m_bits[ix];
            size_t elem = ix * BitsPerWord;
            while (v != 0) {
                if (v & 1ULL) { fn(elem); }
                v >>= 1;
                elem++;
            }
        }
    }
};
class V3BspMerger final {
public:
    static void merge(std::vector<std::unique_ptr<DepGraph>>& partitionsp);
};
}  // namespace V3BspSched

#endif