// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Partition fibers across IPU devices
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

#include "config_build.h"
#include "verilatedos.h"

#include "V3BspIpuDevicePartitioning.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3InstrCount.h"

#include <V3File.h>
#include <algorithm>
#include <libkahypar.h>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_map>
VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3BspSched {

namespace {

static inline std::vector<kahypar_hypernode_weight_t>
fiberCountPerDevice(const IpuDevModel& devModel, std::size_t numFibers) {

    UASSERT(numFibers <= static_cast<std::size_t>(
                std::numeric_limits<kahypar_hypernode_weight_t>::max()),
            "overflow in fiber count!");
    const uint32_t numIpusUsed = devModel.numIpusUsed(static_cast<uint32_t>(numFibers));
    UASSERT(numIpusUsed > 1, "did not expect single IPU");

    auto usablesTiles = devModel.usableTilesPerDevice();

    int64_t fibersLeft = static_cast<int64_t>(numFibers);

    std::vector<kahypar_hypernode_weight_t> blocks(numIpusUsed);

    const float ratio = static_cast<float>(numFibers)
                        / static_cast<float>(std::accumulate(
                            usablesTiles.begin(), usablesTiles.begin() + numIpusUsed, 0));

    for (int i = 0; i < numIpusUsed; i++) {
        // use std::ceil to ensure that total block weights >= numFibers, i.e., allow some slack in
        // partitioning
        blocks[i] = static_cast<kahypar_hypernode_weight_t>(
            std::ceil(static_cast<float>(usablesTiles[i]) * ratio));
    }

    return blocks;
}
class DevicePartitionPreFiberMerge final {
private:
    const IpuDevModel& m_devModel;

    struct HyperEdgeMetaData {
        static constexpr kahypar_hypernode_id_t InvalidId
            = std::numeric_limits<kahypar_hypernode_id_t>::max();
        // no need to keep the producerId, but we just do that to make sure
        // that no variable has two writers: this is an error from previous
        // passes.
        kahypar_hypernode_id_t produerId = InvalidId;  // not really needed, used for sanity checks
        std::unordered_set<kahypar_hypernode_id_t>
            connectedNodeIds;  // later sort for stability across runs
        std::vector<kahypar_hypernode_id_t> m_nodesSorted;
        inline void addConnection(kahypar_hypernode_id_t id) {
            UASSERT(id != InvalidId, "Invalid hypernode id");
            connectedNodeIds.insert(id);
        }
        inline void setProducer(kahypar_hypernode_id_t id) {
            UASSERT(produerId == InvalidId, "multiple producer on the net? First by fiber "
                                                << produerId << " then by fiber " << id << endl);
            produerId = id;
        }
        std::vector<kahypar_hypernode_id_t>& nodes() {
            if (m_nodesSorted.size() != connectedNodeIds.size()) {
                m_nodesSorted.clear();
                for (auto n : connectedNodeIds) { m_nodesSorted.push_back(n); }
                std::stable_sort(m_nodesSorted.begin(), m_nodesSorted.end());
            }
            return m_nodesSorted;
        }
    };

    // STATE
    // VarScope::user1u()       -> hypreedge metadata
    VNUser1InUse m_user1InUse;
    AstUser1Allocator<AstVarScope, HyperEdgeMetaData> m_scoreboard;
    std::vector<AstVarScope*> m_hyperEdgeVscp;
    std::vector<kahypar_partition_id_t> m_partitionIds;

    void findHyperEdges(const std::unique_ptr<DepGraph>& fiberp, kahypar_hypernode_id_t fiberId) {
        for (V3GraphVertex* vtxp = fiberp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            if (ConstrCommitVertex* const commitp = dynamic_cast<ConstrCommitVertex*>(vtxp)) {
                AstVarScope* const vscp = commitp->vscp();
                UINFO(10, "Produced by fiber  " << fiberId << ": " << vscp->prettyNameQ() << endl);
                auto& varInfo = m_scoreboard(vscp);
                varInfo.setProducer(fiberId);
                varInfo.addConnection(fiberId);
                m_hyperEdgeVscp.push_back(vscp);
            } else if (ConstrDefVertex* const defp = dynamic_cast<ConstrDefVertex*>(vtxp)) {
                UINFO(10, "Consumed by fiber  " << fiberId << ": " << defp->vscp()->prettyNameQ()
                                                << endl);
                m_scoreboard(defp->vscp()).addConnection(fiberId);
            }
        }
    }
    void dumpHMetisGraphFile(const std::vector<kahypar_hypernode_weight_t>& nodeWeights,
                             const std::vector<kahypar_hyperedge_weight_t>& edgeWeights,
                             const std::vector<std::size_t> hyperEdgeIndexer,
                             const std::vector<kahypar_hyperedge_id_t>& hyperEdges) {

        string filename = v3Global.debugFilename("device_partition.hmetis");
        UINFO(5, "Dumping hmetis file " << filename << endl);
        std::unique_ptr<std::ofstream> ofs{V3File::new_ofstream(filename)};
        *ofs << edgeWeights.size() << " " << nodeWeights.size() << " 1 " << endl;
        for (std::size_t ix = 0; ix < edgeWeights.size(); ix++) {
            *ofs << edgeWeights[ix] << " ";
            for (std::size_t jx = hyperEdgeIndexer[ix]; jx < hyperEdgeIndexer[ix + 1]; jx++) {
                *ofs << hyperEdges[jx] << " ";
            }
            *ofs << std::endl;
        }
        for (const auto w : nodeWeights) { *ofs << w << std::endl; }
        ofs->close();
    }
    std::vector<V3BspIpuDevicePartitioning::PartitionResult>
    go(std::vector<std::unique_ptr<DepGraph>>& fibersp) {

        AstNode::user1ClearTree();

        for (kahypar_hypernode_id_t fiberId = 0; fiberId < fibersp.size(); fiberId++) {
            UINFO(10, "Finding hyperedges in fiber " << fiberId << endl);
            findHyperEdges(fibersp[fiberId], fiberId);
        }

        // construct the hyperedge adjacency array, see hmetis manual page 14
        // we use the hyperEdgeIndexer to index into an array that contains the
        // hypernodes on some hyperedge e:
        // hyperEdges[hyperEdgeIndexer[e]] ... hyperEdges[hyperEdgeIndexer[e + 1]] - 1
        // are the hypernodes on hyperedge e.
        std::vector<std::size_t> hyperEdgeIndexer;  // for E hyperedges, size it to E + 1
        std::vector<kahypar_hyperedge_id_t> hyperEdges;  // size E
        std::vector<kahypar_hyperedge_weight_t> hyperEdgeWeights;  // size E
        for (AstVarScope* const vscp : m_hyperEdgeVscp) {
            auto& info = m_scoreboard(vscp);
            hyperEdgeWeights.push_back(vscp->dtypep()->arrayUnpackedElements()
                                       * vscp->dtypep()->widthWords());
            UASSERT(info.nodes().size() >= 1, "Empty hyperedge " << vscp->prettyNameQ());
            if (info.nodes().size() == 1) {
                // strange, we expect at least 2: producers and consumer
                UINFO(3, "Hyperedge " << hyperEdgeIndexer.size() << " (" << vscp->prettyNameQ()
                                      << ") has " << info.connectedNodeIds.size() << " node "
                                      << info.nodes().front() << endl);
            }
            hyperEdgeIndexer.push_back(hyperEdges.size());
            for (const kahypar_hyperedge_id_t id : info.nodes()) { hyperEdges.push_back(id); }
        }
        hyperEdgeIndexer.push_back(hyperEdges.size());

        // set the weight for each IPU partition based on the model
        std::vector<kahypar_hypernode_weight_t> blockWeights
            = fiberCountPerDevice(m_devModel, fibersp.size());
        // We instruct KaHyPar to find a partition that contains
        // blocks[i] fibers on the ith IPU. This is needed especially when the last IPU is only
        // partially used (i.e., if the user asks us to use only some of the second, third,... IPU,
        // e.g., --tiles 1475 should use only 3 tiles from the second IPU when a single IPU has
        // 1472 tiles)
        std::unique_ptr<kahypar_context_t, std::function<void(kahypar_context_t*)>> kctxp{
            kahypar_context_new(), [](kahypar_context_t* p) { kahypar_context_free(p); }};
        kahypar_configure_context_from_file(kctxp.get(), (v3Global.opt.getenvVERIPOPLAR_ROOT()
                                                          + "/include/vlpoplar/KaHyParConfig.ini")
                                                             .c_str());

        const kahypar_hypernode_id_t numNodes = fibersp.size();
        const kahypar_hyperedge_id_t numEdges = hyperEdgeWeights.size();
        const double imbalance = v3Global.opt.kahyparImbalance();
        const kahypar_partition_id_t numDevices = blockWeights.size();
        if (debug() >= 3) {
            for (int i = 0; i < numDevices; i++) {
                UINFO(3, "Device " << i << " has weigth " << blockWeights[i] << endl);
            }
        }
        std::vector<kahypar_partition_id_t> partitionIds(
            numNodes);  // result vector: partitionId[fiberId] gives the device number of fiber
                        // with index fiberId after KaHyPar is done
        std::fill(partitionIds.begin(), partitionIds.end(), -1);

        kahypar_hyperedge_weight_t objective = -1;
        // instruct KaHyPar to come up with partitions of the given size
        kahypar_set_custom_target_block_weights(numDevices, blockWeights.data(), kctxp.get());
        UINFO(3, "Starting KaHyPar partitioner "
                     << "\n\t# HN = " << numNodes << " # HE = " << numEdges
                     << " ways = " << numDevices << endl);
        std::vector<kahypar_hypernode_weight_t> nodeWeights(numNodes);
        std::fill(nodeWeights.begin(), nodeWeights.end(), 1);  // unit weight

        if (debug() >= 3) {
            dumpHMetisGraphFile(nodeWeights, hyperEdgeWeights, hyperEdgeIndexer, hyperEdges);
        }
        kahypar_partition(numNodes, numEdges, imbalance, numDevices, nodeWeights.data(),
                          hyperEdgeWeights.data(), hyperEdgeIndexer.data(), hyperEdges.data(),
                          &objective, kctxp.get(), partitionIds.data());
        UINFO(3, "Objective = " << objective << endl);

        std::vector<V3BspIpuDevicePartitioning::PartitionResult> resultsp;
        auto usableTiles = m_devModel.usableTilesPerDevice();
        for (int i = 0; i < numDevices; i++) {
            UINFO(10, "IPU " << i << " usable tiles " << usableTiles[i] << endl);
            resultsp.emplace_back(usableTiles[i]);
        }
        for (int nodeId = 0; nodeId < numNodes; nodeId++) {
            auto partId = partitionIds[nodeId];
            UINFO(10, "fiber " << nodeId << " -> " << partId << endl);
            resultsp[partId].fibersp.emplace_back(std::move(fibersp[nodeId]));
        }

        UASSERT(std::all_of(fibersp.begin(), fibersp.end(), [](const auto& fp) { return !fp; }),
                "Some fiber not mapped to a device!");
        fibersp.clear();
        UINFO(3, "Finished device partitioning" << endl);
        return resultsp;
    }

public:
    inline std::vector<V3BspIpuDevicePartitioning::PartitionResult>
    operator()(std::vector<std::unique_ptr<DepGraph>>& fibersp) {
        return go(fibersp);
    }
    explicit DevicePartitionPreFiberMerge(const IpuDevModel& devModel)
        : m_devModel{devModel} {}
};

// Assigns tile and worker ids to the bsp classes
class IpuLinearPlacement final {
private:
    // AstNetlist* const m_netlistp;
    const IpuDevModel m_devModel;
    // std::vector<AstClass*> m_computeClassesp;
    // std::vector<AstClass*> m_controlOrInitClassesp;
    void setLocationLinearlySingleIpu(const std::vector<AstClass*> classesp) {
        uint32_t tileId = 0;
        uint32_t workerId = 0;
        for (AstClass* classp : classesp) {
            if (tileId == std::min(m_devModel.numAvailTiles, m_devModel.numTilesPerIpu)) {
                workerId++;
                tileId = 0;
            }
            auto newFlag = classp->flag().withTileId(tileId).withWorkerId(workerId);
            classp->flag(newFlag);
            tileId++;
        }
    }
    void setLocationsLinearlyMultiIpu(const std::vector<AstClass*> classesp) {

        // get the number of tiles (vertices) per each device
        const std::vector<uint32_t> tilesInEachDevice = m_devModel.usableTilesPerDevice();
        UINFO(3, "Linearly mapping " << classesp.size() << " BSP classes to tiles and workers."
                                     << "(Target device)" << endl
                                     << " # workers: " << m_devModel.numAvailWorkers
                                     << " # tiles: " << m_devModel.numAvailTiles
                                     << " # tiles per device: " << m_devModel.numTilesPerIpu
                                     << endl);
        std::vector<uint32_t> maxClassesInEachDevice;
        std::transform(tilesInEachDevice.begin(), tilesInEachDevice.end(),
                       std::back_inserter(maxClassesInEachDevice),
                       [this](uint32_t s) { return s * m_devModel.numAvailWorkers; });
        if (debug() >= 3) {
            for (int i = 0; i < tilesInEachDevice.size(); i++) {
                UINFO(3, "Usable tiles/workers on device " << i << ": " << tilesInEachDevice[i]
                                                           << "/" << maxClassesInEachDevice[i]
                                                           << endl);
            }
        }
        uint32_t totalCapacity
            = std::accumulate(maxClassesInEachDevice.begin(), maxClassesInEachDevice.end(), 0);
        UASSERT(totalCapacity * m_devModel.numAvailWorkers >= classesp.size(),
                "not enough device capacity!" << totalCapacity << " < " << classesp.size()
                                              << endl);

        auto assignIds = [&](const uint32_t beginIndex, const uint32_t endIndex,
                             const uint32_t deviceId) {
            const uint32_t tileOffset = deviceId * m_devModel.numTilesPerIpu;
            const uint32_t minTileId = maxClassesInEachDevice.size() > 1 ? 1 : 0;
            const uint32_t maxTiles = tilesInEachDevice[deviceId] + minTileId;
            uint32_t tileId = minTileId;
            uint32_t workerId = 0;
            UINFO(3, "Assigning ids for " << beginIndex << " to " << endIndex
                                          << " maxTiles=" << maxTiles << " minTileId=" << minTileId
                                          << " tileOffset=" << tileOffset << endl);
            for (uint32_t i = beginIndex; i < endIndex; i++) {
                if (tileId == maxTiles) {
                    workerId++;
                    // tileId = 0 in each IPU is reserved when multiple IPUs
                    // are used. We do this because the global exchange code could
                    // become quite huge and reserving one tile for it helps.
                    tileId = minTileId;
                }
                UASSERT(i < classesp.size(),
                        "Index out of bounds " << i << " " << classesp.size() << endl);
                AstClass* classp = classesp[i];
                auto newFlag
                    = classp->flag().withTileId(tileId + tileOffset).withWorkerId(workerId);
                classp->flag(newFlag);
                tileId++;
            }
        };
        uint32_t beginIndex = 0;
        for (uint32_t deviceId = 0; deviceId < maxClassesInEachDevice.size(); deviceId++) {
            if (beginIndex >= classesp.size()) {
                break;  // we may have more capacity than what is really needed
            }
            // take max, the last device might be partially used.
            const uint32_t endIndex = std::min(beginIndex + maxClassesInEachDevice[deviceId],
                                               static_cast<uint32_t>(classesp.size()));
            assignIds(beginIndex, endIndex, deviceId);
            beginIndex += maxClassesInEachDevice[deviceId];
        }
    }
    inline void promote(AstClass* classp) {
        classp->flag(classp->flag().append(VClassFlag::BSP_SUPERVISOR));
    }
    void tryPromoteAll(const std::vector<AstClass*> classesp) {
        uint32_t maxWokerId = 0;
        for (const AstClass* classp : classesp) {
            maxWokerId = std::max(maxWokerId, classp->flag().workerId());
        }
        if (maxWokerId == 0 && v3Global.opt.fIpuSupervisor()) {
            for (AstClass* classp : classesp) { promote(classp); }
        }
    }

public:
    explicit IpuLinearPlacement(AstNetlist* netlistp, const IpuDevModel& devModel)
        : m_devModel{devModel} {

        std::vector<AstClass*> unplaced;

        for (AstVarScope* vscp = netlistp->topScopep()->scopep()->varsp(); vscp;
             vscp = VN_CAST(vscp->nextp(), VarScope)) {
            AstClassRefDType* const clsDTypep = VN_CAST(vscp->dtypep(), ClassRefDType);
            if (!clsDTypep || !clsDTypep->classp()->flag().isBsp()) { continue; }
            AstClass* const classp = clsDTypep->classp();
            if (classp->flag().isBspCond() || classp->flag().isBspInit()) {
                classp->flag(classp->flag().withTileId(0).withWorkerId(0));

            } else {
                unplaced.push_back(classp);
            }
        }
        if (m_devModel.numIpusUsed(unplaced.size()) > 1) {
            setLocationsLinearlyMultiIpu(unplaced);
        } else {
            setLocationLinearlySingleIpu(unplaced);
        }
        tryPromoteAll(unplaced);
    }
};
}  // namespace

std::vector<V3BspIpuDevicePartitioning::PartitionResult>
V3BspIpuDevicePartitioning::partitionFibers(std::vector<std::unique_ptr<DepGraph>>& fibersp,
                                            const IpuDevModel& devModel) {
    UASSERT(fibersp.size() < std::numeric_limits<uint32_t>::max(),
            "Too many fibers " << fibersp.size() << " max supported is "
                               << std::numeric_limits<uint32_t>::max() << endl);
    UINFO(3, "#fiber= " << fibersp.size() << " #tile=" << devModel.numAvailTiles
                        << " #worker=" << devModel.numAvailWorkers
                        << " #ipuTiles=" << devModel.numTilesPerIpu << endl);
    uint32_t numFibersPostMerge = std::min(static_cast<uint32_t>(fibersp.size()),
                                           devModel.numAvailTiles * devModel.numAvailWorkers);
    if (devModel.numTilesPerIpu * devModel.numAvailWorkers >= numFibersPostMerge) {
        // a single IPU can hold all the fibers
        UINFO(3, "A single IPU is enough--skipping device partitioning" << endl);
        std::vector<PartitionResult> res;
        res.emplace_back(devModel.numAvailTiles);
        res.back().fibersp = std::move(fibersp);
        return res;
    }
    return DevicePartitionPreFiberMerge{devModel}(fibersp);
}

void V3BspIpuPlace::placeAll(AstNetlist* nodep, const IpuDevModel& devModel) {
    IpuLinearPlacement{nodep, devModel};
}
}  // namespace V3BspSched