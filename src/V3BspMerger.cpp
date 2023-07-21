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
    explicit CostType(uint32_t f, uint32_t s)
        : instrCount{f}
        , recvCount{s} {}
    CostType() = default;
    inline uint32_t sum() const { return instrCount; }
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
        os << "Cost(" << c.sum() << ":" << c.instrCount << ", " << c.recvCount << ")";
        return os;
    }
    CostType percential(double p) {
        return CostType{static_cast<uint32_t>(instrCount * p),
                        static_cast<uint32_t>(recvCount * p)};
    }
    static CostType max() {
        return CostType{std::numeric_limits<uint32_t>::max(),
                        std::numeric_limits<uint32_t>::max()};
    }
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
    VlBitSet m_dupSet;
    std::vector<int> m_partIndex;
    std::unique_ptr<HeapNode> m_heapNode;
    bool m_hasPli;

public:
    CoreVertex(MultiCoreGraph* graphp, size_t numDups, std::vector<int>&& parts)
        : V3GraphVertex{graphp}
        , m_dupSet{numDups}
        , m_partIndex{parts}
        , m_heapNode(std::make_unique<HeapNode>()) {
        m_heapNode->m_key = HeapKey{.corep = this};
    }

    inline uint32_t instrCount() const { return m_instrCount; }
    inline uint32_t recvWords() const { return m_recvWords; }
    inline void instrCount(uint32_t v) { m_instrCount = v; }
    inline void recvWords(uint32_t v) { m_recvWords = v; }
    inline std::vector<int>& partp() { return m_partIndex; }
    inline VlBitSet& dupSet() { return m_dupSet; }
    inline CostType cost() const { return CostType{instrCount(), recvWords()}; }
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
inline uint32_t targetCoreCount() { return v3Global.opt.tiles() * v3Global.opt.workers(); }

