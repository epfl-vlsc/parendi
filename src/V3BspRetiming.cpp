// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: BSP retiming
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

#include "V3BspRetiming.h"

#include "V3AstUserAllocator.h"
#include "V3BspGraph.h"
#include "V3BspNetlistGraph.h"
#include "V3BspSched.h"
#include "V3Dead.h"
#include "V3File.h"
#include "V3Hasher.h"
#include "V3InstrCount.h"
#include "V3PairingHeap.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <algorithm>
VL_DEFINE_DEBUG_FUNCTIONS;
namespace V3BspSched {
namespace Retiming {

class RetimerVisitor final : VNVisitor {
private:
    struct RetimingLedger {
        std::unordered_set<NetlistGraph*> dontTouch;
        std::unordered_map<NetlistGraph*, uint32_t> retimeRank;
        void illegal(NetlistGraph* graphp) { dontTouch.insert(graphp); }
        bool legal(NetlistGraph* graphp) { return dontTouch.count(graphp) == 0; }
        void notify(NetlistGraph* graphp, uint32_t rank) { retimeRank.emplace(graphp, rank); }
    };

    RetimingLedger m_ledger;
    AstNetlist* const m_netlistp;
    V3UniqueNames m_newNames;
    AstSenTree* m_combSenTreep = nullptr;
    AstSenTree* m_initSenTreep = nullptr;
    V3Sched::LogicClasses m_logicClasses;

    const VNUser1InUse user1InUse;
    const VNUser2InUse user2InUse;
    enum ReplacementAction {
        VU_NOACTION = 0,
        VU_SAMPLE = 1,
        VU_LVSUBST = 2,
        VU_CLONECLEAN = 3,
        VU_INITSUBST = 4
    };
    // const VNUser3InUse user3InUse;
    // AstUser2Allocator<AstVarScope, std::unordered_map<AstSenTree*, AstVarScope*>>
    // m_replacements; AstUser2Allocator<AstScope, AstAlways*> m_guards; reset on partition
    // AstVarScope::user1p()  -> SeqWriteVertex* of the variable
    // AstVarScope::user1()   -> ReplacementAction
    // AstNode::user1()       -> ReplacementAction
    // AstVarScope::user2p()  -> replacemet variable scope
    // AstScope::user2p()     -> comb always block to replace the sequential block

