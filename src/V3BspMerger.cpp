// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Merge and balance BSP partitions
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

#include "V3BspMerger.h"

#include "V3AstUserAllocator.h"
#include "V3BspDifferential.h"
#include "V3BspPliCheck.h"
#include "V3File.h"
#include "V3FunctionTraits.h"
#include "V3Hasher.h"
#include "V3InstrCount.h"
#include "V3PairingHeap.h"
#include "V3Stats.h"

VL_DEFINE_DEBUG_FUNCTIONS;
namespace V3BspSched {

class CoreVertex;
class ChannelEdge;
// using CostType = std::pair<uint32_t, uint32_t>;
struct CostType {
    uint32_t instrCount;
    uint32_t recvCount;
    uint32_t memWords;

    explicit CostType(uint32_t f, uint32_t s, uint32_t memWords)
        : instrCount{f}
        , recvCount{s}
        , memWords{memWords} {}
    CostType() = default;
    inline uint32_t sum() const {
        // what actually constitutes as the cost is the instruction count
        return instrCount;
    }

    friend inline bool operator<(const CostType& c1, const CostType& c2) {
        return c1.sum() < c2.sum();
    }
    friend inline bool operator>(const CostType& c1, const CostType& c2) {
        return c1.sum() > c2.sum();
    }
    friend inline bool operator==(const CostType& c1, const CostType& c2) {
        return c1.sum() == c2.sum();
    }
    friend inline bool operator<=(const CostType& c1, const CostType& c2) {
        return c1.sum() <= c2.sum();
    }
    friend inline bool operator>=(const CostType& c1, const CostType& c2) {
        return c1.sum() >= c2.sum();
    }
    friend inline std::ostream& operator<<(std::ostream& os, const CostType& c) {
        os << "Cost(" << c.sum() << ":" << c.instrCount << ", " << c.recvCount << ", "
           << c.memWords << ")";
        return os;
    }
    CostType percentile(double p) {
        return CostType{static_cast<uint32_t>(instrCount * p),
                        static_cast<uint32_t>(recvCount * p), memWords};
    }
    static CostType max() {
        return CostType{std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(),
                        0};
    }
    static CostType zero() { return CostType{0, 0, 0}; }
};
struct HeapKey {
    CoreVertex* corep;
    inline bool operator<(const HeapKey& other) const;
    inline void increase(CostType v);  // intentionally not defined and not used
};

using MinHeap = PairingHeap<HeapKey>;
using HeapNode = MinHeap::Node;

class MultiCoreGraph : public V3Graph {
public:
    inline void addEdge(CoreVertex* fromp, CoreVertex* top, uint32_t numWords);
};

class CoreVertex : public V3GraphVertex {
private:
    uint32_t m_instrCount = 0;
    uint32_t m_recvWords = 0;
    uint32_t m_memWords = 0;
    VlBitSet m_dupSet;
    VlBitSet m_dupVarSet;
    std::vector<int> m_partIndex;
    std::unique_ptr<HeapNode> m_heapNode;
    bool m_hasPli;

public:
    CoreVertex(MultiCoreGraph* graphp, size_t numDups, size_t numVarDups, std::vector<int>&& parts)
        : V3GraphVertex{graphp}
        , m_dupSet{numDups}
        , m_dupVarSet{numVarDups}
        , m_partIndex{parts}
        , m_heapNode(std::make_unique<HeapNode>()) {
        m_heapNode->m_key = HeapKey{.corep = this};
    }

    inline uint32_t instrCount() const { return m_instrCount; }
    inline uint32_t recvWords() const { return m_recvWords; }
    inline void instrCount(uint32_t v) { m_instrCount = v; }
    inline void recvWords(uint32_t v) { m_recvWords = v; }
    inline void memoryWords(uint32_t v) { m_memWords = v; }
    uint32_t memoryWords() const { return m_memWords; }
    inline std::vector<int>& partp() { return m_partIndex; }
    inline VlBitSet& dupSet() { return m_dupSet; }
    inline VlBitSet& dupVarSet() { return m_dupVarSet; }
    inline CostType cost() const { return CostType{instrCount(), recvWords(), memoryWords()}; }
    inline void heapNode(std::unique_ptr<HeapNode>&& n) { m_heapNode = std::move(n); }
    inline std::unique_ptr<HeapNode>& heapNode() { return m_heapNode; }
    inline bool hasPli() const { return m_hasPli; }
    inline void hasPli(bool v) { m_hasPli = v; }
    string partsString() const {
        std::stringstream ss;
        bool first = true;
        ss << "{";
        int num = 0;
        for (const auto i : m_partIndex) {
            if (!first) { ss << ", "; }
            first = false;
            ss << i;
            if (num >= 1) {
                ss << "...";
                break;
            }
            num++;
        }
        ss << "}(" << m_partIndex.size() << ")";
        return ss.str();
    }
    string name() const override final {
        return (partsString() + " " + cvtToStr(m_instrCount) + ", " + cvtToStr(m_recvWords));
    }
};

class ChannelEdge : public V3GraphEdge {
public:
    ChannelEdge(MultiCoreGraph* graphp, CoreVertex* fromp, CoreVertex* top, uint32_t numWords)
        : V3GraphEdge{graphp, fromp, top, static_cast<int>(numWords), false} {}
    uint32_t numWords() const { return static_cast<uint32_t>(weight()); }
    string dotLabel() const override final { return cvtToStr(numWords()); }
};

inline void MultiCoreGraph::addEdge(CoreVertex* fromp, CoreVertex* top, uint32_t numWords) {
    new ChannelEdge{this, fromp, top, numWords};
}
inline bool HeapKey::operator<(const HeapKey& other) const {
    // user greater to turn the max-heap into a min-heap
    return (corep->cost() >= other.corep->cost());
}

template <typename Fn>
static void iterVertex(V3Graph* const graphp, Fn&& fn) {
    using Traits = FunctionTraits<Fn>;
    using Arg = typename Traits::template arg<0>::type;
    for (V3GraphVertex *vtxp = graphp->verticesBeginp(), *nextp; vtxp; vtxp = nextp) {
        nextp = vtxp->verticesNextp();
        if (Arg const vp = dynamic_cast<Arg>(vtxp)) { fn(vp); }
    }
}
class PartitionMerger {
private:
    struct NodeInfo {
        size_t nodeIndex = -1;  // index into m_instrCount
        size_t nodeDupIndex = -1;  // index into m_dupInstrCount;
        bool hasDuplicates = false;
        bool visited = false;
    };
    struct NodeVtx {
        ConstrCommitVertex* commitp = nullptr;
        ConstrDefVertex* defp = nullptr;
        ConstrPostVertex* postp = nullptr;
        ConstrInitVertex* initp = nullptr;
        CompVertex* compp = nullptr;
    };
    const uint32_t m_targetTileCount;
    const uint32_t m_targetWorkerCount;
    inline uint32_t targetCoreCount() const { return m_targetTileCount * m_targetWorkerCount; }
    const VNUser1InUse m_user1InUse;
    const VNUser2InUse m_user2InUse;
    const VNUser3InUse m_user3Inuse;
    AstUser2Allocator<AstNode, NodeInfo> m_nodeInfo;
    AstUser3Allocator<AstNode, NodeVtx> m_nodeVtx;
    // STATE
    // AstVarScope::user1()  -> Producer partition index + 1 (0 means no producer)
    // AstNode::user1()      -> true if cost is computed
    // AstNode::user2u()      -> node information

