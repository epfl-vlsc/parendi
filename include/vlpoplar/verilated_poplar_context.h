// -*- mode: C++; c-file-style: "cc-mode" -*-
//=============================================================================
//
// Code available from: https://verilator.org
//
// Copyright 2001-2023 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//=============================================================================
///
/// \file  verilated_poplar_context.h
/// \brief Verilated poplar context implementation
///
//=============================================================================

#ifndef VERILATOR_POPLARCONTEXT_H_
#define VERILATOR_POPLARCONTEXT_H_

// #include "VProgram.h"
#include "verilated.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <unordered_map>

#include <boost/filesystem.hpp>
#include <poplar/DeviceManager.hpp>
#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>
#include <poplar/IPUModel.hpp>
#include <poplar/Tensor.hpp>

struct CounterConfig {
    bool exec = false;
    bool sync1 = false;
    bool copy = false;
    bool sync2 = false;
    bool cond = false;
    bool loop = false;
};

struct RuntimeConfig {
    // boost::filesystem::path simOutPath;
    // uint64_t maxRtlCycles = std::numeric_limits<uint64_t>::max();
    CounterConfig counters;
    uint64_t maxRtlCyles;
    bool emulate;
    bool showSteps;
    bool errorOnTimeout;
    bool instrument;
};

RuntimeConfig parseArgs(int argc, char* argv[]);
#ifdef VPROGRAM
class VPROGRAM;
#else
#error "VPROGRAM is not defined"
#endif
// forward decl

///
/// VlPoplarContext is responsbile for create a poplar CDFG, currently we treat
/// computation in two distict phases: initial and nba.
/// The initial phase runs before nba to completion and the nba is a loop.
/// There is a special host request variable, let's call it hasDpi that can
/// break the flow of execution. The initial phase has the following structure:
///
///     init cache for $plusargs and maybe $readmem
///     copy cache to device
///     hasDpi = 0;
///     forall dpiVec: dpiVec = 0;
///     on the host:
///             do:
///                IPU| Execute(initComputeSet);
///                IPU| dpiExchange;
///                IPU| dpiEval;
///                IPU| dpiBroadcast;
///                hostHandle();
///             while hasDpi
///             IPU| initExchage;
///
///
/// The nba phase is slightly more complicated:
///
///     on the host:
///         clear hasDpi; clear dpiVec
///         let simLoop be:
///         IPU|    dpiBroadcast
///         IPU|    Execute(nba)
///         IPU|    while !hasDpi:
///         IPU|        (pre) dpiExchange
///         IPU|        (pre) dpiEval
///         IPU|        nbaExchange
///         IPU|        Execute(nba)
///
///         while !finished:
///             IPU| dpiEval
///             IPU| dpiBroadcast
///             IPU| if hasDpi:
///             IPU|    Execute(nba)
///             IPU|    dpiExchange
///             IPU|    dpiEval
///             IPU|    if !hasDpi:
///             IPU|        nbaExchange
///             IPU| if !hasDpi:
///             IPU|        simLoop
///             hostHandle();

class VlPoplarContext final {
public:
    using TensorId = int;
    struct TensorHandle {
        TensorId id;
        int begin;
        int end;
        int totalSize;
    };
    template <typename Value>
    using TensorIdMap = std::vector<Value>;

private:
    struct HostBuffer {
        std::vector<uint32_t> buff;
        HostBuffer() = default;
        HostBuffer(uint32_t elems)
            : buff(elems){};
    };
    enum EProgramId : uint32_t { E_RESET = 0, E_INIT = 1, E_INITCOPY = 2, E_NBA = 3, _E_NUM_PROG };
    static constexpr int INIT_PROGRAM = 0;
    static constexpr int EVAL_PROGRAM = 1;
    RuntimeConfig cfg;
    std::unique_ptr<VPROGRAM> vprog;
    std::unique_ptr<poplar::Device> device;
    std::unique_ptr<poplar::Graph> graph;
    std::unique_ptr<poplar::Engine> engine;
    std::unique_ptr<poplar::Executable> exec;
    std::unique_ptr<poplar::ComputeSet> workload;
    std::unique_ptr<poplar::ComputeSet> condeval;
    std::unique_ptr<poplar::ComputeSet> initializer;