    std::vector<std::unique_ptr<NetlistGraph>>
    buildNetlistGraphs(const std::vector<std::unique_ptr<DepGraph>>& partitionsp) {

        std::vector<SeqReadVertex*> readsp;
        std::vector<SeqWriteVertex*> writesp;
        AstNode::user1ClearTree();

        std::vector<std::unique_ptr<NetlistGraph>> allGraphsp;

        // go through all the partitions and for each one create a netlistgrpah and
        // also the "sequential register" updated by the partition. A sequential register
        // is in fact a collection of (possibly many) sequential active blocks that
        // commit some values
        int graphIndex = 0;
        for (const auto& partp : partitionsp) {
            NetlistGraph* netGraphp = new NetlistGraph{};
            uint32_t seqCost = 0;
            std::vector<LogicWithDomain> logicsp;
            std::vector<AstVarScope*> commitsp;
            for (V3GraphVertex* vtxp = partp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                ConstrCommitVertex* const commitVtxp = dynamic_cast<ConstrCommitVertex*>(vtxp);
                if (commitVtxp) { commitsp.push_back(commitVtxp->vscp()); }
                CompVertex* const compVtxp = dynamic_cast<CompVertex*>(vtxp);
                if (compVtxp && compVtxp->domainp()) {
                    // seq logic
                    logicsp.emplace_back(compVtxp->domainp(), compVtxp->nodep());
                    seqCost += V3InstrCount::count(compVtxp->nodep(), false);
                }
            }
            UASSERT(!logicsp.empty(), "empty seq?");
            SeqWriteVertex* writep = new SeqWriteVertex{netGraphp, std::move(logicsp), seqCost};
            for (AstVarScope* vscp : commitsp) { vscp->user1u(VNUser{writep}); }
            allGraphsp.emplace_back(netGraphp);
            writesp.push_back(writep);
            partp->dumpDotFilePrefixedAlways("net_partition_" + std::to_string(graphIndex++));
        }
        graphIndex = 0;
        for (const auto& partp : partitionsp) {
            NetlistGraph* const netGraphp = allGraphsp[graphIndex].get();
            SeqWriteVertex* const thisSeqp = writesp[graphIndex];
            graphIndex++;

            partp->userClearVertices();
            auto getCombVertex = [&](CompVertex* vtxp) {
                if (!vtxp->userp()) {
                    CombVertex* newp = new CombVertex{netGraphp, vtxp->nodep(),
                                                      V3InstrCount::count(vtxp->nodep(), false)};
                    vtxp->userp(newp);
                }
                return static_cast<CombVertex*>(vtxp->userp());
            };
            for (V3GraphVertex* vtxp = partp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                ConstrDefVertex* const defp = dynamic_cast<ConstrDefVertex*>(vtxp);
                if (defp) {
                    if (defp->outEmpty()) {
                        continue;  // is it dead?
                    }
                    if (defp->outSize1()) {
                        CompVertex* const succp
                            = dynamic_cast<CompVertex*>(defp->outBeginp()->top());
                        if (VN_IS(succp->nodep(), AssignPre)) {
                            // these ones are artifical connections
                            continue;
                        }
                    }
                    if (defp->inSize1()) {
                        CompVertex* const prevp
                            = dynamic_cast<CompVertex*>(defp->inBeginp()->fromp());
                        if (VN_IS(prevp->nodep(), AssignPre)) { continue; }
                    }

                    SeqWriteVertex* const seqp = defp->vscp()->user1u().to<SeqWriteVertex*>();
                    NetlistVertex* predVtxp = nullptr;
                    if (seqp) {
                        // seq -> def
                        // there should be either no predecessor or a predecessor that
                        // is an AssignPre
                        UASSERT_OBJ(
                            defp->inEmpty()
                                || (defp->inSize1()
                                    && VN_IS(dynamic_cast<CompVertex*>(defp->inBeginp()->fromp())
                                                 ->nodep(),
                                             AssignPre)),
                            defp->vscp(), "did not expect predecessor");
                        SeqReadVertex* readp = new SeqReadVertex{netGraphp, defp->vscp()};
                        seqp->addReadp(netGraphp);
                        predVtxp = readp;
                    } else if (!defp->inEmpty()) {
                        UASSERT_OBJ(defp->inSize1(), defp->vscp(),
                                    "expected exactly one predecessor");
                        // either from AssignPre or from comb logic
                        CompVertex* predp = dynamic_cast<CompVertex*>(defp->inBeginp()->fromp());
                        UASSERT_OBJ(predp, defp->vscp(),
                                    "ill-constructed graph, expected CompVertex");
                        if (!predp->domainp()) { predVtxp = getCombVertex(predp); }
                    }
                    if (!predVtxp) {
                        // defp of some value that is set by initial blocks, effectively constat
                        continue;
                    }
                    // connect prevp -> defp -> succp as prevp -> succp for any succp
                    for (V3GraphEdge* edgep = defp->outBeginp(); edgep;
                         edgep = edgep->outNextp()) {
                        CompVertex* const succp = dynamic_cast<CompVertex*>(edgep->top());
                        UASSERT(succp, "ill-constructed graph, expected ComptVertex");
                        if (succp->domainp()) {
                            // downstream register
                            netGraphp->addEdge(predVtxp, thisSeqp, defp->vscp());
                        } else {
                            // comb to comb connection
                            netGraphp->addEdge(predVtxp, getCombVertex(succp), defp->vscp());
                        }
                    }
                }
            }

            netGraphp->dumpDotFilePrefixedAlways("netlist_" + std::to_string(graphIndex - 1));
        }

        return allGraphsp;
    }

    void initializeCostValues(const std::unique_ptr<NetlistGraph>& graphp) {

        std::vector<NetlistVertex*> sourcesp, sinksp;
        // rank each vertex, entry nodes will have  rank 1 and the the exist nodes
        // will have the higher rank
        graphp->rank();
        // sort based on rank, i.e., topological order
        graphp->sortVertices();

        std::vector<NetlistVertex*> vertices;
        uint32_t totalCost = 0;
        for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            NetlistVertex* const vp = dynamic_cast<NetlistVertex*>(vtxp);
            vertices.push_back(vp);
            totalCost += vp->cost();
        }
        graphp->cost(totalCost);

