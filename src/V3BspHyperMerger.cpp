// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Hypergraph partitioning merge of BSP fibers
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

#include "V3BspHyperMerger.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3BspMerger.h"
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

inline static uint32_t ways() {
    uint32_t nTiles = v3Global.opt.tiles();
    if (nTiles > v3Global.opt.tilesPerIpu()) {
        nTiles--;  // When multiple IPUs are used, keep the zeroth tile free
    }
    return nTiles * v3Global.opt.workers();
}

class BspHyperMerger final {
private:
    template <typename T>
    using HyperEdgeStore = std::vector<T>;
    template <typename T>
    using HyperNodeStore = std::vector<T>;

    using HyperNodeId = kahypar_hypernode_id_t;
    using HyperEdgeId = kahypar_hyperedge_id_t;
    using PartitionId = kahypar_partition_id_t;

    struct CompInfo {
        std::vector<HyperNodeId> users;
        uint32_t cost;
        HyperEdgeId m_id;  // use getter and setter
        inline bool isHyperedge() const { return users.size() > 1; }
        inline void id(HyperEdgeId v) { m_id = v; }
        inline HyperEdgeId id() const {
            UASSERT(isHyperedge(), "not a hyperedge!");
            return m_id;
        }
        CompInfo()
            : cost(0)
            , m_id(-1) {}
    };

    const VNUser1InUse m_user1InUse;
    const VNUser2InUse m_user2InUse;
    AstUser1Allocator<AstNode, CompInfo> m_compInfo;
    // STATE
    // AstNode::useru()   -> hyperedge metadata
    // AstVarScope::user1() -> true if counted in memory usage

    inline bool memoryUsageCounted(AstVarScope* vscp) { return vscp->user2(); }
    inline void memoryUsageMark(AstVarScope* vscp) { vscp->user2(true); }
    inline int memoryUsage(AstVarScope* vscp) {
        return vscp->dtypep()->arrayUnpackedElements() * vscp->dtypep()->widthWords();
    }
    inline void memoryUsageClearAll() { AstNode::user2ClearTree(); }
    inline void hypergraphMetaDataClear() { AstNode::user1ClearTree(); }