    std::unordered_map<TensorId, poplar::Tensor> tensors;
    std::unordered_set<TensorId> alreadyMapped;
    std::unordered_map<std::string, std::unique_ptr<HostBuffer>> hbuffers;
    std::unordered_map<TensorId, std::vector<std::pair<int, poplar::Tensor>>> tensorChunks;
    std::unordered_map<TensorId, poplar::Tensor> nextToCurrent;

    std::unordered_map<std::string, poplar::VertexRef> vertices;

    // std::unordered_map<std::string, std::string> currentToNext;

    std::vector<poplar::Tensor> hostRequest;
    poplar::Tensor interruptCond;
    poplar::program::Sequence initCopies;
    poplar::program::Sequence constInitCopies;
    poplar::program::Sequence exchangeCopies;
    poplar::program::Sequence dpiCopies;
    poplar::program::Sequence dpiBroadcastCopies;
    bool hasCompute = false, hasInit = false, hasCond = false;

    // std::chrono::time_point<std::chrono::high_resolution_clock> m_startTime;
    // double m_simRateLast;
    poplar::Tensor getTensor(const TensorId tid) {
        if (tensors.count(tid) == 0) {
            std::cerr << "Can not find tensor " << tid << std::endl;
            std::exit(EXIT_FAILURE);
        }
        return tensors[tid];
    }
    poplar::Tensor addTensor(uint32_t size, const TensorId& name);
    poplar::Tensor mkTensor(uint32_t size, const std::string& name);
    void dumpCycleTrace(std::ostream& os);

public:
    void init(int argc, char* argv[]);
    void build();
    void buildReEntrant();
    void run();
    void runReEntrant();
    void addCopy(const TensorId& from, const TensorId& to, const int offsetTo, uint32_t size,
                 const std::string& kind);
    void addNextCurrentPair(const TensorId& next, const int nextSize, const TensorId& current,
                            const int currentBegin, const int currentEnd, const int currentSize,
                            const int currentTile);

    void setTileMapping(poplar::VertexRef& vtxRef, uint32_t tileId);
    void setTileMapping(poplar::Tensor& tensor, const TensorId& tid, uint32_t tileId);
    void connect(poplar::VertexRef& vtxRef, const std::string& vtxField, poplar::Tensor& tensor);
    void isHostRequest(poplar::Tensor& tensor, bool isInterruptCond);
    void createHostRead(const std::string& handleName, poplar::Tensor& tensor, uint32_t numElems);
    void createHostWrite(const std::string& handleName, poplar::Tensor& tensor, uint32_t numElems);
    void setPerfEstimate(poplar::VertexRef&, int) {}
    poplar::VertexRef getOrAddVertex(const std::string& name, const std::string& where);

    poplar::Tensor getOrAddTensor(uint32_t size, const TensorId& name, const int tileId);

    template <typename T>
    inline T getHostData(const std::string& handle) {
        static_assert(std::is_trivially_copy_assignable<T>());
        static_assert(std::is_trivially_copy_constructible<T>());
        // find the host buffer
        auto it = hbuffers.find(handle);
        if (it == hbuffers.end()) {
            std::cerr << "Can not find host handle " << handle << std::endl;
            std::exit(EXIT_FAILURE);
        }
        if (it->second->buff.size() == 1) {
            // hacky stuff to handle padded uint32_t values
            std::array<uint32_t, 2> v;
            engine->readTensor(handle, v.data(), v.data() + 2);
            it->second->buff[0] = v[0];

        } else {
            engine->readTensor(handle, it->second->buff.data(),
                               it->second->buff.data() + it->second->buff.size());
        }
        return (*reinterpret_cast<T*>(it->second->buff.data()));
    }
    template <typename T>
    inline void setHostData(const std::string& handle, const T& value) {
        static_assert(std::is_trivially_copy_assignable<T>());
        static_assert(std::is_trivially_copy_constructible<T>());
        auto it = hbuffers.find(handle);
        if (it == hbuffers.end()) {
            std::cerr << "Can not find host handle " << handle << std::endl;
        }
        auto datap = reinterpret_cast<const uint32_t*>(&value);
        if (it->second->buff.size() == 1) {
            // handle padded uint32_t values, see also addTensor method
            std::array<uint32_t, 2> v;
            v[0] = datap[0];
            v[1] = 0;
            engine->writeTensor(handle, v.data(), v.data() + 2);
        } else {
            engine->writeTensor(handle, datap, datap + it->second->buff.size());
        }
    }
};

#endif
