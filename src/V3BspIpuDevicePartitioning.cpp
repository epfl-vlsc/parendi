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
#include <libkahypar.h>
#include <limits>
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
    const uint32_t numIpusNeeded = devModel.numIpusNeeded();
    UASSERT(numIpusNeeded > 1, "did not expect single IPU");
    // note that we may not be able to use all the available tiles because we reserve one
    // tile per IPU (in multi-ipu systems) for the global exchange code.
    auto usablesTiles = devModel.usableTilesPerDevice();
    auto usableAvailTiles = devModel.usableAvailTiles();
    std::vector<kahypar_hypernode_weight_t> blocks(numIpusNeeded);

    for (int i = 0; i < blocks.size(); i++) {
        float frac = static_cast<float>(usablesTiles[i]) / static_cast<float>(usableAvailTiles);
        frac *= static_cast<float>(numFibers);
        frac = std::ceil(frac);
        blocks[i] = static_cast<kahypar_hypernode_weight_t>(frac);
        UASSERT(blocks[i] > 0.0, "empty IPU " << i << " block" << endl);
        // note that the ceil ensures that block[0] + ...  + blocks[numIPusNeeded - 1] >= numFibers
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
        inline void addConnection(kahypar_hypernode_id_t id) {
            UASSERT(id != InvalidId, "Invalid hypernode id");
            connectedNodeIds.insert(id);
        }
        inline void setProducer(kahypar_hypernode_id_t id) {
            UASSERT(produerId == InvalidId, "multiple producer on the net? First by fiber "
                                                << produerId << " then by fiber " << id << endl);
            produerId = id;
        }
    };

    VNUser1InUse m_user1InUse;
    AstUser1Allocator<AstVarScope, HyperEdgeMetaData> m_scoreboard;
    std::vector<AstVarScope*> m_hyperEdgeVscp;
    std::vector<kahypar_partition_id_t> m_partitionIds;

    void findHyperEdges(const std::unique_ptr<DepGraph>& fiberp, kahypar_hypernode_id_t fiberId) {
        for (V3GraphVertex* vtxp = fiberp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            if (ConstrCommitVertex* const commitp = dynamic_cast<ConstrCommitVertex*>(vtxp)) {
                AstVarScope* const vscp = commitp->vscp();
                auto& varInfo = m_scoreboard(vscp);
                varInfo.setProducer(fiberId);
                varInfo.addConnection(fiberId);
                m_hyperEdgeVscp.push_back(vscp);
            } else if (ConstrDefVertex* const defp = dynamic_cast<ConstrDefVertex*>(vtxp)) {
                m_scoreboard(defp->vscp()).addConnection(fiberId);
            }
        }
    }

    std::vector<V3BspIpuDevicePartitioning::PartitionResult>
    go(std::vector<std::unique_ptr<DepGraph>>& fibersp) {

        AstNode::user1ClearTree();

        for (kahypar_hypernode_id_t fiberId = 0; fiberId < fibersp.size(); fiberId++) {
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
            hyperEdgeIndexer.push_back(hyperEdges.size());
            for (const kahypar_hyperedge_id_t id : info.connectedNodeIds) {
                hyperEdges.push_back(id);
            }
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
        kahypar_partition(numNodes, numEdges, imbalance, numDevices, nodeWeights.data(),
                          hyperEdgeWeights.data(), hyperEdgeIndexer.data(), hyperEdges.data(),
                          &objective, kctxp.get(), partitionIds.data());
        UINFO(3, "Objective = " << objective << endl);

        std::vector<V3BspIpuDevicePartitioning::PartitionResult> resultsp;
        auto usableTiles = m_devModel.usableTilesPerDevice();
        for (int i = 0; i < numDevices; i++) { resultsp.emplace_back(usableTiles[i]); }
        for (int nodeId = 0; nodeId < numNodes; nodeId++) {
            auto partId = partitionIds[nodeId];
            resultsp[partId].fibersp.emplace_back(std::move(fibersp[nodeId]));
        }

        partitionIds.clear();
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
class IpuPhysicalPlacement final {
private:
    AstNetlist* const m_netlistp;
    const IpuDevModel m_devModel;
    std::vector<AstClass*> m_computeClassesp;
    std::vector<AstClass*> m_controlOrInitClassesp;

    void setLocation(const std::vector<AstClass*> classesp) {

        auto numIpus = m_devModel.numIpusUsed(static_cast<uint32_t>(classesp.size()));
        
    }

public:
    explicit IpuPhysicalPlacement(AstNetlist* netlistp, const IpuDevModel& devModel)
        : m_netlistp{netlistp}
        , m_devModel{devModel} {
        for (AstVarScope* vscp = m_netlistp->topScopep()->scopep()->varsp(); vscp;
             vscp = VN_CAST(vscp->nextp(), VarScope)) {
            AstClassRefDType* const clsDTypep = VN_CAST(vscp->dtypep(), ClassRefDType);
            if (!clsDTypep || !clsDTypep->classp()->flag().isBsp()) { continue; }
            AstClass* const classp = clsDTypep->classp();
            if (classp->flag().isBspCond() || classp->flag().isBspInit()) {
                m_controlOrInitClassesp.push_back(classp);
            } else {
                m_computeClassesp.push_back(classp);
            }
        }
    }
};
}  // namespace

std::vector<V3BspIpuDevicePartitioning::PartitionResult>
V3BspIpuDevicePartitioning::partitionFibers(std::vector<std::unique_ptr<DepGraph>>& fibersp,
                                            const IpuDevModel& devModel) {
    UASSERT(fibersp.size() < std::numeric_limits<uint32_t>::max(),
            "Too many fibers " << fibersp.size() << " max supported is "
                               << std::numeric_limits<uint32_t>::max() << endl);
    uint32_t numFibersPostMerge = std::min(static_cast<uint32_t>(fibersp.size()),
                                           devModel.numAvailTiles * devModel.numAvailWorkers);
    if (devModel.numTilesPerIpu * devModel.numAvailWorkers >= numFibersPostMerge) {
        // a single IPU can hold all the fibers
        UINFO(3, "A single IPU is enough--skipping device partitioning");
        std::vector<PartitionResult> res;
        res.emplace_back(devModel.numAvailTiles);
        res.back().fibersp = std::move(fibersp);
        return res;
    }
    return DevicePartitionPreFiberMerge{devModel}(fibersp);
}
}  // namespace V3BspSched