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

// #ifndef VERILATOR_POPLARCONTEXT_H_
// #define VERILATOR_POPLARCONTEXT_H_

#include <iostream>
#include <memory>
#include <unordered_map>

#include <poplar/CycleCount.hpp>
#include <poplar/DeviceManager.hpp>
#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>
#include <poplar/IPUModel.hpp>
#include <poplar/Tensor.hpp>
struct RuntimeConfig {};
class VlPoplarContext final {
private:
    struct HostBuffer {
        std::vector<uint32_t> buff;
    };
    RuntimeConfig cfg;
    std::unique_ptr<poplar::Device> device;
    std::unique_ptr<poplar::Graph> graph;
    std::unique_ptr<poplar::Engine> engine;
    std::unique_ptr<poplar::ComputeSet> workload;
    std::unique_ptr<poplar::ComputeSet> initializer;
    std::unordered_map<std::string, poplar::Tensor> tensors;
    std::unordered_map<std::string, std::unique_ptr<HostBuffer>> hbuffers;
    std::unordered_map<std::string, poplar::VertexRef> vertices;
    poplar::program::Sequence initCopies;
    poplar::program::Sequence exchangeCopies;

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
    void addCopy(const std::string& from, const std::string& to, uint32_t size, bool isInit);
    void setTileMapping(poplar::VertexRef& vtxRef, uint32_t tileId);
    void setTileMapping(poplar::Tensor& tensor, uint32_t tileId);
    void connect(poplar::VertexRef& vtxRef, const std::string& vtxField, poplar::Tensor& tensor);
    void createHostRead(const std::string& handleName, poplar::Tensor& tensor);
    void createHostWrite(const std::string& handleName, poplar::Tensor& tensor);

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

void VlPoplarContext::addCopy(const std::string& from, const std::string& to, uint32_t size,
                              bool isInit) {
    poplar::Tensor fromTensor = getTensor(from);
    poplar::Tensor toTensor = getTensor(to);
    poplar::program::Copy cp{fromTensor, toTensor, false, from + " ==> " + to};
    if (isInit) {
        initCopies.add(cp);
    } else {
        exchangeCopies.add(cp);
    }
}
void VlPoplarContext::setTileMapping(poplar::VertexRef& vtxRef, uint32_t tileId) {
    graph->setTileMapping(vtxRef, tileId);
}
void VlPoplarContext::setTileMapping(poplar::Tensor& tensor, uint32_t tileId) {
    graph->setTileMapping(tensor, tileId);
}
void VlPoplarContext::connect(poplar::VertexRef& vtx, const std::string& field,
                              poplar::Tensor& tensor) {
    graph->connect(vtx[field], tensor);
}
void VlPoplarContext::createHostRead(const std::string& handle, poplar::Tensor& tensor) {
    graph->createHostRead(handle, tensor);
}
void VlPoplarContext::createHostWrite(const std::string& handle, poplar::Tensor& tensor) {
    graph->createHostWrite(handle, tensor);
}

int main() { VProgram prog; }

// #endif
