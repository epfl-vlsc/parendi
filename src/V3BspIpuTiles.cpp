// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Assign IPU tile numbers
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

#include "V3BspIpuTiles.h"

#include "V3Ast.h"
#include "V3InstrCount.h"

#include <V3File.h>
#include <libkahypar.h>
#include <limits>
#include <set>
#include <unordered_map>
VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

class PoplarSetTileAndWorkerId final {
private:
    uint32_t m_numAvailTiles;
    uint32_t m_numAvailWorkers;
    AstNetlist* m_netlistp;
    void doLocate(const std::vector<AstClass*> unlocated, uint32_t startTid) {
        if (unlocated.size() > m_numAvailTiles * m_numAvailWorkers) {
            m_netlistp->v3warn(UNOPT, "Not enough tiles, exceeding worker limit: There are  "
                                          << unlocated.size() << " parallel process but have only "
                                          << m_numAvailTiles << "*" << m_numAvailWorkers
                                          << " tiles*workers" << endl);
        }
        // simple tile assignment
        uint32_t maxTileId = 0;

        // start with 1 if multiple IPUs are used to enable profiling and increase the likelyhood
        // of fitting the exchange code.
        uint32_t tid = startTid;
        uint32_t wid = 0;
        for (AstClass* classp : unlocated) {
            maxTileId = std::max(maxTileId, tid);
            auto newFlag = classp->flag().withTileId(tid++).withWorkerId(wid);
            classp->flag(newFlag);
            if (tid == m_numAvailTiles) {
                tid = 0;
                wid++;
            }
        }
    }

    void forEachBspClass(std::function<void(AstClass*)>&& fn) {
        for (AstVarScope* vscp = m_netlistp->topScopep()->scopep()->varsp(); vscp;
             vscp = VN_CAST(vscp->nextp(), VarScope)) {
            AstClassRefDType* const clsRefDTypep = VN_CAST(vscp->dtypep(), ClassRefDType);
            if (!clsRefDTypep || !clsRefDTypep->classp()->flag().isBsp()) { continue; }
            AstClass* const classp = clsRefDTypep->classp();
            fn(classp);
        }
    }
    void fixTileCountAndPromoteToSupervisor() {
        uint32_t maxTileId = 0;
        uint32_t maxWorkerId = 0;
        forEachBspClass([&](AstClass* classp) {
            maxTileId = std::max(classp->flag().tileId(), maxTileId);
            maxWorkerId = std::max(classp->flag().workerId(), maxWorkerId);
        });
        // set the tile count, potentially lower than the requested tile count
        // by the user (i.e, --tiles)
        v3Global.opt.tiles(maxTileId + 1);  // this is needed later to pass to the runtime
        v3Global.opt.workers(maxWorkerId + 1);
        if (maxWorkerId == 0 && v3Global.opt.fIpuSupervisor()) {
            // optionally promote every class to a supervisor, it's good for performance
            UINFO(3, "Promoting all vertices to supervisors" << endl);
            forEachBspClass([](AstClass* classp) {
                classp->flag(classp->flag().append(VClassFlag::BSP_SUPERVISOR));
            });
        }
    }

public:
    explicit PoplarSetTileAndWorkerId(AstNetlist* netlistp) {
        // collect all the bsp classes that do not have tile or worker id
        // const auto numAvailTiles = V3Global.opt
        m_netlistp = netlistp;
        m_numAvailTiles = static_cast<uint32_t>(v3Global.opt.tiles());
        m_numAvailWorkers = static_cast<uint32_t>(v3Global.opt.workers());
        std::vector<AstClass*> unlocatedCompute;
        std::vector<AstClass*> unlocatedInit;
        forEachBspClass([&](AstClass* classp) {
            if (classp->flag().isBspInit())
                unlocatedInit.push_back(classp);
            else
                unlocatedCompute.push_back(classp);
        });

        const bool multiIpu = (unlocatedCompute.size() > v3Global.opt.tilesPerIpu() * v3Global.opt.workers());
        UASSERT(!multiIpu
                    || unlocatedCompute.size()
                           <= (v3Global.opt.tiles() - 1) * v3Global.opt.workers(),
                "need tile 0 to be empty, Is V3BspMerge broken?");
        const uint32_t startTileId = multiIpu ? 1 : 0;
        doLocate(unlocatedCompute, startTileId);
        doLocate(unlocatedInit, startTileId);
        fixTileCountAndPromoteToSupervisor();
    }
};