        // t(op)value computation for each vertex: in topological order
        // v.tvalue = sum([u.tvalue for (u, v) in v.in])
        for (NetlistVertex* vtxp : vertices) {
            uint32_t t = 0;
            for (V3GraphEdge* edgep = vtxp->inBeginp(); edgep; edgep = edgep->inNextp()) {
                t += dynamic_cast<NetlistVertex*>(edgep->fromp())->cost();
            }
            vtxp->tvalue(t);
        }
        // b(ottom)value compute for each vertex: in reverse topological order
        // v.bvalue = sum([u.value for (v, u) in v.out]) + sum([u for u : u.rank == v.rank])
        std::vector<uint32_t> rankSum(vertices.back()->rank());
        std::fill(rankSum.begin(), rankSum.end(), 0);
        for (NetlistVertex* vtxp : vertices) {
            UASSERT(vtxp->rank(), "not ranked");
            UASSERT(vtxp->rank() <= rankSum.size(), "invalid rank");
            rankSum[(vtxp->rank() - 1)] += vtxp->cost();
        }
        for (NetlistVertex* vtxp : vertices) { vtxp->rvalue(rankSum[vtxp->rank() - 1]); }

        for (int i = rankSum.size() - 2; i >= 0; i--) { rankSum[i] += rankSum[i + 1]; }

        for (NetlistVertex* vtxp : vertices) { vtxp->bvalue(rankSum[vtxp->rank() - 1]); }
    }

    void markRetiming(NetlistGraph* const graphp) {
        const uint32_t costToBeat = graphp->cost();
        UINFO(10, "Retiming with worst-case cost " << costToBeat << endl);
        std::vector<std::vector<NetlistVertex*>> vtxpByRank;
        SeqWriteVertex* seqWritep = nullptr;
        for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            if (!vtxp->verticesNextp()) {
                seqWritep = dynamic_cast<SeqWriteVertex*>(vtxp);
                break;  // the last vertex does not matter, since we are trying to move that one up
                        // the graph
            }
            if (vtxp->rank() > vtxpByRank.size()) { vtxpByRank.emplace_back(); }
            vtxpByRank.back().push_back(dynamic_cast<NetlistVertex*>(vtxp));
        }
        UASSERT(seqWritep, "expected a SeqWriteVertex");
        // auto hasImpure = [](AstNode* nodep) { return !nodep->isPure(); };
        if (std::any_of(
                seqWritep->logicsp().cbegin(), seqWritep->logicsp().cend(), [](const auto& pair) {
                    return pair.second->exists([](AstNode* nodep) { return !nodep->isPure(); });
                })) {
            // can not retime impure
            UINFO(3, "impure graph will not be retimed" << endl);
            m_ledger.illegal(graphp);
        }
        if (seqWritep->readsp().empty()) {
            UINFO(3, "empty readers, will not be retimed" << endl);
            m_ledger.illegal(graphp);
        }
        std::unordered_set<AstSenTree*> diffSenses;
        for (const auto lp : seqWritep->logicsp()) { diffSenses.insert(lp.first); }
        if (diffSenses.size() > 1) {
            UINFO(3, "multi-domain logic can not be retimed" << endl);
            m_ledger.illegal(graphp);
        }

