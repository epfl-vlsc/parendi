// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
//
// Code available from: https://verilator.org
//
// Copyright 2003-2023 by Wilson Snyder. This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
///
/// \file
/// \brief Verilated common data type containers
///
/// verilated.h should be included instead of this file.
///
/// Those macro/function/variable starting or ending in _ are internal,
/// however many of the other function/macros here are also internal.
///
//*************************************************************************

#ifndef VERILATOR_VERILATED_IPUTYPES_H_
#define VERILATOR_VERILATED_IPUTYPES_H_

#ifndef VERILATOR_VERILATED_H_INTERNAL_
#error "verilated_types.h should only be included by verilated.h"
#endif

#include <array>
#ifdef __IPU__
struct VlIpuCycle {
    volatile uint32_t l, u;
    VlIpuCycle() {}
    volatile inline void time() {
        u = -1;
        while (__builtin_ipu_get_scount_u() < u) {
            u = __builtin_ipu_get_scount_u();
            l = __builtin_ipu_get_scount_l();
        }
    }
    inline uint64_t get() const {
        return static_cast<uint64_t>(l) | (static_cast<uint64_t>(u) << 32ull);
    }
};
#endif  // __IPU__

template <int ALLOC_SIZE>
struct VlIpuProfileTrace {
    static_assert(ALLOC_SIZE >= 32, "at least 32 words are required!");
    static constexpr int SIZE = (ALLOC_SIZE - 4) / 2;
    static_assert(((ALLOC_SIZE - 4) & 1U) == 0, "SIZE should be even");
    static_assert(SIZE >= 1, "invalid SIZE");
    uint32_t m_storage[ALLOC_SIZE];
    // Avoid defining a constructor, see aggeragete data types in C++
    // data layout:
    //     0-1: total;
    //     2: count
    //     3: head
    //     4-: payload, (start: uint64_t, end: uint64_t)
    inline uint64_t& total() {
        return *reinterpret_cast<uint64_t*>(m_storage);
    }
    inline uint32_t& count() {
        return m_storage[2];
    }
    inline uint32_t& head() {
        return m_storage[3];
    }
    inline uint64_t* datap() {
        return reinterpret_cast<uint64_t*>(m_storage + 4);
    }
    operator WDataOutP() VL_PURE { return &m_storage[0]; }
#ifdef __IPU__
    inline void log(const VlIpuCycle& start, const VlIpuCycle& end) {
        const uint64_t s = start.get();
        const uint64_t e = end.get();
        const uint64_t delta = end.get() - start.get();
        // printf("%lu ", end.get());
        // printf("%lu ", start.get());
        // printf("%lu\n", delta);
        total() += delta;
        count() += 1;
        datap()[head()] = s;
        datap()[head() + 1] = e;
        head() += 2;
        if (head() >= SIZE)
            head() = 0;
    }
#endif  // __IPU__
};


#ifndef __IPU__
#include <cassert>
#include <map>
#include <utility>
#include <vector>
#include <string>
struct VlIpuProfileTraceVec {
    int m_traceSize = 0;
    uint32_t m_currIndex = 0;
    struct Descriptor {
        uint32_t m_tile, m_worker;
        uint32_t m_pred;
        uint32_t m_index;
        uint64_t m_total;
        uint32_t m_count;
    };
    using TraceId = std::string;
    using TracePoint = std::vector<std::pair<uint64_t, uint64_t>>;
    std::map<TraceId, Descriptor> m_desc;
    std::vector<TracePoint> m_trace;

    template <int ALLOC_SIZE>
    void append(VlIpuProfileTrace<ALLOC_SIZE>& p, const std::string& name,
                const uint32_t tileId, const uint32_t workerId, const uint32_t pred) {
        auto neededSize = std::min(static_cast<int>(p.count()), p.SIZE);
        if (m_traceSize < neededSize) {
            m_traceSize = std::min(static_cast<int>(p.count()), p.SIZE);
            // m_trace.clear();
            m_trace.resize(m_traceSize);
            m_currIndex = 0;
        }
        int jx = p.head();
        for (int ix = 0; ix < m_traceSize; ix++) {
            auto s = p.datap()[jx];
            auto e = p.datap()[jx + 1];
            m_trace[ix].push_back({s, e});
            jx += 2;
            if (jx >= p.SIZE) {
                jx = 0;
            }
        }
        m_desc.emplace(
            std::make_pair(
                name,
                Descriptor{
                    .m_tile = tileId,
                    .m_worker = workerId,
                    .m_pred = pred,
                    .m_index = m_currIndex ++,
                    .m_total = p.total(),
                    .m_count = p.count()
                }
            )
        );
    }
};
#endif  // __IPU__
#endif