    std::vector<uint32_t> m_dupInstrCount;
    std::vector<uint32_t> m_dupVarSize;
    std::vector<uint32_t> m_instrCount;
    std::unique_ptr<MultiCoreGraph> m_coreGraphp;
    MinHeap m_heap;

    std::vector<std::unique_ptr<DepGraph>> m_partitionsp;

    template <typename Fn>
    void iterEdges(CoreVertex* corep, GraphWay way, Fn&& fn) {
        using Traits = FunctionTraits<Fn>;
        using Arg = typename Traits::template arg<0>::type;
        for (V3GraphEdge *edgep = corep->beginp(way), *nextp; edgep; edgep = nextp) {
            nextp = edgep->nextp(way);
            if (Arg const ep = dynamic_cast<Arg>(edgep)) { fn(ep); }
        }
    }

    uint32_t cachedInstrCount(AstNode* nodep) const {
        size_t index = m_nodeInfo(nodep).nodeIndex;
        UASSERT_OBJ(index < m_instrCount.size(), nodep, "instrCount not set");
        return m_instrCount[index];
    }

    void buildMultiCoreGraph(const std::vector<std::unique_ptr<DepGraph>>& partitionsp) {

        AstNode::user1ClearTree();
        AstNode::user2ClearTree();
        size_t dupIndex = 0;
        size_t nodeIndex = 0;
        size_t varIndex = 0;
        size_t varDupIndex = 0;
        size_t numDupVars = 0;
        VDouble0 statsCostSeq;
        VDouble0 statsFiberSumCost;
        std::vector<uint32_t> totalCost;
        std::vector<uint32_t> totalMem;
        std::vector<bool> hasPli;
        totalCost.resize(partitionsp.size());
        totalMem.resize(partitionsp.size());
        hasPli.resize(partitionsp.size());
        std::fill_n(hasPli.begin(), hasPli.size(), false);
        for (int pix = 0; pix < partitionsp.size(); pix++) {

            std::unique_ptr<std::ofstream> ofsp;
            if (dump() >= 10) {
                ofsp = std::unique_ptr<std::ofstream>{V3File::new_ofstream(
                    v3Global.debugFilename("cost_" + cvtToStr(pix) + ".txt"))};
            }

            const auto& graphp = partitionsp[pix];
            uint32_t costAccum = 0;
            uint32_t memAccum = 0;

            iterVertex(graphp.get(), [&](AnyVertex* const vtxp) {
                if (ConstrCommitVertex* const commitp = dynamic_cast<ConstrCommitVertex*>(vtxp)) {
                    UASSERT(commitp->vscp(), "ConstrCommitVertex of nullptr");
                    UASSERT_OBJ(!commitp->vscp()->user1p(), commitp->vscp(),
                                "produced by multiple partitions "
                                    << commitp->vscp()->prettyNameQ() << endl);
                    // mark AstVarScope with the partition that produces it
                    commitp->vscp()->user1(pix + 1);
                    const uint32_t bytes
                        = commitp->vscp()->varp()->dtypep()->arrayUnpackedElements()
                          * commitp->vscp()->varp()->widthWords();
                    memAccum += bytes;
                }
                if (ConstrDefVertex* const constrp = dynamic_cast<ConstrDefVertex*>(vtxp)) {
                    UASSERT(constrp->vscp(), "Expected VarScope");
                    auto& infoRef = m_nodeInfo(constrp->vscp());
                    const uint32_t bytes
                        = constrp->vscp()->varp()->dtypep()->arrayUnpackedElements()
                          * constrp->vscp()->varp()->widthWords();
                    // calculating the real memory usage is difficult, since we need
                    // to know
                    if (constrp->inEmpty() /* only consider forever live variables*/
                        && constrp->vscp()->user1() != pix + 1 /*do not double count*/) {
                        memAccum += bytes;
                    }
                    if (!infoRef.visited) {
                        // first visit to this variable that may have duplicates across the graphs
                        infoRef.nodeIndex = varIndex;
                        infoRef.visited = true;
                        varIndex++;
                    } else if (infoRef.visited && !infoRef.hasDuplicates) {
                        infoRef.hasDuplicates = true;
                        infoRef.nodeDupIndex = varDupIndex;
                        m_dupVarSize.push_back(bytes);
                        varDupIndex++;
                    } else {
                        // do not care, third or later visits
                    }
                }
                // compute and cache the cost of each node
                if (CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp)) {
                    auto& infoRef = m_nodeInfo(compp->nodep());

                    if (PliCheck::check(compp->nodep())) { hasPli[pix] = true; }

                    if (!infoRef.visited) {
                        uint32_t numInstr = V3InstrCount::count(compp->nodep(), false, ofsp.get());
                        infoRef.visited = true;
                        infoRef.nodeIndex = nodeIndex;
                        m_instrCount.push_back(numInstr);
                        costAccum += numInstr;
                        nodeIndex += 1;
                    } else if (infoRef.visited && !infoRef.hasDuplicates) {
                        // the second visit, mark as duplicate
                        infoRef.hasDuplicates = true;
                        const uint32_t numInstr = cachedInstrCount(compp->nodep());
                        statsCostSeq += numInstr;
                        costAccum += numInstr;
                        m_dupInstrCount.push_back(numInstr);
                        infoRef.nodeDupIndex = dupIndex;
                        dupIndex += 1;
                    } else {
                        // don't care, third or later visits
                        const uint32_t numInstr = cachedInstrCount(compp->nodep());
                        costAccum += numInstr;
                    }
                }
            });
            totalCost[pix] = costAccum;
            totalMem[pix] = memAccum;
            statsFiberSumCost += costAccum;
            // iterVertex<DepGraph, AnyVertex>(graphp.get(), )
        }
        m_coreGraphp = std::make_unique<MultiCoreGraph>();
        std::vector<CoreVertex*> coresp;


