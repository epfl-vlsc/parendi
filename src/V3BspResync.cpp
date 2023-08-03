// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: BSP resynchronization optimization
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

#include "V3BspResync.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3BspGraph.h"
#include "V3BspPliCheck.h"
#include "V3BspResync.h"
#include "V3BspResyncGraph.h"
#include "V3BspSched.h"
#include "V3Const.h"
#include "V3Dead.h"
#include "V3InstrCount.h"
#include "V3PairingHeap.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3BspSched {
namespace Resync {

// Turn the dependece graphs into ResyncGraphs later used to perform resynchronization
class ResyncGraphBuilder final {
public:
    static std::vector<std::unique_ptr<ResyncGraph>>
    build(const std::vector<std::unique_ptr<DepGraph>>& depGraphp) {

        // list of all the graphs being built
        std::vector<std::unique_ptr<ResyncGraph>> graphsp;

        // map VarScope to the corresponding SeqVertex* (its writer)
        std::unordered_map<AstVarScope*, SeqVertex*> writersp;

        // list of all sink nodes (i.e., SeqVertex*)
        std::vector<SeqVertex*> sinksp;

        // Build a resync graph for each dep graph. Add a single sink SeqVertex* to it
        // as well
        for (int pix = 0; pix < depGraphp.size(); pix++) {
            auto& depp = depGraphp[pix];
            // toplogically sort dependence graph since we are clumping sequential vertices
            depp->rank();
            depp->sortVertices();

            std::unique_ptr<ResyncGraph> graphp{new ResyncGraph{pix}};
            std::vector<AstVarScope*> lvsp;
            std::vector<LogicWithActive>
                logicsp;  // should be ordered, guaranteed by sorting the depp graph
            AstSenTree* senp = nullptr;
            uint32_t seqCost = 0;
            bool unopt = false;
            for (V3GraphVertex* vtxp = depp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                if (ConstrCommitVertex* const commitp = dynamic_cast<ConstrCommitVertex*>(vtxp)) {
                    lvsp.push_back(commitp->vscp());
                }
                CompVertex* const compVtxp = dynamic_cast<CompVertex*>(vtxp);
                if (compVtxp && compVtxp->domainp()) {
                    // seq logic
                    logicsp.emplace_back(compVtxp->nodep(), compVtxp->activep());
                    // UASSERT_OBJ(!senp || senp == compVtxp->domainp(), compVtxp->nodep(),
                    //             "multiple domains?");
                    if (senp && senp != compVtxp->domainp()) {
                        // multiple domains, cannot resync
                        UINFO(3, "Will not resynchronize graph " << pix << " with multiple domains"
                                                                 << endl);
                        unopt = true;
                    }
                    senp = compVtxp->domainp();
                    seqCost += V3InstrCount::count(compVtxp->nodep(), false);
                    if (PliCheck::check(compVtxp->nodep())) {
                        UINFO(3,
                              "Will not resynchronize graph " << pix << " with PLI/DPI " << endl);
                        // PLI/DPI cannot be resynchronized
                        unopt = true;
                    }
                }
            }
            UASSERT(!logicsp.empty(), "empty seq?");
            if (unopt) senp = nullptr;  // zap it
            SeqVertex* const seqp
                = new SeqVertex{graphp.get(), seqCost, senp, std::move(logicsp), lvsp};
            seqp->unopt(unopt);
            for (AstVarScope* vscp : lvsp) { writersp.emplace(vscp, seqp); }
            graphsp.emplace_back(std::move(graphp));
            sinksp.push_back(seqp);
        }

        // do we have def -> AssignPre?
        auto fromAssignPre = [](ConstrDefVertex* defp) {
            if (defp->outSize1()) {
                CompVertex* const succp = dynamic_cast<CompVertex*>(defp->outBeginp()->top());
                if (VN_IS(succp->nodep(), AssignPre)) {
                    // not real combinational data dependence
                    return true;
                }
            }
            return false;
        };
        // do we have AssignPre -> def?
        auto toAssignPre = [](ConstrDefVertex* defp) {
            if (defp->inSize1()) {
                CompVertex* const prevp = dynamic_cast<CompVertex*>(defp->inBeginp()->fromp());
                if (VN_IS(prevp->nodep(), AssignPre)) {
                    // not real combinational data dependence
                    return true;
                }
            }
            return false;
        };

        // cache of all new CombVertex vertices that are being built
        std::unordered_map<AnyVertex*, CombVertex*> newCombsp;
        auto getCombVertex = [&](ResyncGraph* graphp, CompVertex* oldp) {
            if (!newCombsp.count(oldp)) {
                CombVertex* const newp
                    = new CombVertex{graphp, LogicWithActive{oldp->nodep(), oldp->activep()},
                                     V3InstrCount::count(oldp->nodep(), false)};
                newCombsp.emplace(oldp, newp);
            }
            return newCombsp[oldp];
        };

        auto getSeqp = [&](AstVarScope* vscp) -> SeqVertex* {
            auto it = writersp.find(vscp);
            if (it == writersp.end()) {
                return nullptr;
            } else {
                return it->second;
            }
        };
        // complete the ResyncGraph by adding combinational logic and edges to it
        for (int pix = 0; pix < depGraphp.size(); pix++) {
            const auto& depp = depGraphp[pix];
            const auto& graphp = graphsp[pix];
            SeqVertex* const sinkp = sinksp[pix];

            for (V3GraphVertex* vtxp = depp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                ConstrDefVertex* const defp = dynamic_cast<ConstrDefVertex*>(vtxp);
                if (!defp) continue;
                if (defp->outEmpty()) {
                    // is it dead?
                    continue;
                }
                if (toAssignPre(defp) || fromAssignPre(defp)) {
                    // Do not consider them "combinational data dependence"
                    continue;
                }
                ResyncVertex* newp = nullptr;
                SeqVertex* const seqp = getSeqp(defp->vscp());
                if (seqp) {
                    // variable is written by sequential logic, the only acceptable
                    // predecessor is an AssignPre (ruled out above)
                    UASSERT_OBJ(defp->inEmpty(), defp->vscp(), "did not expect predecessor");
                    SeqReadVertex* const readp
                        = new SeqReadVertex{graphp.get(), defp->vscp(), seqp};
                    // link this graph to the producer
                    seqp->consumersp().insert({graphp.get(), readp});
                    newp = readp;
                } else if (!defp->inEmpty()) {
                    UASSERT_OBJ(defp->inSize1(), defp->vscp(), "expected single pred");
                    // can only be from comp logic, or AssignPre but that is rule out above.
                    CompVertex* const predp = dynamic_cast<CompVertex*>(defp->inBeginp()->fromp());
                    UASSERT_OBJ(!predp->domainp(), predp->nodep(), "did not expect clocked logic");
                    newp = getCombVertex(graphp.get(), predp);
                }
                if (!newp) {
                    UASSERT_OBJ(defp->inEmpty(), defp->vscp(), "expected no pred");
                    // def of var set by initial blocks, effectively constant
                    continue;
                }
                // connect predp -> defp -> succp as newp -> succp_as_newp
                for (V3GraphEdge* edgep = defp->outBeginp(); edgep; edgep = edgep->outNextp()) {
                    CompVertex* const succp = dynamic_cast<CompVertex*>(edgep->top());
                    UASSERT(succp, "ill-constructed graph");
                    if (succp->domainp()) {
                        // feeds into seq logic
                        graphp->addEdge(newp, sinkp, defp->vscp());
                    } else {
                        // feeds into comb
                        graphp->addEdge(newp, getCombVertex(graphp.get(), succp), defp->vscp());
                    }
                }
            }
        }
        // done, the graph may have redundant edges, between two vertex, but they have
        // different vscp() pointers, so do not remove them
        return graphsp;
    }
};

// Transform ResyncGraphs and resynchronize them
class ResyncGraphTransformer final {
private:
    Utils::MaxHeap m_heap;
    std::vector<std::unique_ptr<ResyncGraph>>& m_allGraphsp;
    bool m_changed = false;
    VDouble0 m_statsNumTransformed;
    VDouble0 m_statsTransitivelyDisabled;
    VDouble0 m_statsUnoptDisabled;
    VDouble0 m_statsUnableDisabled;
    VDouble0 m_statsCostAfter;
    VDouble0 m_statsCostBefore;