    void buildHypergraph(std::vector<std::unique_ptr<DepGraph>>& depGraphsp) {
        hypergraphMetaDataClear();
        auto initNodeStore = [&](auto& st) {
            st.resize(depGraphsp.size());
            std::fill_n(st.begin(), st.size(), 0);
        };
        HyperNodeStore<uint32_t> memUsage;
        HyperNodeStore<uint32_t> nodeDupCost;
        HyperNodeStore<uint32_t> nodeDupCostNorm;
        HyperNodeStore<uint32_t> nodeCost;
        HyperNodeStore<kahypar_hypernode_weight_t> hyperNodeWeights;
        initNodeStore(memUsage);
        initNodeStore(nodeDupCostNorm);
        initNodeStore(nodeCost);
        initNodeStore(nodeDupCost);
        initNodeStore(hyperNodeWeights);

        HyperEdgeStore<AstNode*> hyperEdgeAstNodesp;
        uint32_t sequentialCost = 0;
        for (HyperNodeId gix = 0; gix < depGraphsp.size(); gix++) {
            memoryUsageClearAll();
            auto& graphp = depGraphsp[gix];
            uint32_t totalCost = 0;
            uint32_t totalMem = 0;
            for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                if (CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp)) {
                    auto& info = m_compInfo(compp->nodep());
                    info.users.push_back(gix);
                    if (info.users.size() == 1) {
                        // compute the cost
                        info.cost = V3InstrCount::count(compp->nodep(), false);
                        sequentialCost += info.cost;
                    }
                    UASSERT(info.cost != 0, "zero cost AstNode?");
                    totalCost += info.cost;
                    if (info.users.size() == 2) {
                        // This is the second time we reach this node, hence the
                        // node is duplicated and we need to later create a hyperedge for it
                        info.id(hyperEdgeAstNodesp.size());
                        hyperEdgeAstNodesp.push_back(compp->nodep());
                        // NOTE: we assume the depGraphs are DAGs, so that in the same graph we
                        // never visit the same CompVertex twice. Therefore any second visit should
                        // be from another dependence graph (i.e., fiber).
                    }
                }
                ConstrDefVertex* const defp = dynamic_cast<ConstrDefVertex*>(vtxp);
                if (defp && defp->inEmpty() && !memoryUsageCounted(defp->vscp())) {
                    totalMem += memoryUsage(defp->vscp());
                    memoryUsageMark(defp->vscp());
                }
                ConstrCommitVertex* const commitp = dynamic_cast<ConstrCommitVertex*>(vtxp);
                if (commitp && !memoryUsageCounted(commitp->vscp())) {
                    totalMem += memoryUsage(commitp->vscp());
                    memoryUsageMark(commitp->vscp());
                }
            }
            memUsage[gix] = totalMem;
            nodeCost[gix] = totalCost;
        }

        // iterate over hyperedges and set the duplicate cost and node degrees
        // at the same time build the two arrays KaHyPar uses to represent the hyperedges
        // The first, hyperEdgePtr, is an N+1 size array (for N hyperedges) such that for any
        // hyperedge index i hyperEdgePtr[i] to hyperEdgePtr[i + 1] point to an interval (inclusive
        // and exlusive respectively) in the second array hyperEdges which contains the list of
        // hypernodes on the ith hyperedge.
        // I.e., hyperEdges[hyperEdgePtr[i]] to hyperEdges[hyperEdgePtr[i + 1] - 1] contains all
        // the hypernodes on the ith hyperedge.
        std::vector<std::size_t> hyperEdgePtr;  /// eptr in hmetis manual page 14
        std::vector<HyperEdgeId> hyperEdges;  /// eind in hmethis manual page 14
        std::vector<kahypar_hyperedge_weight_t> hyperEdgeWeights;

        for (AstNode* astNodep : hyperEdgeAstNodesp) {
            auto& info = m_compInfo(astNodep);
            UASSERT(info.isHyperedge(), "ill-constructed hyper edges");
            hyperEdgePtr.push_back(hyperEdges.size());
            for (const HyperNodeId hyperNode : info.users) {
                nodeDupCost[hyperNode] += info.cost;
                nodeDupCostNorm[hyperNode] += (info.cost / info.users.size());
                hyperEdges.push_back(hyperNode);
            }
            hyperEdgeWeights.push_back(info.cost);
        }
        hyperEdgePtr.push_back(hyperEdges.size());

        for (HyperNodeId id = 0; id < depGraphsp.size(); id++) {

            UASSERT(nodeDupCost[id] >= nodeDupCostNorm[id], "non-positive hypernode weight!");
            hyperNodeWeights[id] = nodeCost[id] - nodeDupCost[id] + nodeDupCostNorm[id];
        }

        // Call KaHyPar
        kahypar_context_t* kcontextp = kahypar_context_new();
        kahypar_configure_context_from_file(
            kcontextp,
            (v3Global.opt.getenvVERIPOPLAR_ROOT() + "/include/vlpoplar/KaHyParConfigMerge.ini")
                .c_str());
        const double imbalance = 0.03;
        const HyperNodeId numNodes = hyperNodeWeights.size();
        const HyperEdgeId numEdges = hyperEdgeWeights.size();
        std::vector<PartitionId> partitions;
        partitions.resize(numNodes);
        std::fill(partitions.begin(), partitions.end(), -1);

        if (debug() >= 0) {
            // clang-format off
            uint32_t unadjustedCost = std::accumulate(hyperNodeWeights.begin(), hyperNodeWeights.end(), 0);
            uint32_t maxCost = *std::max_element(hyperNodeWeights.begin(), hyperNodeWeights.end());
            UINFO(0, "\n\tSequential cost: " << sequentialCost <<
                     "\n\tmax cost:        " << maxCost        <<
                     "\n\tcost sum:        " << unadjustedCost <<
                     "\n\ttarget:          " << static_cast<float>(unadjustedCost) / ways() << endl);
            // clang-format on
            // dump hMetis file
            string filename = v3Global.debugFilename("hypergraph_merge.hmetis");
            UINFO(0, "Dumping hmetis file " << filename << endl);
            std::unique_ptr<std::ofstream> ofs{V3File::new_ofstream(filename)};
            *ofs << numEdges << " " << numNodes << " 11 " << std::endl;
            for (int i = 0; i < hyperEdgeWeights.size(); i++) {
                *ofs << hyperEdgeWeights[i] << " ";
                for (auto j = hyperEdgePtr[i]; j < hyperEdgePtr[i + 1]; j++) {
                    *ofs << hyperEdges[j] << " ";
                }
                *ofs << std::endl;
            }
            for (const auto w : hyperNodeWeights) { *ofs << w << std::endl; }
            ofs->close();
        }

        kahypar_hyperedge_weight_t objective;
        UINFO(3, "Starting KaHyPar partitioner " << std::endl);
        kahypar_partition(numNodes, numEdges, imbalance, ways(), hyperNodeWeights.data(),
                          hyperEdgeWeights.data(), hyperEdgePtr.data(), hyperEdges.data(),
                          &objective, kcontextp, partitions.data());
        UINFO(3, "Objective: " << objective << endl);

        std::vector<std::vector<std::size_t>> indicesTmp;
        indicesTmp.resize(ways());
        for (int i = 0; i < partitions.size(); i++) {
            auto pid = partitions[i];
            indicesTmp[pid].push_back(i);
        }

        // remove empty vectors from indicesTmp
        std::vector<std::vector<std::size_t>> indices;

        for (int i = 0; i < indicesTmp.size(); i++) {
            UINFO(5, "Checking partition " << i << " with " << indicesTmp.size() << " fibers "
                                           << endl);
            if (indicesTmp[i].size() > 0) {

                indices.emplace_back(std::move(indicesTmp[i]));
            } else {
                v3Global.rootp()->v3warn(UNOPTTHREADS,
                                         "Empty partition " << i << " by KaHyPar" << endl);
            }
        }
        if (indices.size() < ways()) {
            v3Global.rootp()->v3warn(UNOPTTHREADS, "Failed to reach the desired thread count "
                                                       << indices.size() << " < " << ways()
                                                       << endl);
        }

        V3BspMerger::merge(depGraphsp, indices);
    }

public:
    explicit BspHyperMerger(std::vector<std::unique_ptr<DepGraph>>& depGraphsp) {
        buildHypergraph(depGraphsp);
    }
};

};  // namespace
void V3BspHyperMerger::mergeAll(std::vector<std::unique_ptr<DepGraph>>& depGraphsp) {
    if (depGraphsp.empty() || depGraphsp.size() < ways()) {
        UINFO(3, "No need to merge fibers" << endl);
        return;
    }
    BspHyperMerger impl{depGraphsp};
}

}  // namespace V3BspSched