        // some stats
        V3Stats::addStat("BspMerger, sequential cost", statsCostSeq);
        V3Stats::addStat("BspMerger, fibers total cost", statsFiberSumCost);
        // number of nodes that have duplicates
        const size_t numDups = m_dupInstrCount.size();
        const size_t numVarDups = m_dupVarSize.size();
        UINFO(3, "There are " << numDups << " nodes that have duplicates" << endl);
        V3Stats::addStat("BspMerger, nodes with duplicates ", numDups);
        V3Stats::addStat("BspMerger, variables with duplicates ", numVarDups);
        V3Stats::addStat("BspMerger, max cost", *std::max_element(totalCost.begin(), totalCost.end()));

        if (totalCost.size() >= 2) {
            V3Stats::addStat(
                    "BspMerger, median cost",
                    totalCost.size() % 2 == 0
                        ? (totalCost[totalCost.size() / 2] + totalCost[totalCost.size() / 2 - 1]) / 2
                        : totalCost[totalCost.size() / 2]);
        } else if (totalCost.size() == 1) {
            V3Stats::addStat("BspMerger, median cost", totalCost.front());
        }
        for (int pix = 0; pix < partitionsp.size(); pix++) {
            // now create a CoreVertex for each partition
            const auto& depGraphp = partitionsp[pix];
            CoreVertex* corep = new CoreVertex{m_coreGraphp.get(), numDups, numVarDups, {pix}};
            coresp.push_back(corep);
            corep->instrCount(totalCost[pix]);
            corep->memoryWords(totalMem[pix]);
            corep->hasPli(hasPli[pix]);

            // Fill-in the duplicate set within the core
            iterVertex(depGraphp.get(), [&](AnyVertex* const vtxp) {
                if (ConstrVertex* constrp = dynamic_cast<ConstrVertex*>(vtxp)) {
                    // set the variables that are duplicated
                    AstVarScope* const vscp = constrp->vscp();
                    auto& info = m_nodeInfo(vscp);
                    auto& varDups = corep->dupVarSet();
                    if (info.hasDuplicates) { varDups.insert(info.nodeDupIndex); }
                } else if (CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp)) {
                    // set the compute nodes that are duplicated
                    AstNode* const nodep = compp->nodep();
                    auto& info = m_nodeInfo(nodep);
                    auto& dups = corep->dupSet();
                    if (info.hasDuplicates) { dups.insert(info.nodeDupIndex); }
                }
            });
        }

        for (int pix = 0; pix < partitionsp.size(); pix++) {
            // create the edges between cores
            const auto& depGraphp = partitionsp[pix];
            CoreVertex* const corep = coresp[pix];
            iterVertex(depGraphp.get(), [&](ConstrDefVertex* const defp) {
                int producerIndexPlus1 = defp->vscp()->user1();
                if (producerIndexPlus1 != 0
                    && producerIndexPlus1 != pix + 1 /*producer is self*/) {
                    const auto& producerCorep = coresp[producerIndexPlus1 - 1];
                    AstNodeDType* const dtypep = defp->vscp()->dtypep();
                    // Consider future optimizations. Not a perfect...
                    const uint32_t numWords
                        = v3Global.opt.fIpuDiffExchnage()
                              ? V3BspDifferential::countWords(dtypep)
                              : dtypep->arrayUnpackedElements() * dtypep->widthWords();
                    // create an edge from producerCore to corep, note that this will
                    // create possibly many edges between two cores and we need to collapse
                    // them into one later
                    m_coreGraphp->addEdge(producerCorep, corep, 0);
                }  // else local production or comb logic production
            });
        }
        // almost done, remove redundant edges
        m_coreGraphp->removeRedundantEdgesSum(V3GraphEdge::followAlwaysTrue);

        // now iter the cores and sum up the weigths on input edges
        iterVertex(m_coreGraphp.get(), [](CoreVertex* corep) {
            uint32_t totalRecv = 0;
            for (V3GraphEdge* edgep = corep->inBeginp(); edgep; edgep = edgep->inNextp()) {
                // UASSERT(edgep->weight(), "zero weigth edge!");
                totalRecv += static_cast<uint32_t>(edgep->weight());
            }
            corep->recvWords(totalRecv);
        });