    inline void insertToHeap(ResyncGraph* graphp) {
        uint32_t c = 0;
        for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            c += dynamic_cast<ResyncVertex*>(vtxp)->cost();
        }
        graphp->cost(c);
        UASSERT(!graphp->heapNodep(), "garbage node?");
        graphp->heapNodep() = std::make_unique<Utils::HeapNode>();
        m_heap.insert(graphp->heapNodep().get(), Utils::Key{graphp});
    }
    inline void removeFromHeap(ResyncGraph* graphp) {
        UASSERT(graphp->heapNodep(), "expected non-nullptr");
        m_heap.remove(graphp->heapNodep().get());
        graphp->heapNodep() = nullptr;
    }

    struct VertexByRank final : public std::vector<std::vector<ResyncVertex*>> {
        inline const std::vector<ResyncVertex*>& atRank(int rank) const {
            UASSERT(rank > 0 && rank <= size(), "out of range");
            return at(rank - 1);
        }

        static VertexByRank build(ResyncGraph* graphp) {
            int currentRank = 0;
            VertexByRank res;
            for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                UASSERT(vtxp->rank() > 0, "not ranked");
                ResyncVertex* ptr = dynamic_cast<ResyncVertex*>(vtxp);
                UASSERT(ptr, "invalid type");
                if (ptr->rank() > currentRank) {
                    res.push_back(std::vector<ResyncVertex*>{ptr});
                } else {
                    res.back().push_back(ptr);
                }
                UASSERT(currentRank <= ptr->rank(), "not sorted");
                currentRank = ptr->rank();
            }
            return res;
        }
    };

    struct CostComputer {
        std::unordered_map<ResyncVertex*, uint32_t> m_costCache;
        std::unordered_set<ResyncVertex*> m_pathExistsFromSeqp;
        SeqVertex* const m_seqp;
        const VertexByRank& m_byRank;
        explicit CostComputer(SeqVertex* seqp, const VertexByRank& ranks)
            : m_seqp{seqp}
            , m_byRank{ranks} {
            m_costCache.clear();
            m_pathExistsFromSeqp.clear();
        }
        VL_UNCOPYABLE(CostComputer);
        /// Get the cummulative cost of anything including and above sinkp
        uint32_t vertexCostAbove(ResyncVertex* sinkp, uint32_t costHigherRanks) {
            // O(EV) can we do better?
            const auto it = m_costCache.find(sinkp);
            if (it == m_costCache.end()) {
                uint32_t sum = 0;
                std::vector<ResyncVertex*> toVisit{sinkp};
                std::unordered_set<ResyncVertex*> dones{sinkp};
                while (!toVisit.empty()) {
                    ResyncVertex* const backp = toVisit.back();
                    sum += backp->cost();
                    toVisit.pop_back();
                    for (V3GraphEdge* inp = backp->inBeginp(); inp; inp = inp->inNextp()) {
                        ResyncVertex* const fromp = dynamic_cast<ResyncVertex*>(inp->fromp());
                        UASSERT(fromp, "bad type");
                        if (!dones.count(fromp)) {
                            dones.insert(fromp);
                            toVisit.push_back(fromp);
                            SeqReadVertex* seqReadp = dynamic_cast<SeqReadVertex*>(fromp);
                            if (seqReadp && seqReadp->writerp() == m_seqp) {
                                m_pathExistsFromSeqp.insert(sinkp);
                            }
                        }
                    }
                }
                m_costCache.emplace(sinkp, sum);
            }
            UASSERT(m_costCache.count(sinkp), "Cost not computed");
            const uint32_t sinkCost = m_costCache[sinkp];
            const uint32_t additionFromBelow
                = (m_pathExistsFromSeqp.count(sinkp) ? costHigherRanks : 0);
            return sinkCost + additionFromBelow;
        }

        uint32_t maxCostAbove(int cutRank, uint32_t costHigherRanks) {
            uint32_t cs = 0;
            for (int r = cutRank; r >= 1; r--) {
                for (ResyncVertex* const vtxp : m_byRank.atRank(r)) {
                    if (r == cutRank) {
                        cs = std::max(cs, vertexCostAbove(vtxp, costHigherRanks));
                    } else {
                        // r < cutRank
                        bool crossing = true;
                        // Any vertex v with v.rank < cutRank may also require sampling.
                        // So need to compute their cost as well. These are te vertices that have
                        // and out edge (v, u) such that u.rank > cutRank
                        for (V3GraphEdge* outp = vtxp->outBeginp(); outp;
                             outp = outp->outNextp()) {
                            if (outp->top()->rank() <= cutRank) {
                                crossing = false;
                                // vtxp ==> outp->top() needs to be sampled as well
                                break;
                            }
                        }
                        if (crossing) {
                            cs = std::max(cs, vertexCostAbove(vtxp, costHigherRanks));
                        }
                    }
                }
            }
            return cs;
        }
        uint32_t maxCostBelow(uint32_t costHigherRanks) {
            uint32_t cBelow = 0;
            for (const auto& it : m_seqp->consumersp()) {
                if (it.second->writerp() == m_seqp) {
                    // already accounted for in the maxCostAbove computation
                } else {
                    // rsync to other graph, simply add the cost
                    cBelow = std::max(cBelow, it.first->cost() + costHigherRanks);
                }
            }
            return cBelow;
        }
    };
    void tryResync(ResyncGraph* graphp) {
        // rank and sort the graph by rank
        graphp->rank();
        graphp->sortVertices();

        if (dump() > 10) {
            graphp->dumpDotFilePrefixed("resync_graph_" + cvtToStr(graphp->index()));
        }
        VertexByRank verticesp = VertexByRank::build(graphp);

        SeqVertex* seqp = dynamic_cast<SeqVertex*>(verticesp.back().front());
        if (seqp->unopt()) {
            UINFO(5, "Unoptimizable partition " << graphp->index() << " with cost "
                                                << graphp->cost() << endl);
            m_statsUnoptDisabled++;
            removeFromHeap(graphp);
            return;
        }
        UASSERT(seqp && verticesp.back().size() == 1, "expected valid single sink");
        // The above assertion does not hold on a graph that has been resynchronized
        // since a resynched graph may have many sinks, all of type CombSeqVertex

        const uint32_t maxCost = graphp->cost();

        const int graphRank = seqp->rank();
        UASSERT(!graphp->resyncRank(), "already resynced!");

        int bestRank = 0;
        int bestCost = maxCost;
        // from the bottom of the graph, crawl up rank-by-rank and find the best rank
        // to perform retiming. If cost starts increasing abort.
        UINFO(8, "Analyzing graph " << graphp->index() << " with cost " << graphp->cost()
                                    << " and rank " << graphRank << endl);
        int costHigherRanks = seqp->cost();
        CostComputer costModel{seqp, verticesp};

        for (int r = graphRank - 1; r > 1; r--) {
            // Consider r as the resync point:
            // 1. compute the cost of any v s.t. v.rank <= r and there exists
            //    an edge (v, u) s.t. u.rank > r
            //    These vertices will need to be sampled
            const uint32_t cAbove = costModel.maxCostAbove(r, costHigherRanks);
            // 2. compute the cost of turn any v s.t. v.rank > r into comb logic,
            //    essentially adding to the execution time of other graphs that consume
            //    the values produced by seqp
            const uint32_t cBelow = costModel.maxCostBelow(costHigherRanks);
            UINFO(10,
                  "    at rank " << r << " cAbove = " << cAbove << " cBelow = " << cBelow << endl);
            if (cBelow < bestCost && cAbove < bestCost) {
                // Resync has benefits
                bestRank = r;
                bestCost = std::max(cAbove, cBelow);
            }

            for (ResyncVertex* const vtxp : verticesp.atRank(r)) {
                costHigherRanks += vtxp->cost();
            }
        }

        if (bestRank) {
            // great, found something
            UINFO(4, "Resync graph " << graphp->index() << " at rank " << bestRank << " gives "
                                     << bestCost << " < " << maxCost << endl);
            // move the vertices around and remove this graph any other consumer graph touched by
            // it from the heap
            m_statsNumTransformed++;
            if (m_statsCostAfter < bestCost) { m_statsCostAfter = bestCost; }
            transformGraph(graphp, bestRank, seqp);
        } else {
            UINFO(5,
                  "Will not resync graph " << graphp->index() << " with cost " << maxCost << endl);
            m_statsUnableDisabled++;
            removeFromHeap(graphp);
        }
    }

    void transformGraph(ResyncGraph* graphp, int cutRank, SeqVertex* const seqp) {
        UINFO(5, "Transforming graph " << graphp->index() << " at rank " << cutRank << endl);
        UASSERT(cutRank > 1, "invalid cut rank");
        m_changed = true;
        std::vector<ResyncEdge*> samplersp;
        std::vector<ResyncVertex*> sourcesp;
        // collect the vertices that need to be sampled
        struct EdgeSubst {
            CombSeqVertex* combSeqp = nullptr;
            CombSeqReadVertex* combReadp = nullptr;
        };
        std::unordered_map<ResyncEdge*, EdgeSubst> m_edgeSubst;
        AstSenTree* sampleSenTreep = seqp->sentreep();
        graphp->foreachVertex([&](ResyncVertex* vtxp) {
            UINFO(70, "Visiting vertex " << vtxp << " " << cvtToHex(vtxp) << endl);
            vtxp->foreachOutEdge([&](ResyncEdge* edgep) {
                if (vtxp->rank() && vtxp->rank() <= cutRank && edgep->top()->rank() > cutRank) {
                    // need to sample
                    UINFO(70, "Morphing edge " << edgep->vscp()->prettyNameQ() << " "
                                               << cvtToHex(edgep) << endl);
                    EdgeSubst substp;
                    auto it = m_edgeSubst.find(edgep);
                    if (it == m_edgeSubst.end()) {
                        substp.combSeqp = new CombSeqVertex{graphp, edgep->vscp(), sampleSenTreep};
                        substp.combReadp
                            = new CombSeqReadVertex{graphp, edgep->vscp(), sampleSenTreep};
                        substp.combSeqp->rank(0);  // new & unranked
                        substp.combReadp->rank(0);  // new & unranked
                        m_edgeSubst.insert({edgep, substp});
                        sourcesp.push_back(substp.combReadp);
                        UINFO(70, "CombSeq = " << cvtToHex(substp.combSeqp) << " CombSeqRead = "
                                               << cvtToHex(substp.combReadp) << endl);
                    } else {
                        substp = it->second;
                    }
                    ResyncVertex* const fromp = dynamic_cast<ResyncVertex*>(edgep->fromp());
                    UASSERT(vtxp == edgep->fromp(), "invalid iteration");
                    ResyncVertex* const top = dynamic_cast<ResyncVertex*>(edgep->top());
                    UASSERT(fromp && top, "bad types");
                    ResyncEdge* e1p = graphp->addEdge(fromp, substp.combSeqp, edgep->vscp());
                    ResyncEdge* e2p = graphp->addEdge(substp.combReadp, top, edgep->vscp());
                    UINFO(10, "New edges " << cvtToHex(e1p) << " and " << cvtToHex(e2p) << endl);
                    VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
                    UINFO(10, "top = " << cvtToHex(top) << " fromp = " << cvtToHex(fromp) << endl);
                    UASSERT(dynamic_cast<ResyncVertex*>(top) && dynamic_cast<ResyncVertex*>(fromp),
                            "inconsistent graph");
                }
            });
        });
        m_edgeSubst.clear();

        if (dump() >= 70) { graphp->dumpDotFilePrefixed("cut_" + cvtToStr(graphp->index())); }
        std::set<ResyncGraph*> consumerGraphs;
        for (auto pair : seqp->consumersp()) { consumerGraphs.insert(pair.first); }

        auto cloneTransform = [&seqp](ResyncGraph* otherp, ResyncVertex* origp) -> ResyncVertex* {
            if (origp == seqp) {
                return new SeqCombVertex{otherp, seqp->cost(), seqp->sentreep(), seqp->logicsp(),
                                         seqp->lvsp()};
            } else if (auto combp = dynamic_cast<CombVertex*>(origp)) {
                return new CombCombVertex{otherp, combp->logicp(), seqp->sentreep(),
                                          combp->cost()};
            } else if (auto readp = dynamic_cast<CombSeqReadVertex*>(origp)) {
                return readp->clone(otherp);
            } else {
                UASSERT(false, "did not expect type");
                return nullptr;
            }
        };

        for (const auto& otherp : consumerGraphs) {
            UINFO(8,
                  "- Pushing logic from " << graphp->index() << " to " << otherp->index() << endl);
            std::unordered_map<ResyncVertex*, ResyncVertex*> clonesp;
            for (ResyncVertex* readp : sourcesp) {
                clonesp.emplace(readp, cloneTransform(otherp, readp));
            }
            std::vector<ResyncVertex*> toVisit{sourcesp.begin(), sourcesp.end()};
            while (!toVisit.empty()) {
                ResyncVertex* const backp = toVisit.back();
                UINFO(70, "    - Visiting " << backp << " " << cvtToHex(backp) << endl);
                toVisit.pop_back();
                backp->foreachOutEdge([&](ResyncEdge* edgep) {
                    ResyncVertex* const top = dynamic_cast<ResyncVertex*>(edgep->top());
                    UASSERT(top, "bad vertex type");
                    if (!clonesp.count(top)) {
                        clonesp.emplace(top, cloneTransform(otherp, top));
                        toVisit.push_back(top);
                    }
                    UASSERT(clonesp.count(backp), "expected backp to have been cloned");
                    ResyncVertex* const fromNewp = clonesp[backp];
                    ResyncVertex* const toNewp = clonesp[top];
                    otherp->addEdge(fromNewp, toNewp, edgep->vscp());
                });
            }

            // connect edges from the SeqCombVertex to any consumer
            const auto consumeRange = seqp->consumersp().equal_range(otherp);
            ResyncVertex* const seqCombp = clonesp[seqp];
            UASSERT(seqCombp, "expected valid clone");
            std::vector<SeqReadVertex*> deletesp;
            for (auto it = consumeRange.first; it != consumeRange.second; it++) {
                SeqReadVertex* const seqReadp = it->second;
                seqReadp->foreachOutEdge([&](ResyncEdge* outp) {
                    ResyncVertex* top = dynamic_cast<ResyncVertex*>(outp->top());
                    otherp->addEdge(seqCombp, top, seqReadp->vscp());
                });
                VL_DO_DANGLING(seqReadp->unlinkDelete(otherp), seqReadp);
                VL_DANGLING(it->second);
            }

            if (otherp->heapNodep()) {
                // If otherp is in the heap, remove it so as to not resync it later
                // In general we maybe able to resync some graph whose one its producers
                // has been resynced into it, but that requires extra machinery to ensure
                // correct latching behavior and initialization.
                m_statsTransitivelyDisabled++;
                removeFromHeap(otherp);
            }
        }
        // delete all that has been resynced, that is anything reachable from sourcesp
        std::vector<V3GraphVertex*> deletersp{sourcesp.cbegin(), sourcesp.cend()};
        while (!deletersp.empty()) {
            V3GraphVertex* backp = deletersp.back();
            deletersp.pop_back();
            UASSERT(backp->inEmpty(), "expected no pred");
            for (V3GraphEdge* outp = backp->outBeginp(); outp; outp = outp->outNextp()) {
                if (outp->top()->inSize1()) { deletersp.push_back(outp->top()); }
            }
            VL_DO_DANGLING(backp->unlinkDelete(graphp), backp);
        }
        removeFromHeap(graphp);
    }
    void doApply() {

        // insert all graphs into the heap
        for (const auto& graphp : m_allGraphsp) { insertToHeap(graphp.get()); }
        if (m_heap.empty()) {
            return;
        } else {
            m_statsCostBefore = m_heap.max()->key().graphp->cost();
        }
        // Resync starting from the most costly one.
        // Once a graph is resynced, it will not be resynced again and all other graphs
        // that depend on it will also not be resynced
        while (!m_heap.empty()) { tryResync(m_heap.max()->key().graphp); }
    }