        if (!m_ledger.legal(graphp)) { return; }
        // starting from the bottom, try to find a new place for the final seq block
        uint32_t bestRetimingRank = vtxpByRank.back().front()->rank() + 1;
        uint32_t bestReducedCost = costToBeat;
        bool canRetime = false;
        for (const auto& vtxsInRank : vlstd::reverse_view(vtxpByRank)) {
            // does it make sense to push the final register to the outwards edges of
            // this level/rank?
            auto slowestVtxpAboveHere = *std::max_element(
                vtxsInRank.begin(), vtxsInRank.end(), [](const auto* vp1, const auto* vp2) {
                    return (vp1->tvalue() + vp1->cost()) < (vp2->tvalue() + vp2->cost());
                });

            // how much improvement do we get from the current costToBeat?
            NetlistGraph* const slowestDownStream = seqWritep->slowestReader();
            uint32_t costAbove = slowestVtxpAboveHere->tvalue() + slowestVtxpAboveHere->cost();
            uint32_t costBelow = slowestDownStream->cost() + vtxsInRank.front()->bvalue();
            // we can not have the cost above us to ever increase, since we are removing the higher
            // ranked vertices
            UASSERT(costAbove <= costToBeat, "something is not right with the netlist graph");
            // but we can have cost below to surpass the existing worst cost, since we may
            // push the higher ranked vertices to another graph that is already critical
            UINFO(3, "at rank " << slowestVtxpAboveHere->rank() << " cabove = " << costAbove
                                << " cbelow = " << costBelow
                                << " down = " << slowestDownStream->cost() << endl);
            if (costBelow > costToBeat) {
                // tough luck, continuing does not make sense since we are bound to
                // only increase the costBelow
                break;
            }
            const uint32_t costAfterRetimingHere = std::max(costAbove, costBelow);
            if (costAfterRetimingHere <= bestReducedCost) {
                bestReducedCost = costAfterRetimingHere;
                bestRetimingRank = slowestVtxpAboveHere->rank();
                canRetime = true;
            }
        }
        if (canRetime) {
            UINFO(3, "Can slice up in rank " << bestRetimingRank << " which reduces cost to "
                                             << bestReducedCost << endl);
            m_ledger.notify(graphp, bestRetimingRank);
            for (auto& readp : seqWritep->readsp()) {
                m_ledger.illegal(readp);  // make any further retiming to downstream
                // graphs illegal
            }
        } else {
            UINFO(3, "Can not retime " << endl);
        }
    }
    AstSenTree* findSenItemComb() {
        // find the COMBO sentree
        if (!m_combSenTreep) {
            for (AstSenTree* sentreep = m_netlistp->topScopep()->senTreesp(); sentreep;
                 sentreep = VN_AS(sentreep->nextp(), SenTree)) {
                if (sentreep->hasCombo()) { m_combSenTreep = sentreep; }
            }
            if (!m_combSenTreep) {
                m_combSenTreep
                    = new AstSenTree{m_netlistp->fileline(),
                                     new AstSenItem{m_netlistp->fileline(), AstSenItem::Combo{}}};
                m_netlistp->topScopep()->addSenTreesp(m_combSenTreep);
            }
        }
        return m_combSenTreep;
    }
    AstSenTree* findSenItemInit() {
        if (!m_initSenTreep) {
            for (AstSenTree* sentreep = m_netlistp->topScopep()->senTreesp(); sentreep;
                 sentreep = VN_AS(sentreep->nextp(), SenTree)) {
                if (sentreep->hasInitial()) { m_initSenTreep = sentreep; }
            }
            if (!m_initSenTreep) {
                m_initSenTreep = new AstSenTree{
                    m_netlistp->fileline(),
                    new AstSenItem{m_netlistp->fileline(), AstSenItem::Initial{}}};
                m_netlistp->topScopep()->addSenTreesp(m_initSenTreep);
            }
        }
        return m_initSenTreep;
    }
    AstVarScope* makeVscp(AstVarScope* const vscp) {
        AstVar* const varp
            = new AstVar{vscp->fileline(), VVarType::VAR, m_newNames.get(vscp->varp()->name()),
                         vscp->varp()->dtypep()};
        varp->lifetime(VLifetime::STATIC);
        AstVarScope* const newVscp = new AstVarScope{vscp->fileline(), vscp->scopep(), varp};
        vscp->scopep()->addVarsp(newVscp);
        vscp->scopep()->modp()->addStmtsp(varp);
        // keep track of oldVscp -> newVcsp
        vscp->user2p(newVscp);
        return newVscp;
    }