class PartitionAndAssignTileNumbers final {
private:
    // static constexpr int TILES_PER_IPU = 1472;

    AstNetlist* const m_netlistp;
    const std::vector<AstClass*> m_classesp;
    const int m_numIpusNeeded;
    std::vector<kahypar_hypernode_weight_t> m_nodeWeights;

    // STATE:
    // AstClass::user1()   -> hyper node index

    VNUser1InUse m_user1InUse;

    // using NetId = std::pair<int, int>;
    // @brief NetId holds the two endpoints of an exchange in an undirected fashion
    // The firstp is the class pointer to the end point with less-equal hypernode index
    // This way both connections s1.f1 = t1.f1 and t1.f2 = s2.f2 will have the same
    // id {s1, t1} given that s1 has a lower hypernode index.
    struct NetId {
        AstVar* const varp;
        AstClass *const sourcep, *const targetp;
        NetId(AstVar* varp, AstClass* sp, AstClass* tp)
            : varp{varp}
            , sourcep{sp}
            , targetp{tp} {}
    };

    struct NetIdHash {
        inline size_t operator()(const NetId& id) const {
            return std::hash<AstVar*>{}(id.varp);
            // size_t s = static_cast<size_t>(id.firstp->user1());
            // size_t t = static_cast<size_t>(id.secondp->user1());
            // UASSERT(s < (1ull << 32ull) && t < (1ull << 32ull), "bad hash");
            // return (t << 32ull) | s;
        }
    };
    struct NetIdEqual {
        inline bool operator()(const NetId& op1, const NetId& op2) const {
            return op1.varp == op2.varp;
        }
    };

    struct NetBuilder {
        std::unordered_map<NetId, int, NetIdHash, NetIdEqual> m_index;
        std::vector<int> m_weights;
        std::vector<std::set<int>> m_edgeNodes;

        std::vector<std::size_t> m_edgeIndex;
        std::vector<kahypar_hyperedge_id_t> m_hyperedges;

        bool tryIncr(NetId id, int v) {
            auto it = m_index.find(id);
            if (it != m_index.end()) {
                m_edgeNodes[it->second].insert(id.sourcep->user1());
                m_edgeNodes[it->second].insert(id.targetp->user1());
                return true;
            } else {
                return false;
            }
        }
        void mk(NetId id, int v) {
            if (!tryIncr(id, v)) {
                // this is a newly encountered hyper edge, assign an index to it
                const auto newIndex = m_weights.size();
                m_index.emplace(id, newIndex);  // cache the index
                m_edgeNodes.emplace_back();  // create a set of vertices on the edge
                // add the two end nodes to the hyperedge
                m_edgeNodes.back().insert(id.sourcep->user1());
                m_edgeNodes.back().insert(id.targetp->user1());
                // keep the weight, i.e., total number of words on the net
                m_weights.push_back(v);
            }
        }