public:
    explicit ResyncGraphTransformer(std::vector<std::unique_ptr<ResyncGraph>>& graphsp)
        : m_allGraphsp{graphsp} {}
    ~ResyncGraphTransformer() {
        auto append = [](const std::string& desc, VDouble0 v) {
            V3Stats::addStat("Optimizations, resync " + desc, v);
        };
        append("partitions", m_statsNumTransformed);
        append("cost before", m_statsCostBefore);
        append("cost after", m_statsCostAfter);
        append("transitively disabled", m_statsTransitivelyDisabled);
        append("unoptimizable", m_statsUnoptDisabled);
        append("unable to improve", m_statsUnableDisabled);
        append("improved", m_statsNumTransformed);
    }
    inline void apply() { doApply(); }
    inline bool changed() { return m_changed; }
};

class ResyncVisitor final : VNVisitor {
private:
    V3UniqueNames m_newNames;
    AstNetlist* m_netlistp;
    std::vector<std::unique_ptr<ResyncGraph>>& m_graphsp;
    V3Sched::LogicClasses& m_logicClasses;

    AstSenTree* m_combSensep = nullptr;
    AstSenTree* m_initialSensep = nullptr;
    const VNUser1InUse m_user1InUse;
    const VNUser2InUse m_user2InUse;
    const VNUser3InUse m_user3InUse;

