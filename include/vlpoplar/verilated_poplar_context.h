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

#include <iostream>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <boost/filesystem.hpp>
#include <poplar/CycleCount.hpp>
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
    bool loop = false;
};

struct RuntimeConfig {
    // boost::filesystem::path simOutPath;
    // uint64_t maxRtlCycles = std::numeric_limits<uint64_t>::max();
    CounterConfig counters;
    bool emulate;
    bool showSteps;
    bool errorOnTimeout;
};

RuntimeConfig parseArgs(int argc, char* argv[]);
#ifdef VPROGRAM
class VPROGRAM;
#else
#error "VPROGRAM is not defined"
#endif
// forward decl

class VlPoplarContext final {
private:
    struct HostBuffer {
        std::vector<uint32_t> buff;
        HostBuffer() = default;
        HostBuffer(uint32_t elems)
            : buff(elems){};
    };
    static constexpr int INIT_PROGRAM = 0;
    static constexpr int EVAL_PROGRAM = 1;
    RuntimeConfig cfg;
    std::unique_ptr<VPROGRAM> vprog;
    std::unique_ptr<poplar::Device> device;
    std::unique_ptr<poplar::Graph> graph;
    std::unique_ptr<poplar::Engine> engine;
    std::unique_ptr<poplar::ComputeSet> workload;
    std::unique_ptr<poplar::ComputeSet> initializer;
    std::unordered_map<std::string, poplar::Tensor> tensors;
    std::unordered_map<std::string, std::unique_ptr<HostBuffer>> hbuffers;
    std::unordered_map<std::string, poplar::VertexRef> vertices;
    std::vector<poplar::Tensor> hostRequest;
    poplar::program::Sequence initCopies;
    poplar::program::Sequence exchangeCopies;



    // std::chrono::time_point<std::chrono::high_resolution_clock> m_startTime;
    // double m_simRateLast;
    poplar::Tensor getTensor(const std::string& name) {
        auto it = tensors.find(name);
        if (it == tensors.end()) {
            std::cerr << "Can not find tensor " << name << std::endl;
            std::exit(EXIT_FAILURE);
        }
        return it->second;
    }

public:
    void init(int argc, char* argv[]);
    void build();
    void run();
    void addCopy(const std::string& from, const std::string& to, uint32_t size, bool isInit);
    void setTileMapping(poplar::VertexRef& vtxRef, uint32_t tileId);
    void setTileMapping(poplar::Tensor& tensor, uint32_t tileId);
    void connect(poplar::VertexRef& vtxRef, const std::string& vtxField, poplar::Tensor& tensor);
    void isHostRequest(poplar::Tensor& tensor);
    void createHostRead(const std::string& handleName, poplar::Tensor& tensor);
    void createHostWrite(const std::string& handleName, poplar::Tensor& tensor);
    void setPerfEstimate(poplar::VertexRef&, int) {}
    poplar::VertexRef getOrAddVertex(const std::string& name, bool isInit);

    poplar::Tensor addTensor(uint32_t size, const std::string& name);

    template <typename T>
    inline T getHostData(const std::string& handle, const T& /*unused*/) {
        static_assert(std::is_trivially_copy_assignable<T>());
        static_assert(std::is_trivially_copy_constructible<T>());
        // find the host buffer
        auto it = hbuffers.find(handle);
        if (it == hbuffers.end()) {
            std::cerr << "Can not find host handle " << handle << std::endl;
            std::exit(EXIT_FAILURE);
        }
        engine->readTensor(handle, it->second->buff.data(),
                           it->second->buff.data() + it->second->buff.size());

        return (*reinterpret_cast<T*>(it->second->buff.data()));
    }
};

#endif