    void applyRetiming(std::unique_ptr<NetlistGraph>&& graphp,
                       std::unique_ptr<DepGraph>&& depGraphp) {
        AstNode::user1ClearTree();
        AstNode::user2ClearTree();
        auto it = m_ledger.retimeRank.find(graphp.get());
        if (it == m_ledger.retimeRank.end()) {
            for (V3GraphVertex* dvtxp = depGraphp->verticesBeginp(); dvtxp;
                 dvtxp = dvtxp->verticesNextp()) {
                if (CompVertex* const compp = dynamic_cast<CompVertex*>(dvtxp)) {
                    if (!compp->activep()->backp()) {
                        m_netlistp->topScopep()->scopep()->addBlocksp(compp->activep());
                    }
                }
            }
            graphp = nullptr;
            depGraphp = nullptr;
            return;  // not retimed
        }
        uint32_t cutRank = it->second;
        // anything below cutRank is made into comb logic and the vertices
        // at the cut rank are made into seq logic that samples all the values
        // produced earlier at the cutRank

        // Any vertex whose rank <= cutRank and has a successor whose rank > cutRank
        // needs to be sampled
        // std::vector<NetlistEdge*> crossingEdgesp;

        // find the sentree used to sample the combinational values
        AstSenTree* seqSenTreep = nullptr;
        for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            if (SeqWriteVertex* const seqWritep = dynamic_cast<SeqWriteVertex*>(vtxp)) {
                for (const auto& pair : seqWritep->logicsp()) {
                    UASSERT(!seqSenTreep || seqSenTreep == pair.first,
                            "multiple domains cannot be retimed");
                    seqSenTreep = pair.first;
                }
                UASSERT(seqSenTreep, "sequential logic with no domain");
            }
        }
        UASSERT(seqSenTreep, "sequential logic without domain");