        if (dumpGraph() >= 5) { m_coreGraphp->dumpDotFilePrefixed("multicore"); }
    }
    // compute the cost of merging
    CostType costAfterMerge(CoreVertex* core1p, CoreVertex* core2p) const {

        const uint32_t rawInstrCost = core1p->instrCount() + core2p->instrCount();
        const uint32_t rawRecvCost = core1p->recvWords() + core2p->recvWords();
        const uint32_t rawMemWords = core1p->memoryWords() + core2p->memoryWords();

        auto sumRecvFrom = [](CoreVertex* recvp, CoreVertex* sendp) {
            uint32_t sum = 0;
            for (V3GraphEdge* edgep = recvp->inBeginp(); edgep; edgep = edgep->inNextp()) {
                if (edgep->fromp() == sendp) { sum += static_cast<uint32_t>(edgep->weight()); }
            }
            return sum;
        };
        const uint32_t recvReduction = sumRecvFrom(core1p, core2p) + sumRecvFrom(core2p, core1p);

        // compute the duplicatation instruction count between the two core
        uint32_t dupCostCommon = 0;
        if (!v3Global.opt.ipuMergeStrategy().ignoreDupCost()) {
            VlBitSet dupInCommon = VlBitSet::doIntersect(core1p->dupSet(), core2p->dupSet());
            dupInCommon.foreach([&](size_t dupIx) { dupCostCommon += m_dupInstrCount[dupIx]; });
        }

        // compute the variable duplication count between the two cores
        VlBitSet varDupInCommon = VlBitSet::doIntersect(core1p->dupVarSet(), core2p->dupVarSet());
        uint32_t dupVarCostCommon = 0;
        varDupInCommon.foreach([&](size_t dupIx) { dupVarCostCommon += m_dupVarSize[dupIx]; });

        UASSERT(rawInstrCost >= dupCostCommon, "invalid instr cost computation");
        UASSERT(rawRecvCost >= recvReduction, "invalid recv cost computation");
        const uint32_t mergedCost = rawInstrCost - dupCostCommon;
        const uint32_t mergedRecvCost = rawRecvCost - recvReduction;
        // the following assertion does not need to hold since we only model the
        // cost of always-live variables and duplications occur in temporary variables
        // UASSERT(rawMemWords >= dupVarCostCommon, "invalid mem byte computation");

        return CostType{mergedCost, mergedRecvCost, rawMemWords};
    }
    // merger core1p and core2p
    // complexity should be amortized O(max(log V, E))

    void doMergeKeepHeap(CoreVertex* core1p, CoreVertex* core2p,
                         CostType newCost = CostType{0, 0, 0}) {
        if (newCost == CostType{0, 0, 0}) { newCost = costAfterMerge(core1p, core2p); }

        UINFO(10, "merging " << core1p->partsString() << " and " << core2p->partsString() << endl);

        // push core2p into core1p
        std::vector<int> parts{core1p->partp()};

        for (const auto p : core2p->partp()) { core1p->partp().push_back(p); }

        core1p->dupSet().unionInPlace(core2p->dupSet());
        core1p->dupVarSet().unionInPlace(core2p->dupVarSet());
        core1p->memoryWords(newCost.memWords);
        core1p->instrCount(newCost.instrCount);
        core1p->recvWords(newCost.recvCount);
        // connect every in/out edge core2p to core1p (careful not to make redundant
        // edges)
        auto deleteReconnect = [&](GraphWay way) {
            // set userp to any core connected to core1p
            for (V3GraphEdge* edgep = core1p->beginp(way); edgep; edgep = edgep->nextp(way)) {
                edgep->furtherp(way)->userp(edgep);
            }
            for (V3GraphEdge *edgep = core2p->beginp(way), *nextp; edgep; edgep = nextp) {
                nextp = edgep->nextp(way);
                CoreVertex* const furtherp = reinterpret_cast<CoreVertex*>(edgep->furtherp(way));
                if (furtherp != core1p) {
                    if (edgep->furtherp(way)
                            ->userp()) { /*furtherp is connected to both core1p and core2p*/
                        // fromp -w1-> core2p & fromp -w2-> core1p (or reverse direction)
                        // should become
                        // fromp -w1+w2-> {core1p, core2p}
                        V3GraphEdge* otherEdgep
                            = reinterpret_cast<V3GraphEdge*>(edgep->furtherp(way)->userp());
                        int sumWeight = otherEdgep->weight() + edgep->weight();
                        otherEdgep->weight(sumWeight);
                    } else {
                        // fromp -w-> core2p (or reverse direction)
                        // becomes fromp -w-> core1p
                        CoreVertex* const furtherp
                            = reinterpret_cast<CoreVertex*>(edgep->furtherp(way));

                        if (way == GraphWay::REVERSE) {
                            m_coreGraphp->addEdge(furtherp, core1p,
                                                  static_cast<uint32_t>(edgep->weight()));
                        } else if (way == GraphWay::FORWARD) {
                            m_coreGraphp->addEdge(core1p, furtherp,
                                                  static_cast<uint32_t>(edgep->weight()));
                        }
                    }
                } else {
                    // nothing to do, let the edge disappear
                }
                // delete the edge, no longer relevant
                VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
            }
            // clear userp
            for (V3GraphEdge* edgep = core1p->beginp(way); edgep; edgep = edgep->nextp(way)) {
                edgep->furtherp(way)->userp(nullptr);
            }
        };
        deleteReconnect(GraphWay::FORWARD);
        deleteReconnect(GraphWay::REVERSE);
    }
    void doMerge(CoreVertex* core1p, CoreVertex* core2p, CostType newCost = CostType{0, 0, 0}) {
        if (newCost == CostType{0, 0, 0}) { newCost = costAfterMerge(core1p, core2p); }

        UINFO(10, "merging " << core1p->partsString() << " and " << core2p->partsString() << endl);
        // remove both cores from the min-heap
        // O(log V) * 2
        m_heap.remove(core1p->heapNode().get());
        m_heap.remove(core2p->heapNode().get());
        doMergeKeepHeap(core1p, core2p, newCost);
        // O(1)
        m_heap.insert(core1p->heapNode().get());
        VL_DO_DANGLING(core2p->unlinkDelete(m_coreGraphp.get()), core2p);
    }

    std::vector<CostType> gatherCost() {
        std::vector<CostType> coreCost;
        iterVertex(m_coreGraphp.get(),
                   [&coreCost](CoreVertex* const corep) { coreCost.push_back(corep->cost()); });
        return coreCost;
    }

    uint32_t maxMemoryWords() const {
        return static_cast<uint32_t>(v3Global.opt.ipuMemoryPerTile() / 8);
    }

    bool isFeasible(CostType& cost) const { return cost.memWords <= maxMemoryWords(); }
    bool isInHeap(CoreVertex* corep) const { return corep->heapNode()->m_ownerpp; }

    uint32_t mergeConservatively() {
        UASSERT(m_heap.empty(), "heap should be empty");
        uint32_t numMerges = 0;
        std::vector<CostType> coreCost = gatherCost();
        uint32_t numCores = coreCost.size();
        if (numCores <= targetCoreCount()) {
            // no need to merge
            return 0;
        }

        std::stable_sort(coreCost.begin(), coreCost.end());

        CostType worstCost = coreCost.back();

        worstCost = worstCost.percentile(v3Global.opt.ipuMergeStrategy().threshold());
        UINFO(3, "Max permissible cost is "
                     << worstCost << " and the max absolute cost is " << coreCost.back()
                     << "(threshold = " << v3Global.opt.ipuMergeStrategy().threshold() << ")"
                     << endl);
        if (!(worstCost > CostType::zero())) {
            UINFO(3, "Conservative merge is not possible" << endl);
            // Do not attempt to merge since we may end up increasing the execution time.
            // Let the next stage of merging take care of this.
            // We do this to deal with the inaccuracy of the cost model.
            return 0;
        }
        iterVertex(m_coreGraphp.get(), [&](CoreVertex* corep) {
            if (!corep->hasPli()) {
                UINFO(10, "Adding core " << corep->name() << " to the heap " << endl);
                m_heap.insert(corep->heapNode().get());
                UASSERT(corep->heapNode()->m_ownerpp, "no ownerpp");
            } else {
                UINFO(10, "Will not merge " << corep->name() << " for now" << endl);
            }
        });

        HeapNode* minNodep = m_heap.max();
        bool didSomething = true;
        // Conservatively merge: avoid an increase to the critical path
        while ((numCores > targetCoreCount() || didSomething) && !m_heap.empty() && minNodep
               && minNodep->key().corep->cost() <= worstCost) {
            // try merging minNodep with a neighbor
            CoreVertex* bestNeighbor = nullptr;
            CostType bestCost = CostType::max();
            CoreVertex* corep = minNodep->key().corep;
            auto visitNeighbor = [&](GraphWay way) {
                iterEdges(corep, way, [&](V3GraphEdge* edgep) {
                    CoreVertex* const neighbor = dynamic_cast<CoreVertex*>(edgep->furtherp(way));
                    CostType newCost = costAfterMerge(corep, neighbor);
                    // update cost if neighbor does not have pli has offers a better cost that
                    // already found
                    if (isInHeap(neighbor) && isFeasible(newCost) && newCost < worstCost
                        && newCost < bestCost) {
                        bestNeighbor = neighbor;
                        bestCost = newCost;
                    }
                });
            };
            UINFO(8, "inspecting  " << corep->partsString() << " " << corep->cost() << endl);
            // there are probably more inEdges than outEdges, so maybe we could merge
            // on outEdges to make a it a bit faster. Currently do both to find the better
            // candidate
            visitNeighbor(GraphWay::REVERSE);  // iter inEdges
            visitNeighbor(GraphWay::FORWARD);  // iter outEdges
            HeapNode* const secondMinNodep = m_heap.secondMax();
            CostType costWithNext = CostType::max();
            if (secondMinNodep) {
                costWithNext = costAfterMerge(corep, secondMinNodep->key().corep);
            }
            // allow mergin below the desired core count if the user wants it.
            didSomething = v3Global.opt.ipuMergeStrategy().minimizeTileCount();
            if (bestNeighbor && isFeasible(bestCost)
                && (bestCost < costWithNext || !secondMinNodep || !isFeasible(costWithNext))) {
                // found a neighbor, merge it
                UINFO(8, "Merging with neighbor: " << bestCost << endl);
                doMerge(corep, bestNeighbor, bestCost);
                UASSERT(numCores > 1, "numCores underflowed");
                numCores--;
                numMerges++;
            } else if (secondMinNodep && isFeasible(costWithNext) && costWithNext < worstCost) {
                // did not find the neighbor, try the next min key
                CoreVertex* otherCorep = secondMinNodep->key().corep;
                UINFO(8, "Merging with next the smallest core: " << costWithNext << endl);
                doMerge(corep, otherCorep, costWithNext);
                UASSERT(numCores > 1, "numCores underflowed");
                numCores--;
                numMerges++;

            } else {
                // tough luck, cannot merge minNodep with anything, discard it
                UINFO(8, "Could not merge" << endl);
                m_heap.remove(minNodep);
                didSomething = false;
                // UASSERT(!m_heap.max(), "expected empty heap");
            }
            minNodep = m_heap.max();
        }
        // We have done our best not to increase the critical latency. Hopefully we do not need to
        // merge any further

        // clean up
        while (!m_heap.empty()) {
            minNodep = m_heap.max();
            m_heap.remove(minNodep);
        }

        if (dumpGraph() >= 5) {
            m_coreGraphp->dumpDotFilePrefixed("multicore_conservative_final");
        }
        UINFO(3, "Finished conservative merge" << endl);
        return numMerges;
    }

    uint32_t mergeForced() {

        UASSERT(m_heap.empty(), "heap should be empty");
        // get the current cost estimates
        std::vector<CostType> coreCost = gatherCost();
        uint32_t numCores = coreCost.size();
        if (numCores <= targetCoreCount()) {
            return 0;  // nothing to do
        }

        iterVertex(m_coreGraphp.get(), [&](CoreVertex* corep) {
            // PLI or not, add it to the heap
            m_heap.insert(corep->heapNode().get());
        });

        uint32_t numMerges = 0;
        std::stable_sort(coreCost.begin(), coreCost.end());
        CostType currentWorst = coreCost.back();

        UINFO(8, "Forcing merge with worst cost " << currentWorst << " and " << numCores
                                                  << " cores to have target core count "
                                                  << targetCoreCount() << endl);
        // merge smallest with a neighbor that yeilds the smallest cost

        while (numCores > targetCoreCount() && !m_heap.empty()) {

            // try merging minNodep with a neighbor
            HeapNode* minNodep = m_heap.max();  // max is actually min
            CoreVertex* bestNeighborp = nullptr;
            CostType bestCost = CostType::max();
            CoreVertex* corep = minNodep->key().corep;
            auto visitNeighbor = [&](GraphWay way) {
                iterEdges(corep, way, [&](V3GraphEdge* edgep) {
                    CoreVertex* const neighborp = dynamic_cast<CoreVertex*>(edgep->furtherp(way));
                    CostType newCost = costAfterMerge(corep, neighborp);
                    if (isInHeap(neighborp) && isFeasible(newCost) && newCost < bestCost) {
                        bestNeighborp = neighborp;
                        bestCost = newCost;
                    }
                });
            };
            visitNeighbor(GraphWay::FORWARD);
            visitNeighbor(GraphWay::REVERSE);
            HeapNode* const secondMinNodep = m_heap.secondMax();
            CostType costWithNext = CostType::max();
            if (secondMinNodep) {
                costWithNext = costAfterMerge(corep, secondMinNodep->key().corep);
            }
            auto applyMerge = [&](CoreVertex* otherp, const CostType& newCost) {
                doMerge(corep, otherp, newCost);
                if (newCost > currentWorst) {
                    currentWorst = newCost;
                    UINFO(6, "Increasing cost " << currentWorst << endl);
                }
                numMerges++;
                UASSERT(numCores > 1, "underflow");
                numCores--;
            };
            if (bestNeighborp && isFeasible(bestCost)
                && (bestCost < costWithNext || !secondMinNodep || !isFeasible(costWithNext))) {
                UINFO(8, "Merging with neighbor core givs "
                             << bestCost << " current = " << currentWorst << endl);
                applyMerge(bestNeighborp, bestCost);
            } else if (secondMinNodep && isFeasible(costWithNext)) {
                // there were no neighbors, merge with the next in the heap
                CoreVertex* otherCorep = secondMinNodep->key().corep;
                UINFO(8, "Merging with next in line " << costWithNext
                                                      << " current = " << currentWorst << endl);
                applyMerge(otherCorep, costWithNext);
            } else {
                // something is up
                // could not merge this node, remove it and continue.
                m_heap.remove(minNodep);
                UINFO(4, "Could not merge node with neither neighbors nor the next inline, "
                         "perhaps low on memory? next = "
                             << costWithNext << " neighbor = " << bestCost << endl);
            }
        }

        if (numCores > targetCoreCount()) {
            v3Global.rootp()->v3fatal("Could not reach the desired core count! "
                                      << numCores << " > " << targetCoreCount() << endl
                                      << "Probably ran out of IPU memory...");
        }
        // UASSERT(numCores <= targetCoreCount(), "Could not reach desired count "
        //                                            << targetCoreCount() << " > " << numCores
        //                                            << endl);
        // clean up
        while (!m_heap.empty()) { m_heap.remove(m_heap.max()); }

        if (dumpGraph() >= 5) { m_coreGraphp->dumpDotFilePrefixed("multicore_forced_final"); }
        return numMerges;
    }

    /// Longest-processing-time-first merge (scheduling) oblivious to communication

    uint32_t mergeLongestProcessingTimeFirst() {

        UASSERT(m_heap.empty(), "heap should be empty");
        // get the current cost estimates
        std::vector<CostType> coreCost = gatherCost();
        uint32_t numCores = coreCost.size();

        if (numCores <= targetCoreCount()) {
            UINFO(3, "Nothing to merge LPTF!" << endl);
            return 0;  // nothing to do
        }

        std::vector<CoreVertex*> coresLeftp;
        iterVertex(m_coreGraphp.get(), [&](CoreVertex* corep) {
            coresLeftp.push_back(corep);
            auto deleteEdge = [corep](GraphWay way) {
                for (V3GraphEdge *edgep = corep->beginp(way), *nextp; edgep; edgep = nextp) {
                    nextp = edgep->nextp(way);
                    VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
                }
            };
            // delete the edges since we are not using them anymore
            deleteEdge(GraphWay::FORWARD);
            deleteEdge(GraphWay::REVERSE);
        });
        // delete edges, not needed

        uint32_t numMerges = 0;
        // sort in increasing order of execution time
        std::stable_sort(coresLeftp.begin(), coresLeftp.end(),
                         [](CoreVertex* const c1p, CoreVertex* const c2p) {
                             return c1p->cost() < c2p->cost();
                         });

        std::vector<CoreVertex*> placesp;
        UASSERT(coresLeftp.size() > targetCoreCount(), "unexpected merge state");
        // first popluate the places with the largest cores
        for (int i = 0; i < targetCoreCount(); i++) {
            m_heap.insert(coresLeftp.back()->heapNode().get());
            coresLeftp.pop_back();
        }
        VDouble0 statsReinsertions;
        std::vector<HeapNode*> reinsertionList;
        float maxNumberOfSteps = static_cast<float>(coresLeftp.size());
        float lastProgress = 0.0f;
        while (coresLeftp.size() != 0) {
            // nothing to merge
            if (debug() >= 3) {
                const float progress = 100.0
                                       * (maxNumberOfSteps - static_cast<float>(coresLeftp.size()))
                                       / maxNumberOfSteps;
                if (progress - lastProgress >= 10.0f) {
                    UINFO(3, "LPTF progress " << progress << "%" << endl);
                }
                lastProgress = progress;
            }
            bool merged = false;
            reinsertionList.clear();
            CoreVertex* const core1p = coresLeftp.back();
            coresLeftp.pop_back();
            UINFO(8, "inspecting  " << core1p->partsString() << " " << core1p->cost() << endl);
            uint32_t minMemory = core1p->memoryWords();
            do {
                HeapNode* placep = m_heap.max();
                CoreVertex* core2p = placep->key().corep;
                CostType mergedCost = costAfterMerge(core1p, core2p);
                if (isFeasible(mergedCost)) {
                    m_heap.remove(placep);
                    doMergeKeepHeap(core1p, core2p);
                    VL_DO_DANGLING(core2p->unlinkDelete(m_coreGraphp.get()), core2p);
                    m_heap.insert(core1p->heapNode().get());  // reinsert into the heap
                    merged = true;
                    numMerges++;
                } else {
                    // look deeper into the heap
                    UINFO(3, "Could not merge with the smallest processor" << endl);
                    reinsertionList.push_back(placep);
                    m_heap.remove(placep);
                    minMemory = std::max(minMemory, mergedCost.memWords);
                }
            } while (!merged && !m_heap.empty());

            if (!merged) {
                v3Global.rootp()->v3fatal("Could not reach the desired core count: ran out of "
                                          "memory while trying to merge "
                                          << core1p->partsString() << " which uses "
                                          << (core1p->memoryWords() * VL_EDATASIZE / VL_BYTESIZE)
                                          << " bytes and merge requires "
                                          << (minMemory * VL_EDATASIZE / VL_BYTESIZE) << " bytes"
                                          << endl);
                return 0;
            }
            for (HeapNode* nodep : reinsertionList) {
                m_heap.insert(nodep);
                statsReinsertions++;
            }
        }
        UINFO(3, "Finished LPTF merge" << endl);
        V3Stats::addStat("BspMerger, reinsertions ", statsReinsertions);
        return numMerges;
    }
    void buildMergedPartitions(std::vector<std::unique_ptr<DepGraph>>& oldPartitionsp) {

        int pix = 0;
        std::ofstream summary{v3Global.opt.makeDir() + "/" + "mergedCostEstimate.txt"};
        // clang-format off
        summary << "Vertex" << "            "
                << "Cost  " << "            "
                << "Memory" << "            "
                << "Fibers" << std::endl;
        // clang-format on
        iterVertex(m_coreGraphp.get(), [&](CoreVertex* corep) {
            // reconstruct the partitions
            // corep->partp()
            std::unique_ptr<std::ofstream> ofsp;
            uint32_t totalCost = 0;
            if (dump() >= 10) {
                ofsp = std::unique_ptr<std::ofstream>{V3File::new_ofstream(
                    v3Global.debugFilename("cost_post_merge" + cvtToStr(pix) + ".txt"))};
            }
            summary << pix << "            " << corep->instrCount() << "            "
                    << corep->memoryWords() * VL_EDATASIZE / VL_BYTESIZE << "            "
                    << corep->partp().size() << std::endl;

            UASSERT(corep->partp().size(), "invalid core partp size");
            if (corep->partp().size() == 1) {
                // this core was not merged
                m_partitionsp.emplace_back(std::move(oldPartitionsp[corep->partp().front()]));
                if (ofsp) {
                    for (V3GraphVertex* vtxp = m_partitionsp.back()->verticesBeginp(); vtxp;
                         vtxp = vtxp->verticesNextp()) {
                        if (CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp)) {
                            totalCost += V3InstrCount::count(compp->nodep(), false, ofsp.get());
                        }
                    }
                }
                pix++;
                return;
            }

            // a merged partition
            AstNode::user3ClearTree();
            // user3u has the new vertices
            m_partitionsp.emplace_back(std::make_unique<DepGraph>());
            const auto& newPartp = m_partitionsp.back();
            // iterate the vertices and clone them if not already cloned
            for (const int pix : corep->partp()) {
                const auto& oldPartp = oldPartitionsp[pix];
                iterVertex(oldPartp.get(), [&](AnyVertex* vtxp) {
                    auto cloneOnce = [&newPartp](auto const origp, auto newp) {
                        // clone only clone does not exist.
                        if (origp && !(*newp)) { *newp = origp->clone(newPartp.get()); }
                    };

                    if (ConstrVertex* const constrp = dynamic_cast<ConstrVertex*>(vtxp)) {
                        UASSERT(constrp->vscp(), "vscp should not be nullptr");
                        auto& linker = m_nodeVtx(constrp->vscp());
                        auto const commitp = dynamic_cast<ConstrCommitVertex*>(vtxp);
                        auto const defp = dynamic_cast<ConstrDefVertex*>(vtxp);
                        auto const postp = dynamic_cast<ConstrPostVertex*>(vtxp);
                        auto const initp = dynamic_cast<ConstrInitVertex*>(vtxp);
                        cloneOnce(commitp, &linker.commitp);
                        cloneOnce(defp, &linker.defp);
                        cloneOnce(postp, &linker.postp);
                        cloneOnce(initp, &linker.initp);
                    } else {
                        CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp);
                        UASSERT(compp, "ill-constructed partitionp" << pix << endl);
                        if (ofsp && !m_nodeVtx(compp->nodep()).compp) {
                            totalCost += V3InstrCount::count(compp->nodep(), false, ofsp.get());
                        }
                        cloneOnce(compp, &m_nodeVtx(compp->nodep()).compp);
                    }
                });
            }
            if (ofsp) {
                UASSERT(totalCost == corep->instrCount(),
                        "Invalid instruction count!" << totalCost << " != " << corep->instrCount()
                                                     << " in core " << pix << endl);
            }
            // now iterate old edges and clone them
            auto getNewVtxp = [this](AnyVertex* const oldp) -> AnyVertex* {
                AnyVertex* newp = nullptr;
                if (auto const compp = dynamic_cast<CompVertex*>(oldp)) {
                    newp = m_nodeVtx(compp->nodep()).compp;
                } else {
                    auto const constrp = dynamic_cast<ConstrVertex*>(oldp);
                    auto& linker = m_nodeVtx(constrp->vscp());
                    if (dynamic_cast<ConstrCommitVertex*>(oldp)) {
                        newp = linker.commitp;
                    } else if (dynamic_cast<ConstrDefVertex*>(oldp)) {
                        newp = linker.defp;
                    } else if (dynamic_cast<ConstrPostVertex*>(oldp)) {
                        newp = linker.postp;
                    } else if (dynamic_cast<ConstrInitVertex*>(oldp)) {
                        newp = linker.initp;
                    }
                }
                UASSERT(newp, "vertex not cloned");
                return newp;
            };
            for (const int pix : corep->partp()) {
                const auto& oldPartp = oldPartitionsp[pix];
                // TODO: do not create redundant edges
                iterVertex(oldPartp.get(), [&](AnyVertex* vtxp) {
                    for (V3GraphEdge* edgep = vtxp->outBeginp(); edgep;
                         edgep = edgep->outNextp()) {
                        // DepEdge* dedgep = dynamic_cast<DepEdge*>(edgep);
                        // UASSERT(dedgep, "invalid edge type");
                        ConstrVertex* const fromConstrp
                            = dynamic_cast<ConstrVertex*>(edgep->fromp());
                        CompVertex* const toCompp = dynamic_cast<CompVertex*>(edgep->top());
                        if (fromConstrp && toCompp) {
                            auto newFromp = dynamic_cast<ConstrVertex*>(getNewVtxp(fromConstrp));
                            auto newTop = dynamic_cast<CompVertex*>(getNewVtxp(toCompp));
                            newPartp->addEdge(newFromp, newTop);
                        } else {
                            ConstrVertex* const toConstrp
                                = dynamic_cast<ConstrVertex*>(edgep->top());
                            CompVertex* const frompCompp
                                = dynamic_cast<CompVertex*>(edgep->fromp());
                            UASSERT(toConstrp && frompCompp, "ill-constructed graph");
                            auto newFromp = dynamic_cast<CompVertex*>(getNewVtxp(frompCompp));
                            auto newTop = dynamic_cast<ConstrVertex*>(getNewVtxp(toConstrp));
                            newPartp->addEdge(newFromp, newTop);
                        }
                    }
                });
            }
            newPartp->removeRedundantEdges(V3GraphEdge::followAlwaysTrue);
            pix++;
        });

        oldPartitionsp.clear();
        oldPartitionsp = std::move(m_partitionsp);  // BOOM we are done
    }