        inline int baseCost(int fanout) {
            if (fanout == 1) {
                return 1194;
            } else if (fanout <= 8) {
                return 1254;
            } else if (fanout <= 16) {
                return 1264;
            } else if (fanout <= 32) {
                return 1289;
            } else if (fanout <= 64) {
                return 1322;
            } else {
                return 1325;
            }
        }
        inline int fanoutCost(int words, int fanout) { return baseCost(fanout) + words * 2; }
        void build() {
            // In a hypergraph with N edges, we have and edgeIndex array of size N + 1
            // that is used to index into a second array that contains the list of
            // node indeices on each edge.
            // edgeIndex = [i0, i2, i3, ... iN, iN+1]
            // edge      = [....] // length depends on the connectivity degree of the graph
            // nodesOnEdge(edgeId) = edge[edgeIndex[edgeId] : edgeIndex[edgeId + 1]]

            for (const auto& nodeSet : m_edgeNodes) {
                m_edgeIndex.push_back(m_hyperedges.size());
                std::copy(nodeSet.cbegin(), nodeSet.cend(), std::back_inserter(m_hyperedges));
            }
            m_edgeIndex.push_back(m_hyperedges.size());
            for (int ix = 0; ix < m_weights.size(); ix++) {
                int words = m_weights[ix];
                const int fanout = m_edgeNodes[ix].size() - 1;
                m_weights[ix] = fanoutCost(words, fanout);
            }
        }

        NetBuilder() = default;
    };

    void dumpHMetisGraphFile() {

        string filename = v3Global.debugFilename("hyperedges.hmetis");
        UINFO(5, "Dumping hmetis file " << filename << endl);
        std::unique_ptr<std::ofstream> ofs{V3File::new_ofstream(filename)};
        *ofs << m_netBuilder.m_edgeNodes.size() << " " << m_classesp.size() << " 1 " << endl;
        for (int ix = 0; ix < m_netBuilder.m_edgeNodes.size(); ix++) {
            *ofs << m_netBuilder.m_weights[ix] << " ";
            for (const auto node : m_netBuilder.m_edgeNodes[ix]) { *ofs << node << " "; }
            *ofs << std::endl;
        }
        ofs->close();
    }
    NetBuilder m_netBuilder;

    void recalculateTileMapping(const std::vector<AstClass*>& classesp) {}

    void mkHyperNodes() {
        AstNode::user1ClearTree();
        int nodeIndex = 0;
        for (AstClass* const classp : m_classesp) {
            uint32_t cost = 0;
            classp->foreach([this, &cost](AstCFunc* funcp) {
                if (funcp->name() == "nbaTop") { cost = V3InstrCount::count(funcp, false); }
            });
            // UASSERT(cost < std::numeric_limits::max<int>())
            UASSERT(cost <= static_cast<uint32_t>(std::numeric_limits<int>::max()),
                    "cost overflow");

            // m_nodeWeights.push_back(static_cast<int>(cost));
            m_nodeWeights.push_back(1);
            classp->user1(nodeIndex++);
        }
    }

    void mkHyperEdges() {
        AstCFunc* exchangep = nullptr;
        for (AstNode* nodep = m_netlistp->topScopep()->scopep()->blocksp(); nodep;
             nodep = nodep->nextp()) {
            AstCFunc* const cfuncp = VN_CAST(nodep, CFunc);
            if (!cfuncp) { continue; }
            if (cfuncp->name() == "exchange") {
                exchangep = cfuncp;
                break;
            }
        }
        UASSERT(exchangep, "did not find the 'exchange' function");
        auto getClassp = [](AstNode* nodep) {
            return VN_AS(VN_AS(nodep, MemberSel)->fromp()->dtypep(), ClassRefDType)->classp();
        };

        for (AstNode* nodep = exchangep->stmtsp(); nodep; nodep = nodep->nextp()) {
            UASSERT_OBJ(VN_IS(nodep, Assign), nodep, "expected simple assign");
            AstAssign* const assignp = VN_AS(nodep, Assign);
            const int payloadSize = assignp->lhsp()->dtypep()->arrayUnpackedElements()
                                    * assignp->lhsp()->dtypep()->widthWords();
            AstClass* const sourceClassp = getClassp(assignp->rhsp());
            AstClass* const targetClassp = getClassp(assignp->lhsp());
            AstVar* const varp = VN_AS(assignp->rhsp(), MemberSel)->varp();
            m_netBuilder.mk(NetId{varp, sourceClassp, targetClassp}, payloadSize);
        }

        m_netBuilder.build();
    }