    using VarScopeByDomain = std::unordered_map<AstSenTree*, AstVarScope*>;
    using LogicCloneByDomain = std::unordered_map<AstSenTree*, AstNode*>;
    AstUser2Allocator<AstVarScope, VarScopeByDomain> m_newVscpByDomain;
    AstUser3Allocator<AstNode, LogicCloneByDomain> m_newLogicByDomain;
    // STATE:
    // AstVarScope::user1p()        -> pointer to new AstVarScope, clear on each graph
    // AstVarScope::user2u()        -> pointer to new AstVarScope per domain, clear on
    // construction AstNode::user2u()            -> clone of the logic by domain, clear on
    // construction

    AstVarScope* makeVscp(AstVarScope* const oldp) {
        FileLine* flp = oldp->fileline();
        AstVar* const varp = new AstVar{flp, VVarType::VAR, m_newNames.get(oldp->varp()->name()),
                                        oldp->varp()->dtypep()};

        varp->lifetime(VLifetime::AUTOMATIC);
        oldp->scopep()->modp()->addStmtsp(varp);
        AstVarScope* const newp = new AstVarScope{flp, oldp->scopep(), varp};
        newp->scopep()->addVarsp(newp);
        return newp;
    }
    inline AstVarRef* mkVRef(AstVarScope* const vscp, VAccess access) {
        return new AstVarRef{vscp->fileline(), vscp, access};
    }
    inline AstVarRef* mkLV(AstVarScope* const vscp) { return mkVRef(vscp, VAccess::WRITE); }
    inline AstVarRef* mkRV(AstVarScope* const vscp) { return mkVRef(vscp, VAccess::READ); }