public:
    explicit PartitionMerger(std::vector<std::unique_ptr<DepGraph>>& partitionsp,
                             uint32_t targetTileCount, uint32_t targetWorkerCount)
        : m_targetTileCount{targetTileCount}
        , m_targetWorkerCount{targetWorkerCount} {

        UINFO(10, "merging " << partitionsp.size() << " to " << targetCoreCount() << endl);
        if (partitionsp.empty() || partitionsp.size() <= targetCoreCount()) { return; }

        buildMultiCoreGraph(partitionsp);
        uint32_t numMergesConservative = 0;
        uint32_t numMergesForced = 0;
        if (v3Global.opt.ipuMergeStrategy().topDown()) {
            UINFO(3, "TopDown merge" << endl);
            numMergesForced = mergeLongestProcessingTimeFirst();
        } else if (v3Global.opt.ipuMergeStrategy().bottomUpTopDown()) {
            UINFO(3, "BottomUpTopDown merge " << endl);
            numMergesConservative = mergeConservatively();
            numMergesForced = mergeLongestProcessingTimeFirst();
        } else if (v3Global.opt.ipuMergeStrategy().bottomUp()) {
            UINFO(3, "BottomUp merge " << endl);
            numMergesConservative = mergeConservatively();
            numMergesForced = mergeForced();
        } else {
            v3Global.rootp()->v3fatal("Unimplemented merge strategy!");
        }
        V3Stats::addStat("BspMerger, initial partitions", partitionsp.size());
        if (numMergesConservative + numMergesForced > 0) {
            buildMergedPartitions(partitionsp);  // modifies partitionsp
        }
        V3Stats::addStat("BspMerger, merged partitions - conservative", numMergesConservative);
        V3Stats::addStat("BspMerger, merged partitions - forced", numMergesForced);
        V3Stats::addStat("BspMerger, final partitions", partitionsp.size());
        UINFO(3, "Finished merging" << endl);
    }
};