    static uint32_t tileIpuId(uint32_t tileId) { return tileId / v3Global.opt.tilesPerIpu(); }

    std::vector<kahypar_hypernode_weight_t> getBlockWeights() {
        std::vector<kahypar_hypernode_weight_t> blockWeights(m_numIpusNeeded);
        std::fill(blockWeights.begin(), blockWeights.end(),
                  0 /* allow for a bit of slack, fix up later*/);
        for (AstClass* const classp : m_classesp) {
            const auto tileId = classp->flag().tileId();
            const auto ipuId = tileIpuId(tileId);
            blockWeights[ipuId]++;
        }

        if (debug() >= 3) {
            std::stringstream strb;
            for (int ipuId = 0; ipuId < m_numIpusNeeded; ipuId++) {
                strb << "IPU" << ipuId << " has weight " << blockWeights[ipuId] << endl;
            }
            UINFO(3, "IPU weigth:" << endl << strb.str());
        }
        return blockWeights;
    }

    void partition() {

        kahypar_context_t* kcontextp = kahypar_context_new();
        kahypar_configure_context_from_file(kcontextp, (v3Global.opt.getenvVERIPOPLAR_ROOT()
                                                        + "/include/vlpoplar/KaHyParConfig.ini")
                                                           .c_str());
        const kahypar_hypernode_id_t numNodes = m_nodeWeights.size();
        const kahypar_hyperedge_id_t numEdges = m_netBuilder.m_edgeIndex.size() - 1;
        const double imbalance = v3Global.opt.kahyparImbalance();
        const kahypar_partition_id_t k = m_numIpusNeeded;

        std::vector<kahypar_partition_id_t> partitions;
        partitions.resize(numNodes);
        std::fill(partitions.begin(), partitions.end(), -1);

        kahypar_hyperedge_weight_t objective;
        // Set the target weight on each block. This is crucial when the user provied
        // number of tiles does strictly covers a whole ipu (e.g., --tile 1500). Otherwise
        // we'll end up using more tiles than requested
        auto blockWeight = getBlockWeights();
        kahypar_set_custom_target_block_weights(k, blockWeight.data(), kcontextp);

        UINFO(3, "Starting KaHyPar partitioner " << std::endl);
        kahypar_partition(numNodes, numEdges, imbalance, k,
                          m_nodeWeights.data() /*uweighted nodes*/, m_netBuilder.m_weights.data(),
                          m_netBuilder.m_edgeIndex.data(), m_netBuilder.m_hyperedges.data(),
                          &objective, kcontextp, partitions.data());
        UINFO(3, "Objective: " << objective << endl);
        // KaHyPar may give us more vertices on some IPUs, since we want to exactly
        // v3Global.opt.workers() * TILES_PER_IPU vertices in each partition then
        // we may need to fix up the KaHyPar's solution
        std::vector<std::vector<AstClass*>> ipuNodes(m_numIpusNeeded);
        std::vector<AstClass*> overload;
        const int maxPartitionSize = v3Global.opt.tilesPerIpu() * v3Global.opt.workers();
        for (AstClass* const classp : m_classesp) {
            int ipuIndex = partitions[classp->user1()];
            if (ipuNodes[ipuIndex].size() < maxPartitionSize) {
                ipuNodes[ipuIndex].push_back(classp);
            } else {
                UINFO(3, "Overloaded partition " << ipuIndex << endl);
                overload.push_back(classp);
            }
        }
        for (AstClass* classp : overload) {
            bool assigned = false;
            int ipuId = 0;
            for (int ipuId = 0; ipuId < ipuNodes.size(); ipuId++) {
                if (ipuNodes[ipuId].size() < blockWeight[ipuId]) {
                    ipuNodes[ipuId].push_back(classp);
                    assigned = true;
                    break;
                }
            }
            UASSERT(assigned, "could not assign to any IPU");
        }
        int ipuId = 0;

        for (int ipuId = 0; ipuId < ipuNodes.size(); ipuId++) {
            int tileId = ipuId == 0 ? 1 : 0;
            int workerId = 0;
            const int tileIdBase = ipuId * v3Global.opt.tilesPerIpu();
            const int tilesInLastIpu = (v3Global.opt.tiles() % v3Global.opt.tilesPerIpu())
                                           ? (v3Global.opt.tiles() % v3Global.opt.tilesPerIpu())
                                           : v3Global.opt.tilesPerIpu();
            const int tileIdLen
                = (ipuId == ipuNodes.size() - 1) ? tilesInLastIpu : v3Global.opt.tilesPerIpu();
            for (AstClass* const classp : ipuNodes[ipuId]) {
                int newTileId = tileId + tileIdBase;
                UASSERT(newTileId < v3Global.opt.tiles(), "overflow in tileid");
                UASSERT(workerId < v3Global.opt.workers(), "overflow in workerid");
                VClassFlag flag = classp->flag();
                UINFO(10, "reassign (" << flag.tileId() << ", " << flag.workerId() << ") to ("
                                       << newTileId << "," << workerId << ")" << endl);
                classp->flag(flag.withTileId(newTileId).withWorkerId(workerId));
                if (tileId == tileIdLen - 1) {
                    workerId += 1;
                    tileId = 0;
                } else {
                    tileId++;
                }
            }
            UINFO(5, "Reassignment finished for IPU" << ipuId << endl);
        }
    }