    struct LocalSubst {
        static void clearAll() { AstNode::user1ClearTree(); }
        static inline void set(AstVarScope* oldp, AstVarScope* newp) { oldp->user1p(newp); }
        static inline AstVarScope* get(AstVarScope* oldp) {
            return VN_CAST(oldp->user1p(), VarScope);
        }
    };
    void setSenTrees() {

        for (AstSenTree* sentreep = m_netlistp->topScopep()->senTreesp(); sentreep;
             sentreep = VN_AS(sentreep->nextp(), SenTree)) {
            for (AstSenItem* itemp = sentreep->sensesp(); itemp;
                 itemp = VN_AS(itemp->nextp(), SenItem)) {
                if (itemp->isCombo()) {
                    m_combSensep = sentreep;
                } else if (itemp->isInitial()) {
                    m_initialSensep = sentreep;
                }
            }
        }
        if (!m_combSensep) {
            m_combSensep
                = new AstSenTree{m_netlistp->fileline(),
                                 new AstSenItem{m_netlistp->fileline(), AstSenItem::Combo{}}};
            m_netlistp->topScopep()->addSenTreesp(m_combSensep);
        }
        if (!m_initialSensep) {
            m_initialSensep
                = new AstSenTree{m_netlistp->fileline(),
                                 new AstSenItem{m_netlistp->fileline(), AstSenItem::Initial{}}};
            m_netlistp->topScopep()->addSenTreesp(m_initialSensep);
        }
    }