void V3BspMerger::merge(std::vector<std::unique_ptr<DepGraph>>& oldFibersp,
                        const std::vector<std::vector<std::size_t>>& indices) {
    struct NodeVtx {
        ConstrCommitVertex* commitp = nullptr;
        ConstrDefVertex* defp = nullptr;
        ConstrPostVertex* postp = nullptr;
        ConstrInitVertex* initp = nullptr;
        CompVertex* compp = nullptr;
    };

    std::ofstream summary{v3Global.opt.makeDir() + "/" + "mergedCostEstimate.txt"};
    // clang-format off
        summary << "Vertex" << "            "
                << "Cost  " << "            "
                << "Memory" << "            "
                << "Fibers" << std::endl;
    // clang-format on
    std::vector<std::unique_ptr<DepGraph>> newPartitionsp;
    for (int pix = 0; pix < indices.size(); pix++) {
        // reconstruct the partitions
        // corep->partp()
        const auto& includedParts = indices[pix];
        std::unique_ptr<std::ofstream> ofsp;
        uint32_t totalCost = 0;
        uint32_t memUsage = 0;

        // summary << pix << "            " << instrCount[pix] << "            "
        //         << (memoryUsage[pix] * VL_EDATASIZE / VL_BYTESIZE) << "            "
        //         << includedParts.size() << std::endl;

        UASSERT(includedParts.size() > 0, "invalid paritition size");
        // if (includedParts.size() == 1) {
        //     // this one has single fiber
        //     newPartitionsp.emplace_back(std::move(oldFibersp[includedParts.front()]));
        //     if (ofsp) {
        //         for (V3GraphVertex* vtxp = newPartitionsp.back()->verticesBeginp(); vtxp;
        //              vtxp = vtxp->verticesNextp()) {
        //             if (CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp)) {
        //                 totalCost += V3InstrCount::count(compp->nodep(), false, ofsp.get());
        //             }
        //         }
        //     }
        //     continue;
        // }

        // a merged partition

        std::unordered_map<AstNode*, NodeVtx> nodeLookup;
        // user3u has the new vertices
        newPartitionsp.emplace_back(std::make_unique<DepGraph>());
        const auto& newPartp = newPartitionsp.back();
        // iterate the vertices and clone them if not already cloned
        for (const int fiberId : includedParts) {
            const auto& oldPartp = oldFibersp[fiberId];
            for (V3GraphVertex* vtxp = oldPartp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                auto cloneOnce = [&newPartp](auto const origp, auto newp) {
                    // clone only clone does not exist, since fibers share compuate, the
                    // clone may already exist.
                    if (origp && !(*newp)) { *newp = origp->clone(newPartp.get()); }
                };

                if (ConstrVertex* const constrp = dynamic_cast<ConstrVertex*>(vtxp)) {
                    UASSERT(constrp->vscp(), "vscp should not be nullptr");
                    auto& linker = nodeLookup[constrp->vscp()];
                    auto const commitp = dynamic_cast<ConstrCommitVertex*>(vtxp);
                    auto const defp = dynamic_cast<ConstrDefVertex*>(vtxp);
                    auto const postp = dynamic_cast<ConstrPostVertex*>(vtxp);
                    auto const initp = dynamic_cast<ConstrInitVertex*>(vtxp);
                    cloneOnce(commitp, &linker.commitp);
                    cloneOnce(defp, &linker.defp);
                    cloneOnce(postp, &linker.postp);
                    cloneOnce(initp, &linker.initp);
                } else {
                    CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp);
                    UASSERT(compp, "ill-constructed fiber " << fiberId << endl);
                    if (ofsp && !nodeLookup[compp->nodep()].compp) {
                        totalCost += V3InstrCount::count(compp->nodep(), false, ofsp.get());
                    }
                    cloneOnce(compp, &nodeLookup[compp->nodep()].compp);
                }
            }
        }
        summary << pix << "            " << totalCost << "            "
                << (memUsage * VL_EDATASIZE / VL_BYTESIZE) << "            "
                << includedParts.size() << std::endl;
        // now iterate old edges and clone them
        auto getNewVtxp = [&nodeLookup](AnyVertex* const oldp) -> AnyVertex* {
            AnyVertex* newp = nullptr;
            if (auto const compp = dynamic_cast<CompVertex*>(oldp)) {
                newp = nodeLookup[compp->nodep()].compp;
            } else {
                auto const constrp = dynamic_cast<ConstrVertex*>(oldp);
                auto& linker = nodeLookup[constrp->vscp()];
                if (dynamic_cast<ConstrCommitVertex*>(oldp)) {
                    newp = linker.commitp;
                } else if (dynamic_cast<ConstrDefVertex*>(oldp)) {
                    newp = linker.defp;
                } else if (dynamic_cast<ConstrPostVertex*>(oldp)) {
                    newp = linker.postp;
                } else if (dynamic_cast<ConstrInitVertex*>(oldp)) {
                    newp = linker.initp;
                }
            }
            UASSERT(newp, "vertex not cloned");
            return newp;
        };
        for (int fiberId : includedParts) {
            const auto& oldPartp = oldFibersp[fiberId];
            // TODO: do not create redundant edges
            for (V3GraphVertex* vtxp = oldPartp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                for (V3GraphEdge* edgep = vtxp->outBeginp(); edgep; edgep = edgep->outNextp()) {
                    // DepEdge* dedgep = dynamic_cast<DepEdge*>(edgep);
                    // UASSERT(dedgep, "invalid edge type");
                    ConstrVertex* const fromConstrp = dynamic_cast<ConstrVertex*>(edgep->fromp());
                    CompVertex* const toCompp = dynamic_cast<CompVertex*>(edgep->top());
                    if (fromConstrp && toCompp) {
                        auto newFromp = dynamic_cast<ConstrVertex*>(getNewVtxp(fromConstrp));
                        auto newTop = dynamic_cast<CompVertex*>(getNewVtxp(toCompp));
                        newPartp->addEdge(newFromp, newTop);
                    } else {
                        ConstrVertex* const toConstrp = dynamic_cast<ConstrVertex*>(edgep->top());
                        CompVertex* const frompCompp = dynamic_cast<CompVertex*>(edgep->fromp());
                        UASSERT(toConstrp && frompCompp, "ill-constructed graph");
                        auto newFromp = dynamic_cast<CompVertex*>(getNewVtxp(frompCompp));
                        auto newTop = dynamic_cast<ConstrVertex*>(getNewVtxp(toConstrp));
                        newPartp->addEdge(newFromp, newTop);
                    }
                }
            }
        }
        newPartp->removeRedundantEdges(V3GraphEdge::followAlwaysTrue);
    }

    oldFibersp.clear();
    oldFibersp = std::move(newPartitionsp);  // BOOM we are done
}
void V3BspMerger::mergeAll(std::vector<std::unique_ptr<DepGraph>>& partitionsp,
                           uint32_t targetTileCount, uint32_t targetWorkerCount) {
    PartitionMerger{partitionsp, targetTileCount, targetWorkerCount};
}

};  // namespace V3BspSched