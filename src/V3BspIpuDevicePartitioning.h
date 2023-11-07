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

#ifndef VERILATOR_V3BSPIPUDEVICEPARTITIONING_H_
#define VERILATOR_V3BSPIPUDEVICEPARTITIONING_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3BspGraph.h"
#include "V3Sched.h"

#include <climits>
#include <limits>
namespace V3BspSched {

struct IpuDevModel {
    // number of available workers per tile
    const uint32_t numAvailWorkers;
    // number of available tiles in the IPU system (may span multiple devices)
    const uint32_t numAvailTiles;
    // number of tiles in a single IPU
    const uint32_t numTilesPerIpu;

    /// returns the number of IPUs needed to host that many fibers
    inline uint32_t numIpusNeeded() const { return numIpusUsed(numAvailTiles * numAvailWorkers); }
    inline uint32_t numIpusUsed(uint32_t fiberCount) const {
        return (std::min(numAvailTiles * numAvailWorkers, fiberCount) - 1)
                   / (numTilesPerIpu * numAvailWorkers)
               + 1;
    }
    inline uint32_t usableAvailTiles() const {
        if (numIpusNeeded() == 1) { return numAvailTiles; }
        return numAvailTiles - numIpusNeeded();
    }
    inline std::vector<uint32_t> usableTilesPerDevice() const {
        uint32_t numDevs = numIpusNeeded();
        if (numDevs == 1) {
            // single  IPU can be fully used
            return {numAvailTiles};
        }
        // Reserve 1 tile on each IPU for exchange code, for large RTL designs the global
        // exchnage code
        // could become quite large and colocating it with actual computation will overflow the
        // instruction memory of tile 0, 2944, 4416, and 5888. Note that with so many tiles we
        // can afford to lose some resources, but may lead compilation failure if the user
        // defines and imaginary IPU that has only 1 tile per device, but who cares?
        UASSERT(numTilesPerIpu > 1, "need at least 2 tiles per IPU");
        uint32_t usableTilesPerIpu = numTilesPerIpu - 1;

        // The last IPU may get fewer tiles, depending on the numAvailTiles
        uint32_t usableTilesInTheLastIpu = usableAvailTiles() % usableTilesPerIpu;
        if (usableTilesInTheLastIpu == 0) usableTilesInTheLastIpu = usableTilesPerIpu;
        std::vector<uint32_t> usables;
        for (int i = 0; i < (numDevs - 1); i++) { usables.push_back(usableTilesPerIpu); }
        usables.push_back(usableTilesInTheLastIpu);
        return usables;
    }
    explicit IpuDevModel(uint32_t workers, uint32_t tiles, uint32_t tilesPerIpu)
        : numAvailWorkers(workers)
        , numAvailTiles(tiles)
        , numTilesPerIpu(tilesPerIpu) {}
    static IpuDevModel instance() {
        return IpuDevModel{static_cast<uint32_t>(v3Global.opt.workers()),
                           static_cast<uint32_t>(v3Global.opt.tiles()),
                           static_cast<uint32_t>(v3Global.opt.tilesPerIpu())};
    }
};
class V3BspIpuDevicePartitioning final {
public:
    struct PartitionResult {
        std::vector<std::unique_ptr<DepGraph>> fibersp;
        const uint32_t usableTiles;
        PartitionResult(uint32_t numTiles)
            : usableTiles(numTiles) {}
    };

    // partition fibers into IPU devices, assigning equal number of them to each available device.
    static std::vector<PartitionResult>
    partitionFibers(std::vector<std::unique_ptr<DepGraph>>& fibersp, const IpuDevModel& ipuDevice);
};

class V3BspIpuPlace final {
public:
    // assign tile and worker ids to Bsp classes, somewhat linked to the device partitioning from
    // above
    static void placeAll(AstNetlist* nodep, const IpuDevModel& devModel);
};
}  // namespace V3BspSched

#endif