    // Create copies for variables created by CombSeqVertex, these are now "sequential"
    void markSubstOrCreateNewVscp(AstVarScope* const oldVscp, AstSenTree* const sentreep) {
        // Create a "sequential" version of this variable
        if (!m_newVscpByDomain(oldVscp).count(sentreep)) {
            AstVarScope* const newVscp = makeVscp(oldVscp);
            LocalSubst::set(oldVscp, newVscp);
            m_newVscpByDomain(oldVscp).emplace(sentreep, newVscp);
            FileLine* flp = oldVscp->fileline();
            UINFO(8, "creating sampler " << newVscp->prettyNameQ() << " for "
                                         << oldVscp->prettyNameQ() << endl);
            AstAssign* const assignp = new AstAssign{flp, mkLV(newVscp), mkRV(oldVscp)};
            AstAlways* const newAlwaysp
                = new AstAlways{flp, VAlwaysKwd::ALWAYS_FF, nullptr, assignp};
            AstActive* const newActivep = new AstActive{flp, "resync::combseq", sentreep};
            newActivep->addStmtsp(newAlwaysp);
            oldVscp->scopep()->addBlocksp(newActivep);
        } else {
            // sequential version exists
            AstVarScope* const newVscp = m_newVscpByDomain(oldVscp)[sentreep];
            UINFO(8, "Using cached sampler " << newVscp->prettyNameQ() << endl);
            LocalSubst::set(oldVscp, newVscp);
        }
    }
    inline void markSubstCombSeq(CombSeqVertex* vtxp) {
        AstVarScope* const oldVscp = vtxp->vscp();
        markSubstOrCreateNewVscp(oldVscp, vtxp->sentreep());
    }
    inline void markSubstCombSeqRead(CombSeqReadVertex* vtxp) {
        AstVarScope* const oldVscp = vtxp->vscp();
        markSubstOrCreateNewVscp(oldVscp, vtxp->sentreep());
    }

    // mark any outgoing edge from ComCombVertex to be renamed as well
    void markSubstCombComb(CombCombVertex* vtxp) {
        vtxp->foreachOutEdge([&](ResyncEdge* edgep) {
            AstVarScope* const oldVscp = edgep->vscp();
            if (!m_newVscpByDomain(oldVscp).count(vtxp->sentreep())) {

                AstVarScope* const newVscp = makeVscp(oldVscp);
                LocalSubst::set(oldVscp, newVscp);
                m_newVscpByDomain(oldVscp).emplace(vtxp->sentreep(), newVscp);
                UINFO(8, "Creating new comb signal " << newVscp->prettyNameQ() << " for "
                                                     << oldVscp->prettyNameQ() << endl);
            }
        });
    }

    void cloneCombComb(CombCombVertex* vtxp) {
        // Combinational logic that has been pushed. Needs to be freshly cloned per unique
        // senseTree
        AstNode* newp = nullptr;
        if (!m_newLogicByDomain(vtxp->logicp().logicp).count(vtxp->sentreep())) {
            UINFO(8, "Reconstructing 'pushed-down' combinational logic "
                         << vtxp->logicp().logicp << " under " << vtxp->logicp().activep << endl);
            AstNode* const newp = vtxp->logicp().logicp->cloneTree(false);

            m_newLogicByDomain(vtxp->logicp().logicp).emplace(vtxp->sentreep(), newp);

            // add it to its active
            vtxp->logicp().activep->addStmtsp(newp);

            // relink the active
            relinkActive(vtxp->logicp().activep);
            applySubst(newp);
        }
    }

