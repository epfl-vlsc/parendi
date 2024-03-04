// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Prepartitiong the fibers into IPU devices
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

#ifndef VERILATOR_V3BSPDEVICEPARTITIONING_H_
#define VERILATOR_V3BSPDEVICEPARTITIONING_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3BspGraph.h"
#include "V3Sched.h"

#include <climits>
#include <limits>
#include <vector>

namespace V3BspSched {

// Keep using the IPU naming terminology, even for x64 CPUs where it doesn't
// make sense.

struct DevModel {
    virtual uint32_t numAvailWorkers() const { return 0; };
    virtual uint32_t numAvailTiles() const { return 0; };
    virtual uint32_t numTilesPerIpu() const { return 0; };
    virtual uint32_t numIpusUsed(uint32_t fiberCount) const { return 0; };
    virtual std::vector<uint32_t> usableTilesPerDevice() const { return {0}; };
};

struct IpuDevModel : DevModel {
    // number of available workers per tile
    uint32_t m_numAvailWorkers;
    // number of available tiles in the IPU system (may span multiple devices)
    uint32_t m_numAvailTiles;
    // number of tiles in a single IPU
    uint32_t m_numTilesPerIpu;

    virtual uint32_t numAvailWorkers() const { return m_numAvailWorkers; }
    virtual uint32_t numAvailTiles() const { return m_numAvailTiles; }
    virtual uint32_t numTilesPerIpu() const { return m_numTilesPerIpu; }

    /// returns the number of IPUs needed to host that many fibers
    virtual uint32_t numIpusNeeded() const {
        return numIpusUsed(m_numAvailTiles * m_numAvailWorkers);
    }

    virtual uint32_t numIpusUsed(uint32_t fiberCount) const {
        return (std::min(m_numAvailTiles * m_numAvailWorkers, fiberCount) - 1)
                   / (m_numTilesPerIpu * m_numAvailWorkers)
               + 1;
    }

    uint32_t usableAvailTiles() const {
        if (numIpusNeeded() == 1) { return m_numAvailTiles; }
        return m_numAvailTiles - numIpusNeeded();
    }

    virtual std::vector<uint32_t> usableTilesPerDevice() const {
        uint32_t numDevs = numIpusNeeded();
        if (numDevs == 1) {
            // single  IPU can be fully used
            return {m_numAvailTiles};
        }
        // Reserve 1 tile on each IPU for exchange code, for large RTL designs the global
        // exchnage code
        // could become quite large and colocating it with actual computation will overflow the
        // instruction memory of tile 0, 2944, 4416, and 5888. Note that with so many tiles we
        // can afford to lose some resources, but may lead compilation failure if the user
        // defines and imaginary IPU that has only 1 tile per device, but who cares?
        UASSERT(m_numTilesPerIpu > 1, "need at least 2 tiles per IPU");
        uint32_t usableTilesPerIpu = m_numTilesPerIpu - 1;

        // The last IPU may get fewer tiles, depending on the numAvailTiles
        uint32_t usableTilesInTheLastIpu = usableAvailTiles() % usableTilesPerIpu;
        if (usableTilesInTheLastIpu == 0) usableTilesInTheLastIpu = usableTilesPerIpu;
        std::vector<uint32_t> usables;
        for (int i = 0; i < (numDevs - 1); i++) { usables.push_back(usableTilesPerIpu); }
        usables.push_back(usableTilesInTheLastIpu);
        return usables;
    }

    explicit IpuDevModel(uint32_t workers, uint32_t tiles, uint32_t tilesPerIpu)
        : m_numAvailWorkers(workers)
        , m_numAvailTiles(tiles)
        , m_numTilesPerIpu(tilesPerIpu) {}

    static DevModel& instance() {
        static IpuDevModel singleton
            = IpuDevModel{static_cast<uint32_t>(v3Global.opt.workers()),
                          static_cast<uint32_t>(v3Global.opt.tiles()),
                          static_cast<uint32_t>(v3Global.opt.tilesPerIpu())};
        return singleton;
    }
};

struct CpuDevModel : DevModel {
    uint32_t m_numThreads;

    virtual uint32_t numAvailWorkers() const { return 1; }  // One worker per tile (core)
    virtual uint32_t numAvailTiles() const { return m_numThreads; }  // A tile == core
    virtual uint32_t numTilesPerIpu() const { return m_numThreads; } // Number of cores is number of tiles
    virtual uint32_t numIpusUsed(uint32_t fiberCount) const { return 1; } // Only one IPU
    virtual std::vector<uint32_t> usableTilesPerDevice() const { return {m_numThreads}; }

    explicit CpuDevModel(uint32_t numthreads)
        : m_numThreads(numthreads) {}

    static DevModel& instance() {
        static CpuDevModel singleton
            = CpuDevModel{static_cast<uint32_t>(v3Global.opt.openmp_threads())};
        return singleton;
    };
};

struct V3BspDevicePartitioning final {
public:
    struct PartitionResult {
        std::vector<std::unique_ptr<DepGraph>> fibersp;
        uint32_t usableTiles;
        PartitionResult(uint32_t numTiles)
            : usableTiles(numTiles) {}
    };

    // partition fibers into IPU devices, assigning equal number of them to each available
    // device.
    static std::vector<PartitionResult>
    partitionFibers(std::vector<std::unique_ptr<DepGraph>>& fibersp, const DevModel& devModel);
};

struct V3BspIpuPlace final {
public:
    // assign tile and worker ids to Bsp classes, somewhat linked to the device partitioning
    // from above
    static void placeAll(AstNetlist* nodep, const DevModel& devModel);
};

};  // namespace V3BspSched

#endif