        // create new variables for each combinational results, some are "sampled" by the
        // seqSenTreep, while others are simply cloned fresh. Note that cloning fresh
        // variables is necessary since we may retime a combinational block only in one partition,
        // but leave it as is in another.
        for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            if (vtxp->rank() <= cutRank) {
                for (V3GraphEdge* edgep = vtxp->outBeginp(); edgep; edgep = edgep->outNextp()) {
                    if (edgep->top()->rank() > cutRank) {
                        NetlistEdge* const netedgep = dynamic_cast<NetlistEdge*>(edgep);
                        UASSERT(netedgep, "invalid edge type");
                        // crossingEdgesp.push_back(netedgep);
                        if (!netedgep->vscp()->user2p()) {
                            AstVarScope* const vscp = netedgep->vscp();
                            AstVarScope* const newVscp = makeVscp(vscp);
                            vscp->user1(VU_SAMPLE);
                            vscp->user2p(newVscp);
                            UINFO(8, "variable will be sampled for retiming "
                                         << vscp->prettyNameQ() << endl);
                            AstAssign* const assignp = new AstAssign{
                                vscp->fileline(),
                                new AstVarRef{vscp->fileline(), newVscp, VAccess::WRITE},
                                new AstVarRef{vscp->fileline(), vscp, VAccess::READ}};
                            AstAlways* const newAlwaysp = new AstAlways{
                                vscp->fileline(), VAlwaysKwd::ALWAYS_FF, nullptr, assignp};
                            AstActive* const newActivep
                                = new AstActive{vscp->fileline(), "retimeseq", seqSenTreep};
                            newActivep->addStmtsp(newAlwaysp);
                            vscp->scopep()->addBlocksp(newActivep);
                        }
                    }
                }
            } else {
                if (CombVertex* const combp = dynamic_cast<CombVertex*>(vtxp)) {
                    // combo block, mark it to be freshly cloned later
                    combp->logicp()->user1(VU_CLONECLEAN);
                    UINFO(8, "Marking logic to be cloned " << combp->logicp() << endl);
                    for (V3GraphEdge* edgep = vtxp->outBeginp(); edgep;
                         edgep = edgep->outNextp()) {
                        NetlistEdge* const netedgep = dynamic_cast<NetlistEdge*>(edgep);
                        if (!netedgep->vscp()->user2p()) {
                            UINFO(8, "LValue will be duplicated "
                                         << netedgep->vscp()->prettyNameQ() << endl);
                            netedgep->vscp()->user1(VU_LVSUBST);
                            netedgep->vscp()->user2p(makeVscp(netedgep->vscp()));
                        }
                    }
                }
            }
        }

        // each crossing edge has a variable that needs to be sampled
        // by the the right SenTree, since the netlist graph has collapsed
        // all of the sequential logic into a single vertex, we may end up
        // sampling things that never get used in case of multiple SenTrees
        // always_comb:
        //    a = ...
        //    b = ...
        // always_ff @(pos clk1)
        //    x = a + ...
        // always_ff @(pos clk2)
        //    y = b + ..
        // Here we need to only sample a and b by the SenTrees that actually
        // reads them. This information is available in the dependence graph
        // but not the netlist graph. Essentially we need to find out the set
        // of active blocks that each variable on the cut edges feed into, and
        // use the SenTrees to create new sequential active blocks that sample
        // these variables.

        // find the COMBO sentree
        findSenItemComb();

        // create an active, and an always_comb to hold the sequential logic
        AstActive* const retimeActiveCombp
            = new AstActive{m_netlistp->fileline(), "retimecomb", m_combSenTreep};
        AstAlways* const retimeAlwaysComb
            = new AstAlways{m_netlistp->fileline(), VAlwaysKwd::ALWAYS_COMB, nullptr, nullptr};
        retimeActiveCombp->addStmtsp(retimeAlwaysComb);
        m_netlistp->topScopep()->scopep()->addBlocksp(retimeActiveCombp);

        // compute the topological order
        depGraphp->rank();
        depGraphp->sortVertices();

        std::set<AstVarScope*> commitedp;
        for (V3GraphVertex* vtxp = depGraphp->verticesBeginp(); vtxp;
             vtxp = vtxp->verticesNextp()) {
            if (ConstrCommitVertex* const commitp = dynamic_cast<ConstrCommitVertex*>(vtxp)) {
                commitedp.insert(commitp->vscp());
            }
            CompVertex* const compVtxp = dynamic_cast<CompVertex*>(vtxp);
            if (!compVtxp) continue;  // not logic

            AstNode* nodep = compVtxp->nodep();

            if (!compVtxp->activep()->backp()) {
                // put active back into the netlist if it is stranded. This is because
                // an earlier call to the V3Sched::partition snatched all logic out of
                // the netlist
                m_netlistp->topScopep()->scopep()->addBlocksp(compVtxp->activep());
            }
            if (compVtxp->domainp()) {  // sequential logic
                UASSERT(compVtxp->domainp() == seqSenTreep, "invalid domain");
                UINFO(15, "Transforming to comb logic:    " << nodep << endl);
                // turn sequential logic into combinational
                if (VN_IS(nodep, AssignPost) || VN_IS(nodep, AssignPre)) {
                    AstNodeAssign* assignOldp = VN_AS(nodep, NodeAssign);
                    // rewrite as vanilla Assign
                    AstAssign* newp
                        = new AstAssign{assignOldp->fileline(), assignOldp->lhsp()->unlinkFrBack(),
                                        assignOldp->rhsp()->unlinkFrBack()};
                    retimeAlwaysComb->addStmtsp(newp);
                    VL_DO_DANGLING(assignOldp->unlinkFrBack()->deleteTree(), assignOldp);
                    // rename variables
                } else if (AstNodeProcedure* blockp = VN_CAST(nodep, NodeProcedure)) {
                    retimeAlwaysComb->addStmtsp(blockp->stmtsp()->unlinkFrBackWithNext());
                    VL_DO_DANGLING(blockp->unlinkFrBack()->deleteTree(), blockp);
                } else {
                    // something is up
                    UASSERT_OBJ(false, nodep,
                                "unknown node type " << nodep->prettyTypeName() << endl);
                }

            } else {
                UINFO(10, "Reinstate logic " << nodep << endl);
                // comb logic or no retiming, keep as-is

                if (nodep->user1() == VU_CLONECLEAN) {
                    UINFO(8, "Fresh clone " << nodep << endl);
                    // need to create a fresh clone that is also renamed
                    AstNode* const newp = nodep->cloneTree(false);
                    iterateChildren(newp);
                    compVtxp->activep()->addStmtsp(newp);
                }
            }
        }
        iterateChildren(retimeAlwaysComb);

        AstNode::user1ClearTree();
        AstNode::user2ClearTree();
        // create a clone of the commited variables
        for (AstVarScope* const vscp : commitedp) {
            // for each vscp create an initValue that contains
            // the value set by any initial/static assignments
            vscp->user2p(makeVscp(vscp));
        }
        // substitute the original variables initial blocks
        auto substInit = [this](AstNode* nodep) { iterateChildren(nodep); };
        m_logicClasses.m_static.foreachLogic(substInit);
        m_logicClasses.m_initial.foreachLogic(substInit);

        // now create a "flag" variable

        findSenItemInit();

        AstVar* const initVarp = new AstVar{m_netlistp->fileline(), VVarType::VAR,
                                            m_newNames.get("init"), m_netlistp->findUInt32DType()};

        AstVarScope* const initVscp
            = new AstVarScope{m_netlistp->fileline(), m_netlistp->topScopep()->scopep(), initVarp};
        initVscp->scopep()->addVarsp(initVscp);
        initVscp->scopep()->modp()->addStmtsp(initVarp);
        AstActive* const initActivep = new AstActive{m_netlistp->fileline(), "", m_initSenTreep};
        m_netlistp->topScopep()->scopep()->addBlocksp(initActivep);
        AstInitial* const initBlockp = new AstInitial{m_netlistp->fileline(), nullptr};
        initActivep->addStmtsp(initBlockp);
        initBlockp->addStmtsp(
            new AstAssign{m_netlistp->fileline(),
                          new AstVarRef{m_netlistp->fileline(), initVscp, VAccess::WRITE},
                          new AstConst{m_netlistp->fileline(), 1U}});
        AstActive* const commitActivep = new AstActive{m_netlistp->fileline(), "", seqSenTreep};
        m_netlistp->topScopep()->scopep()->addBlocksp(commitActivep);
        AstAlways* const commitAlwaysp
            = new AstAlways{m_netlistp->fileline(), VAlwaysKwd::ALWAYS_FF, nullptr, nullptr};
        commitActivep->addStmtsp(commitAlwaysp);

        // create a new variable for each variable that was commited in the original
        // sequential block:
        // for each vscp commit create newVscp
        // then create:
        // always_ff
        //      if (initVscp)
        //         newVscp = initValue
        //         initVscp = 0;
        //      else
        //         newVscp = vscp;
        //
        AstIf* const ifp = new AstIf{
            m_netlistp->fileline(), new AstVarRef{m_netlistp->fileline(), initVscp, VAccess::READ},
            nullptr, nullptr};

        commitAlwaysp->addStmtsp(
            new AstAssign{m_netlistp->fileline(),
                          new AstVarRef{m_netlistp->fileline(), initVscp, VAccess::WRITE},
                          new AstConst{m_netlistp->fileline(), 0U}});

        for (AstVarScope* const vscp : commitedp) {
            AstVarScope* const initValue = VN_AS(vscp->user2p(), VarScope);
            AstVarScope* const newVscp = makeVscp(vscp);
            ifp->addThensp(
                new AstAssign{m_netlistp->fileline(),
                              new AstVarRef{m_netlistp->fileline(), vscp, VAccess::WRITE},
                              new AstVarRef{m_netlistp->fileline(), initValue, VAccess::READ}});

            ifp->addElsesp(
                new AstAssign{m_netlistp->fileline(),
                              new AstVarRef{m_netlistp->fileline(), vscp, VAccess::WRITE},
                              new AstVarRef{m_netlistp->fileline(), newVscp, VAccess::READ}});
            commitAlwaysp->addStmtsp(
                new AstAssign{m_netlistp->fileline(),
                              new AstVarRef{m_netlistp->fileline(), newVscp, VAccess::WRITE},
                              new AstVarRef{m_netlistp->fileline(), vscp, VAccess::READ}});
        }
        ifp->addElsesp(retimeAlwaysComb->stmtsp()->unlinkFrBackWithNext());
        retimeAlwaysComb->addStmtsp(ifp);

        depGraphp = nullptr;
        graphp = nullptr;
        // We have all the sampling logic, now we need to turn the sequential logic into
        // combinational and rename all the "cut" variables in the transformed logic.
        // In transforming sequential logic, we create one always_comb that subsumes all
        // the AssignPre and AssignPost/AlwaysPost logic as well.
        // morphLogic(std::move(depGraphp), true);
    }

    void visit(AstNodeVarRef* vrefp) {
        // rename variables if there is an entry in m_replacements
        AstVarScope* const oldVscp = vrefp->varScopep();
        AstVarScope* const newVscp = VN_CAST(oldVscp->user2p(), VarScope);
        if (!newVscp) {
            return;  // nothing to do
        }
        vrefp->name(newVscp->varp()->name());
        vrefp->varp(newVscp->varp());
        vrefp->varScopep(newVscp);
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit RetimerVisitor(AstNetlist* netlistp,
                            std::tuple<V3Sched::LogicClasses, V3Sched::LogicRegions,
                                       std::vector<std::unique_ptr<DepGraph>>>&& deps)
        : m_newNames("__Vretime")
        , m_netlistp{netlistp} {
        // build a data dependence grpah

        auto& depGraphsp = std::get<2>(deps);
        auto& regions = std::get<1>(deps);
        m_logicClasses = std::move(std::get<0>(deps));

        if (!regions.m_act.empty()) {
            regions.m_act.front().second->v3warn(UNOPTFLAT, "active regions prevents retiming");
            return;
        }
        UASSERT(regions.m_act.empty(), "retiming can not be done when there is an active region");
        // use the data dependence graph to build a netlist grpah, a netlist graph is
        // a per partition graph with a single sink node that represents a combinational
        // pass followed by sequential logic as sink. The sequential logic may be a collection
        // of registers.

        std::vector<std::unique_ptr<NetlistGraph>> netGraphsp = buildNetlistGraphs(depGraphsp);
        // initialize cost values for each netlist
        for (auto& netp : netGraphsp) { initializeCostValues(netp); }

        // sort in decearsing order of computation time.
        std::unordered_map<NetlistGraph*, int> netIndex;
        for (int i = 0; i < netGraphsp.size(); i++) {
            // keep a mapping from the NetlistGraph to their original dependence graph
            netIndex.emplace(netGraphsp[i].get(), i);
        }
        std::stable_sort(
            netGraphsp.begin(), netGraphsp.end(),
            [](const auto& gp1, const auto& gp2) { return gp1->cost() > gp2->cost(); });
        // starting from the slowest, try to retime each graph. If a retiming solution
        // is found for one graph, we disable retiming for any other graph affected by it.
        // This is because a retiming will invalidate the cost values and ranks computed
        // earlier. This is rather restrictive, but we are not looking to do too much retiming
        // anyway. If we can reduce the execution time just slightly, that would be more than
        // enough.
        for (const auto& gp : netGraphsp) { markRetiming(gp.get()); }

        // iterate through all the partitions and create new sequential logic if
        // retiming is beneficial based on the resulst of tryRetime
        for (int ix = 0; ix < netGraphsp.size(); ix++) {
            auto& graphp = netGraphsp[ix];
            auto& depp = depGraphsp[netIndex[graphp.get()]];
            applyRetiming(std::move(graphp), std::move(depp));
        }
        netGraphsp.clear();
        depGraphsp.clear();
    }
};