    void cloneSeqComb(SeqCombVertex* vtxp) {
        // Sequential made into combinational
        AstAlways* const newAlwaysp
            = new AstAlways{m_netlistp->fileline(), VAlwaysKwd::ALWAYS_COMB, nullptr, nullptr};
        AstActive* const newActivep
            = new AstActive{m_netlistp->fileline(), "resync::seqcomb", m_combSensep};
        m_netlistp->topScopep()->scopep()->addBlocksp(newActivep);
        newActivep->addStmtsp(newAlwaysp);
        for (const LogicWithActive& pair : vtxp->logicsp()) {
            UINFO(8, "Constructing comb from seq " << pair.logicp << endl);
            if (VN_IS(pair.logicp, AssignPost) || VN_IS(pair.logicp, AssignPre)) {
                // cannot have AssignPost and AssignPre under Always (V3BspGraph fails
                // otherwise)
                AstNodeAssign* assignOldp = VN_AS(pair.logicp, NodeAssign);
                // rewrite as vanilla Assign
                AstAssign* newp
                    = new AstAssign{assignOldp->fileline(), assignOldp->lhsp()->unlinkFrBack(),
                                    assignOldp->rhsp()->unlinkFrBack()};
                newAlwaysp->addStmtsp(newp);
                VL_DO_DANGLING(assignOldp->unlinkFrBack()->deleteTree(), assignOldp);
            } else if (AstNodeProcedure* blockp = VN_CAST(pair.logicp, NodeProcedure)) {
                newAlwaysp->addStmtsp(blockp->stmtsp()->unlinkFrBackWithNext());
                VL_DO_DANGLING(blockp->unlinkFrBack()->deleteTree(), blockp);
            } else {
                UASSERT_OBJ(false, pair.logicp,
                            "unknown node type " << pair.logicp->prettyTypeName() << endl);
            }
        }

        LocalSubst::clearAll();

        vtxp->foreachInEdge([&](ResyncEdge* edgep) {
            UASSERT(dynamic_cast<CombCombVertex*>(edgep->fromp())
                        || dynamic_cast<CombSeqReadVertex*>(edgep->fromp()),
                    "unexpected fromp type");
            UASSERT(vtxp->sentreep(), "something is up, resync logic with multiple domains?");
            auto it = m_newVscpByDomain(edgep->vscp()).find(vtxp->sentreep());
            if (it != m_newVscpByDomain(edgep->vscp()).end()) {
                LocalSubst::set(edgep->vscp(), it->second);
            }
        });
        // replace any RV that has a subst, potentially coming from CombSeqRead or CombComb
        applySubst(newAlwaysp);

        LocalSubst::clearAll();
        fixBehavSeqComb(vtxp, newAlwaysp);
    }

    void fixBehavSeqComb(SeqCombVertex* vtxp, AstAlways* newAlwaysp) {

        // Create a clone of every LV in the transformed logic that replaced the original
        // instances in the initial/static initial logic

        for (AstVarScope* const vscp : vtxp->lvsp()) {
            AstVarScope* initVscp = makeVscp(vscp);
            LocalSubst::set(vscp, initVscp);
        }
        AstScope* const scopep = m_netlistp->topScopep()->scopep();
        m_logicClasses.m_static.foreachLogic([this](AstNode* nodep) { applySubst(nodep); });
        m_logicClasses.m_static.foreachLogic([this](AstNode* nodep) { applySubst(nodep); });
        FileLine* flp = m_netlistp->fileline();
        AstVar* const firstVarp = new AstVar{flp, VVarType::VAR, m_newNames.get("init"),
                                             m_netlistp->findUInt32DType()};
        AstVarScope* const firstVscp
            = new AstVarScope{flp, m_netlistp->topScopep()->scopep(), firstVarp};
        scopep->addVarsp(firstVscp);
        scopep->modp()->addStmtsp(firstVarp);

        AstActive* const firstActivep = new AstActive{flp, "resync::first", m_initialSensep};
        scopep->addBlocksp(firstActivep);
        AstInitial* const initBlockp = new AstInitial{flp, nullptr};
        firstActivep->addStmtsp(initBlockp);
        initBlockp->addStmtsp(new AstAssign{flp, mkLV(firstVscp), new AstConst{flp, 1U}});

        // create a new variable for each variable that was commited in the original
        // sequential block:
        // for each vscp commit create newVscp
        // then create:
        // always_comb
        //      if (initVscp)
        //         vscp = initValue
        //      else
        //         vscp = newVscp;
        //         rest of the original seq logic
        // always_ff @(sentree)
        //      newVscp = vscp;
        //      initVscp = 0;
        // Note that setting vscp = newValue is necessary to simulate the "latching" behavior
        // and also to correctly set the initial value. Latching behavior is needed even in the
        // simplest case: always_ff
        //      counter = counter + 1
        // simply turned into a comb block results in a comb loop:
        // always_comb counter = counter + 1;
        // but with the above transformation we'll have:
        // always_comb:
        //      if (initCounter)
        //          counter = initial value of the counter, if any, otherwise garbage
        //      else
        //          counter = newCounter;
        //          counter = counter + 1;
        // always_ff
        //      newCounter = counter;
        //      initCounter = 0;
        // Now this new program has no comb loops and is behaviorally equivalent

        // first create the newVscp and the corresponding active

        AstActive* const seqActivep = new AstActive{flp, "resync::seqseq", vtxp->sentreep()};
        scopep->addBlocksp(seqActivep);
        AstAlways* const seqAlwaysp = new AstAlways{flp, VAlwaysKwd::ALWAYS_FF, nullptr, nullptr};
        seqActivep->addStmtsp(seqAlwaysp);

        seqAlwaysp->addStmtsp(new AstAssign{flp, mkLV(firstVscp), new AstConst{flp, 0U}});

        AstIf* const ifp = new AstIf{flp, mkRV(firstVscp), nullptr, nullptr};

        for (AstVarScope* const vscp : vtxp->lvsp()) {
            AstVarScope* const newVscp = makeVscp(vscp);
            AstVarScope* const initVscp = LocalSubst::get(vscp);
            UASSERT_OBJ(initVscp, vscp, "initVscp not found");
            ifp->addThensp(new AstAssign{flp, mkLV(vscp), mkRV(initVscp)});
            ifp->addElsesp(new AstAssign{flp, mkLV(vscp), mkRV(newVscp)});
            seqAlwaysp->addStmtsp(new AstAssign{flp, mkLV(newVscp), mkRV(vscp)});
        }

        ifp->addElsesp(newAlwaysp->stmtsp()->unlinkFrBackWithNext());
        newAlwaysp->addStmtsp(ifp);
        V3Const::constifyEdit(newAlwaysp);
    }