    explicit PartitionAndAssignTileNumbers(AstNetlist* netlistp,
                                           const std::vector<AstClass*>& classesp,
                                           const int numIpusNeeded)
        : m_netlistp{netlistp}
        , m_classesp{classesp}
        , m_numIpusNeeded{numIpusNeeded} {

        mkHyperNodes();
        mkHyperEdges();
        if (dump() >= 5) { dumpHMetisGraphFile(); }
        partition();
    }

public:
    static inline void tryPartition(AstNetlist* netlistp) {

        const int numMaxTiles = v3Global.opt.tiles();
        const int numMaxWorkers = v3Global.opt.workers();
        std::vector<AstClass*> bspComputeClasses;

        uint32_t maxTileId = 0;

        for (AstVarScope* vscp = netlistp->topScopep()->scopep()->varsp(); vscp;
             vscp = VN_AS(vscp->nextp(), VarScope)) {
            AstClassRefDType* const clsDTypep = VN_CAST(vscp->dtypep(), ClassRefDType);
            if (!clsDTypep || !clsDTypep->classp()->flag().isBsp()) { continue; }
            AstClass* const classp = clsDTypep->classp();
            if (!classp->flag().isBspInit() && !classp->flag().isBspCond()) {
                bspComputeClasses.push_back(classp);
                maxTileId = std::max(maxTileId, clsDTypep->classp()->flag().tileId());
            }
        }
        const int numIpusUsed = tileIpuId(maxTileId) + 1;
        if (numIpusUsed > 1) {
            UINFO(3, "Optimizating inter-IPU communication over "
                         << numIpusUsed << " IPUs with " << maxTileId + 1 << " tiles " << endl);
            PartitionAndAssignTileNumbers{netlistp, bspComputeClasses, numIpusUsed};
        }
    }
};


}  // namespace

void V3BspIpuTiles::tileAll(AstNetlist* netlistp) {
    // first set the tile and worker ids, potentially also promote to supervisor threads
    PoplarSetTileAndWorkerId{netlistp};
    // now if there are more than one IPUs, perform a k-way partition to minimize inter-IPU
    // communication
    if (v3Global.opt.fInterIpuComm()) { PartitionAndAssignTileNumbers::tryPartition(netlistp); }
}