// namespace {
// struct HeapKey {
//     NetlistGraph* m_graphp;
//     void increase(uint32_t c) {
// #if VL_DEBUG
//         // UASSERT(c >= m_graphp->cost());
// #endif
//         m_graphp->cost(c);
//     }
//     bool operator<(const HeapKey& other) const {
//         return m_graphp->cost() < other.m_graphp->cost();
//     }
// };
// using MaxHeap = PairingHeap<HeapKey>;
// using HeapNode = MaxHeap::Node;
// }  // namespace

class IsRetimingAllowedVisitor final : VNVisitor {
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
    explicit IsRetimingAllowedVisitor(AstNetlist* netlistp) {
        m_allowed = true;
        iterate(netlistp);
    }

public:
    // returns true if all actives fall under the same scope
    static bool allowed(AstNetlist* netlistp) {
        return IsRetimingAllowedVisitor{netlistp}.m_allowed;
    }
};
void retimeAll(AstNetlist* netlistp) {

    if (IsRetimingAllowedVisitor::allowed(netlistp)) {
        auto deps = V3BspSched::buildDepGraphs(netlistp);
        { RetimerVisitor{netlistp, std::move(deps)}; }
        v3Global.dumpCheckGlobalTree("retimed", 0, dumpTree() >= 3);
        // clean the tree
        V3Dead::deadifyAllScoped(netlistp);
    } else {
        netlistp->v3warn(UNOPTFLAT, "skipping retiming since the design is not flattened");
    }
}

};  // namespace Retiming
};  // namespace V3BspSched