    inline void relinkActive(AstActive* activep) {
        if (!activep->backp()) {
            UINFO(11, "Relink " << activep << endl);
            m_netlistp->topScopep()->scopep()->addBlocksp(activep);
        }
    }
    inline void relinkComb(CombVertex* vtxp) { relinkActive(vtxp->logicp().activep); }
    inline void relinkSeq(SeqVertex* vtxp) {
        for (const LogicWithActive& pair : vtxp->logicsp()) { relinkActive(pair.activep); }
    }

    void reconstruct(const std::unique_ptr<ResyncGraph>& graphp) {

        if (dump() > 10) {
            graphp->dumpDotFilePrefixed("resync_post_" + cvtToStr(graphp->index()));
        }
        LocalSubst::clearAll();

        // create new variables if needed
        graphp->foreachVertex([this](ResyncVertex* vp) {
            if (auto vtxp = dynamic_cast<CombSeqVertex*>(vp)) {
                markSubstCombSeq(vtxp);
            } else if (auto vtxp = dynamic_cast<CombSeqReadVertex*>(vp)) {
                markSubstCombSeqRead(vtxp);
            } else if (auto vtxp = dynamic_cast<CombCombVertex*>(vp)) {
                markSubstCombComb(vtxp);
            }
        });

        // clone or relink logic, handle SeqComb later
        graphp->foreachVertex([&](ResyncVertex* vp) {
            if (auto vtxp = dynamic_cast<CombCombVertex*>(vp)) {
                cloneCombComb(vtxp);
            } else if (auto vtxp = dynamic_cast<CombVertex*>(vp)) {
                relinkComb(vtxp);
            } else if (auto vtxp = dynamic_cast<SeqVertex*>(vp)) {
                relinkSeq(vtxp);
            }
        });

        LocalSubst::clearAll();
        // deal with seqentual logic turn comb last since it requires new substitutions
        graphp->foreachVertex([&](SeqCombVertex* vtxp) { cloneSeqComb(vtxp); });
        // at last, fix the behavior of seq logic that was turned into comb. Need to happen
        // after everything else as there are new substitutions
    }

    void applySubst(AstNode* nodep) { iterateChildren(nodep); }

    // substitute reference with what's in user1p
    void visit(AstNodeVarRef* vrefp) {
        AstVarScope* const oldVscp = vrefp->varScopep();
        AstVarScope* const newVscp = LocalSubst::get(oldVscp);
        if (!newVscp) { return; }
        vrefp->name(newVscp->varp()->name());
        vrefp->varp(newVscp->varp());
        vrefp->varScopep(newVscp);
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit ResyncVisitor(AstNetlist* netlistp,
                           std::vector<std::unique_ptr<ResyncGraph>>& graphsp,
                           V3Sched::LogicClasses& logicClasses)
        : m_newNames{"__Vresync"}
        , m_netlistp{netlistp}
        , m_graphsp{graphsp}
        , m_logicClasses{logicClasses} {
        AstNode::user2ClearTree();
        AstNode::user1ClearTree();
        setSenTrees();
        for (auto& graphp : graphsp) {
            reconstruct(graphp);
            VL_DANGLING(graphp);
        }
        graphsp.clear();
    }
};

class ResyncLegalVisitor final : VNVisitor {
private:
    AstScope* m_scopep = nullptr;
    AstScope* m_activeScopep = nullptr;
    bool m_allowed = true;

    void visit(AstActive* activep) override {
        if (m_activeScopep && m_activeScopep != m_scopep) { m_allowed = false; }
        m_activeScopep = m_scopep;
    }
    void visit(AstScope* scopep) override {
        UASSERT(!m_scopep, "nested scopes");
        VL_RESTORER(m_scopep);
        {
            m_scopep = scopep;
            iterateChildren(scopep);
        }
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }
    explicit ResyncLegalVisitor(AstNetlist* netlistp) {
        m_allowed = true;
        iterate(netlistp);
    }

public:
    // returns true if all actives fall under the same scope
    static bool allowed(AstNetlist* netlistp) { return ResyncLegalVisitor{netlistp}.m_allowed; }
};

void resyncAll(AstNetlist* netlistp) {

    if (ResyncLegalVisitor::allowed(netlistp)) {
        bool changed = false;

        v3Global.dumpCheckGlobalTree("preresync", 0, dumpTree() >= 3);
        auto deps = V3BspSched::buildDepGraphs(netlistp);
        auto& depGraphsp = std::get<2>(deps);
        auto& regions = std::get<1>(deps);
        auto& logicClasses = std::get<0>(deps);
        if (!regions.m_act.empty()) {
            regions.m_act.front().second->v3fatalExit("Active regions prevent resync");
            // do better than failing...
            return;
        }
        auto resyncGraphsp = ResyncGraphBuilder::build(depGraphsp);
        {
            ResyncGraphTransformer resyncer{resyncGraphsp};
            resyncer.apply();
            changed = resyncer.changed();
        }

        { ResyncVisitor{netlistp, resyncGraphsp, logicClasses}; }
        v3Global.dumpCheckGlobalTree("postresync", 0, dumpTree() >= 3);
        V3Dead::deadifyAllScoped(netlistp);

    } else {
        netlistp->v3warn(UNOPTFLAT, "Skipping resync. Is the design not flattened?");
    }
}
}  // namespace Resync
}  // namespace V3BspSched