class InstrPliChecker final : public VNVisitor {
private:
    bool m_hasPli = false;
    inline void setPli() { m_hasPli = true; }
    void visit(AstCCall* callp) {
        if (callp->funcp()->dpiImportWrapper()) { setPli(); }
        iterateChildren(callp);
    }
    void visit(AstDisplay* nodep) override { setPli(); }
    void visit(AstFinish* nodep) override { setPli(); }
    void visit(AstStop* nodep) override { setPli(); }
    void visit(AstNodeReadWriteMem* nodep) override { setPli(); }
    void visit(AstNode* nodep) { iterateChildren(nodep); }
    explicit InstrPliChecker(AstNode* nodep) {
        m_hasPli = false;
        iterate(nodep);
    }

public:
    static bool hasPli(AstNode* nodep) {
        const InstrPliChecker visitor{nodep};
        return visitor.m_hasPli;
    }
};
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
    std::vector<uint32_t> m_instrCount;
    std::unique_ptr<MultiCoreGraph> m_coreGraphp;
    MinHeap m_heap;

    std::vector<std::unique_ptr<DepGraph>> m_partitionsp;

    template <typename Fn>
    void iterVertex(V3Graph* const graphp, Fn&& fn) {
        using Traits = FunctionTraits<Fn>;
        using Arg = typename Traits::template arg<0>::type;
        for (V3GraphVertex *vtxp = graphp->verticesBeginp(), *nextp; vtxp; vtxp = nextp) {
            nextp = vtxp->verticesNextp();
            if (Arg const vp = dynamic_cast<Arg>(vtxp)) { fn(vp); }
        }
    }
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
        std::vector<uint32_t> totalCost;
        std::vector<bool> hasPli;
        totalCost.resize(partitionsp.size());
        hasPli.resize(partitionsp.size());
        std::fill_n(hasPli.begin(), hasPli.size(), false);

        for (int pix = 0; pix < partitionsp.size(); pix++) {
            const auto& graphp = partitionsp[pix];
            uint32_t costAccum = 0;
            iterVertex(graphp.get(), [&](AnyVertex* const vtxp) {
                if (ConstrCommitVertex* const commitp = dynamic_cast<ConstrCommitVertex*>(vtxp)) {
                    UASSERT(commitp->vscp(), "ConstrCommitVertex of nullptr");
                    UASSERT_OBJ(!commitp->vscp()->user1p(), commitp->vscp(),
                                "produced by multiple partitions "
                                    << commitp->vscp()->prettyNameQ() << endl);
                    // mark AstVarScope with the partition that produces it
                    commitp->vscp()->user1(pix + 1);
                }
                // compute and cache the cost of each node
                if (CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp)) {
                    auto& infoRef = m_nodeInfo(compp->nodep());
                    const uint32_t numInstr = V3InstrCount::count(compp->nodep(), false);
                    if (InstrPliChecker::hasPli(compp->nodep())) { hasPli[pix] = true; }
                    costAccum += numInstr;
                    if (!infoRef.visited) {
                        infoRef.visited = true;
                        infoRef.nodeIndex = nodeIndex;
                        m_instrCount.push_back(numInstr);
                        nodeIndex += 1;
                    } else if (infoRef.visited && !infoRef.hasDuplicates) {
                        // the second visit, mark as duplicate
                        infoRef.hasDuplicates = true;
                        m_dupInstrCount.push_back(cachedInstrCount(compp->nodep()));
                        infoRef.nodeDupIndex = dupIndex;
                        dupIndex += 1;
                    } else {
                        // don't care, third or later visits
                    }
                }
            });
            totalCost[pix] = costAccum;
            // iterVertex<DepGraph, AnyVertex>(graphp.get(), )
        }
        m_coreGraphp = std::make_unique<MultiCoreGraph>();
        std::vector<CoreVertex*> coresp;

        // number of nodes that have duplicates
        const size_t numDups = m_dupInstrCount.size();
        UINFO(3, "There are " << numDups << " nodes that have duplicates" << endl);
        V3Stats::addStat("BspMerger, nodes with duplicates ", numDups);
        for (int pix = 0; pix < partitionsp.size(); pix++) {
            // now create a CoreVertex for each partition
            const auto& depGraphp = partitionsp[pix];
            CoreVertex* corep = new CoreVertex{m_coreGraphp.get(), numDups, {pix}};
            coresp.push_back(corep);
            corep->instrCount(totalCost[pix]);
            corep->hasPli(hasPli[pix]);
            // Fill-in the duplicate set within the core
            iterVertex(depGraphp.get(), [&](CompVertex* const compp) {
                AstNode* const nodep = compp->nodep();
                auto& info = m_nodeInfo(nodep);
                auto& dups = corep->dupSet();
                if (info.hasDuplicates) { dups.insert(info.nodeDupIndex); }
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

        // // insert all the cores in a min heap
        // for (CoreVertex* corep : coresp) {
        //     if (!corep->hasPli()) {  // don't merge the cores that do PLI, we should
        //         // consider them separately
        //         m_heap.insert(corep->heapNode().get());
        //     }
        // }

        if (dumpGraph() >= 5) { m_coreGraphp->dumpDotFilePrefixed("multicore"); }
    }
    // compute the cost of merging
    CostType costAfterMerge(CoreVertex* core1p, CoreVertex* core2p) const {

        const uint32_t rawInstrCost = core1p->instrCount() + core2p->instrCount();
        const uint32_t rawRecvCost = core1p->recvWords() + core2p->recvWords();

        auto sumRecvFrom = [](CoreVertex* recvp, CoreVertex* sendp) {
            uint32_t sum = 0;
            for (V3GraphEdge* edgep = recvp->inBeginp(); edgep; edgep = edgep->inNextp()) {
                if (edgep->fromp() == sendp) { sum += static_cast<uint32_t>(edgep->weight()); }
            }
            return sum;
        };
        const uint32_t recvReduction = sumRecvFrom(core1p, core2p) + sumRecvFrom(core2p, core1p);

        VlBitSet dupInCommon = VlBitSet::doIntersect(core1p->dupSet(), core2p->dupSet());
        uint32_t dupCostCommon = 0;
        dupInCommon.foreach([&](size_t dupIx) { dupCostCommon += m_dupInstrCount[dupIx]; });
        UASSERT(rawInstrCost >= dupCostCommon, "invalid instr cost computation");
        UASSERT(rawRecvCost >= recvReduction, "invalid recv cost computation");
        const uint32_t mergedCost = rawInstrCost - dupCostCommon;
        const uint32_t mergedRecvCost = rawRecvCost - recvReduction;
        return CostType{mergedCost, mergedRecvCost};
    }
    // merger core1p and core2p
    // complexity should be amortized O(max(log V, E))
    void doMerge(CoreVertex* core1p, CoreVertex* core2p, CostType newCost = CostType{0, 0}) {
        if (newCost == CostType{0, 0}) { newCost = costAfterMerge(core1p, core2p); }

        UINFO(10, "merging " << core1p->partsString() << " and " << core2p->partsString() << endl);

        // remove both cores from the min-heap
        // O(log V) * 2
        m_heap.remove(core1p->heapNode().get());
        m_heap.remove(core2p->heapNode().get());

        // create push core2p into core1p
        std::vector<int> parts{core1p->partp()};

        for (const auto p : core2p->partp()) { core1p->partp().push_back(p); }

        core1p->dupSet().unionInPlace(core2p->dupSet());
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
    uint32_t mergeConservatively() {
        UASSERT(m_heap.empty(), "heap should be empty");
        uint32_t numMerges = 0;
        std::vector<CostType> coreCost = gatherCost();
        uint32_t numCores = coreCost.size();
        if (numCores <= targetCoreCount()) { return 0; }

        iterVertex(m_coreGraphp.get(), [&](CoreVertex* corep) {
            if (!corep->hasPli()) { m_heap.insert(corep->heapNode().get()); }
        });
        std::stable_sort(coreCost.begin(), coreCost.end());
        // do not exceed the 95th percentile worst-case cost
        CostType worstCost = coreCost.back();
        for (const double p : {0.95, 0.96, 0.97, 0.98, 0.99}) {
            if (worstCost.percential(p) > CostType{0, 0}) {
                worstCost = worstCost.percential(p);
                break;
            }
        }

        if (worstCost == CostType{0, 0}) { worstCost = coreCost.back(); }
        // worstCost.instrCount += 200;
        // worstCost.recvCount += 200;
        UINFO(8, "Max permissible cost is " << worstCost << " and the max absolute cost is "
                                            << coreCost.back() << endl);
        // iteratively merge small cores up to the worstCost
        // m_heap.max() is actually min
        HeapNode* minNodep = m_heap.max();

        // Conservatively merge: avoid an increase to the critical path
        while (numCores > targetCoreCount() && !m_heap.empty() && minNodep
               && minNodep->key().corep->cost() <= worstCost) {
            // try merging minNodep with a neighbor
            CoreVertex* bestNeighbor = nullptr;
            CostType bestCost = CostType::max();
            CoreVertex* corep = minNodep->key().corep;
            auto visitNeighbor = [&](GraphWay way) {
                iterEdges(corep, way, [&](V3GraphEdge* edgep) {
                    CoreVertex* const neighbor = dynamic_cast<CoreVertex*>(edgep->furtherp(way));
                    CostType newCost = costAfterMerge(corep, neighbor);
                    if (newCost <= worstCost && newCost <= bestCost) {
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
            if (bestNeighbor && bestCost <= costWithNext) {
                // found a neighbor, merge it
                UINFO(8, "Merging with neighbors: " << bestCost << endl);
                doMerge(corep, bestNeighbor, bestCost);
                UASSERT(numCores > 1, "numCores underflowed");
                numCores--;
                numMerges++;
            } else if (secondMinNodep && costWithNext <= worstCost) {
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
                    if (newCost < bestCost) {
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
            if (bestNeighborp && bestCost <= costWithNext) {
                UINFO(8, "Merging with neighbor core givs "
                             << bestCost << " current = " << currentWorst << endl);
                applyMerge(bestNeighborp, bestCost);
            } else if (secondMinNodep) {
                // there were no neighbors, merge with the next in the heap
                CoreVertex* otherCorep = secondMinNodep->key().corep;
                UINFO(8, "Merging with next in line " << costWithNext
                                                      << " current = " << currentWorst << endl);
                applyMerge(otherCorep, costWithNext);
            } else {
                // something is up
                UASSERT(false, "Nothing left to merge!");
            }
        }

        UASSERT(numCores <= targetCoreCount(), "Could not reach desired count "
                                                   << targetCoreCount() << " > " << numCores
                                                   << endl);
        // clean up
        while (!m_heap.empty()) { m_heap.remove(m_heap.max()); }

        if (dumpGraph() >= 5) { m_coreGraphp->dumpDotFilePrefixed("multicore_forced_final"); }
        return numMerges;
    }

    void buildMergedPartitions(std::vector<std::unique_ptr<DepGraph>>& oldPartitionsp) {

        iterVertex(m_coreGraphp.get(), [&](CoreVertex* corep) {
            // reconstruct the partitions
            // corep->partp()
            UASSERT(corep->partp().size(), "invalid core partp size");
            if (corep->partp().size() == 1) {
                // this core was not merged
                m_partitionsp.emplace_back(std::move(oldPartitionsp[corep->partp().front()]));
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
                        cloneOnce(compp, &m_nodeVtx(compp->nodep()).compp);
                    }
                });
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
        });

        oldPartitionsp.clear();
        oldPartitionsp = std::move(m_partitionsp);  // BOOM we are done
    }

public:
    explicit PartitionMerger(std::vector<std::unique_ptr<DepGraph>>& partitionsp) {

        if (partitionsp.empty() || partitionsp.size() <= targetCoreCount()) { return; }

        buildMultiCoreGraph(partitionsp);
        uint32_t numMerges = mergeConservatively();
        uint32_t numMergesForced = mergeForced();
        if (numMerges) {
            V3Stats::addStat("BspMerger, initial partitions", partitionsp.size());
            buildMergedPartitions(partitionsp);  // modifies partitionsp
            V3Stats::addStat("BspMerger, merged partitions - conservative", numMerges);
            V3Stats::addStat("BspMerger, merged partitions - forced", numMergesForced);
            V3Stats::addStat("BspMerger, final partitions", partitionsp.size());
        }
    }
};

void V3BspMerger::merge(std::vector<std::unique_ptr<DepGraph>>& partitionsp) {
    PartitionMerger{partitionsp};
}

};  // namespace V3BspSched