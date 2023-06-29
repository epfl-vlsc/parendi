// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Try to split more variables (automatically)
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

#include "V3SplitVarExtra.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3Const.h"
#include "V3Graph.h"
#include "V3Sched.h"
#include "V3SchedAcyclic.h"
#include "V3SplitVar.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include "queue"

#include <memory>
#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

// Visitor to find potential combinational loops, these will be used to find
// candidate variables to split, this is VERY similar to V3SchedAcylic.
namespace {
using Graph = V3Sched::V3SchedAcyclic::Graph;
using LogicVertex = V3Sched::V3SchedAcyclic::LogicVertex;
using VarVertex = V3Sched::V3SchedAcyclic::VarVertex;
using Candidate = V3Sched::V3SchedAcyclic::Candidate;

class SplitVariableCombLoopsVisitor : public VNVisitor {
private:
    // STATE, avoid using userN to keep this portable and safely callable
    std::unordered_map<AstVarScope*, VarVertex*> m_varVtxp;  // clear on netlistp
    std::unordered_set<AstVarScope*> m_prodp;  // clear on logicp
    std::unordered_set<AstVarScope*> m_consp;  // clear on logicp

    std::unique_ptr<Graph> m_graphp;
    LogicVertex* m_logicVtxp = nullptr;

    AstScope* m_scopep = nullptr;

    VarVertex* getVarVertex(AstVarScope* const vscp) {
        auto it = m_varVtxp.find(vscp);
        if (it == m_varVtxp.end()) {
            m_varVtxp.emplace(vscp, new VarVertex{m_graphp.get(), vscp});
        }
        return m_varVtxp[vscp];
    }
    void visit(AstVarRef* vrefp) {
        if (!m_logicVtxp) {
            // not in a place that concerns us
            return;
        }
        AstVarScope* const vscp = vrefp->varScopep();
        VarVertex* const varVtxp = getVarVertex(vscp);

        const int weight = (vscp->width() - 1) / VL_EDATASIZE + 1;

        if (vrefp->access().isWriteOrRW() && !m_prodp.count(vscp) /*is it the first prod*/) {
            new V3GraphEdge{m_graphp.get(), m_logicVtxp, varVtxp, weight, true};
            m_prodp.emplace(vscp);
        }

        if (vrefp->access().isReadOrRW() && !m_consp.count(vscp) /* first consume*/
            && !m_prodp.count(vscp) /*no edge to self*/) {
            // note that !m_prodp.count(vscp) is a bit tricky in special cases, e.g.:
            // comb:
            //      if (cond) v = ...
            //      x = v + s
            // since in this case maybe we want to allow a loop to self: var <--> logic
            // but in other cases we would not want it, e.g.,:
            //      if (cond) v = ...
            //      else  v = ...
            //      x = v + s
            new V3GraphEdge{m_graphp.get(), varVtxp, m_logicVtxp, weight, true};
            m_consp.emplace(vscp);
        }
    }
    void iterateLogic(AstNode* logicp) {
        UASSERT_OBJ(VN_IS(logicp, Always) || VN_IS(logicp, AssignW) || VN_IS(logicp, AssignAlias),
                    logicp, "unexpected comb logic type " << logicp->prettyTypeName() << endl);
        UASSERT_OBJ(m_logicVtxp == nullptr, logicp, "nesting logic?");
        m_logicVtxp = new LogicVertex{m_graphp.get(), logicp, m_scopep};
        m_prodp.clear();
        m_consp.clear();
        iterateChildren(logicp);
        m_logicVtxp = nullptr;
    }

    void visit(AstScope* scopep) override {
        m_scopep = scopep;
        iterateChildrenConst(scopep);
    }
    void visit(AstActive* activep) override {
        // UASSERT(!activep->sensesStorep(), "split variable analysis should be after
        // V3ActiveTop");
        if (!activep->sensesp()->hasCombo()) {
            return;  // not touching none-combinational logic
        }
        UASSERT_OBJ(
            activep->sensesp()->forall([](AstSenItem* const itemp) { return itemp->isCombo(); }),
            activep, "mix logic found!");
        for (AstNode* logicp = activep->stmtsp(); logicp; logicp = logicp->nextp()) {
            UINFO(15, "        iterating " << logicp << endl);
            iterateLogic(logicp);
        }
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    explicit SplitVariableCombLoopsVisitor(AstNetlist* netlistp)
        : m_graphp{new Graph} {
        m_varVtxp.clear();
        iterate(netlistp);
    }

public:
    static std::unique_ptr<Graph> build(AstNetlist* netlistp) {
        // build the graph
        auto impl = SplitVariableCombLoopsVisitor{netlistp};
        if (dumpGraph() >= 4) { impl.m_graphp->dumpDotFilePrefixed("split_extra_dep"); }
        // remove anything that does not contribute to a combinational loop
        V3Sched::V3SchedAcyclic::removeNonCyclic(impl.m_graphp.get());
        if (dumpGraph() >= 4) { impl.m_graphp->dumpDotFilePrefixed("split_extra_dep_loops"); }
        // return the part that contains combinational loops
        return std::move(impl.m_graphp);
    }
};

class BitVector {
private:
    const int m_width;
    std::vector<uint32_t> m_bits;

public:
    static constexpr int BUCKET_SIZE = sizeof(uint32_t) * 8;

    int index(int pos) const { return pos / BUCKET_SIZE; }

    explicit BitVector(const int width)
        : m_width{width} {
        int numWords = index(m_width - 1) + 1;
        m_bits.clear();
        for (int i = 0; i < numWords; i++) { m_bits.push_back(0); }
    }
    bool conflict(const BitVector& other) const {
        UASSERT(other.m_width == m_width, "invalid intersection");
        for (int i = 0; i < m_bits.size(); i++) {
            if (m_bits[i] & other.m_bits[i]) { return true; }
        }
        return false;
    }
    void set(int from, int width) {
        if (width == 0) return;
        const int fromIndex = index(from);
        const int toIndex = index(from + width - 1);

        UASSERT(from + width - 1 < m_width,
                "invalid range [" << from << "+:" << width << "]" << endl);
        const int shAmount = from % BUCKET_SIZE;
        uint32_t mask = 0;
        if (toIndex == fromIndex) {
            mask = (width == BUCKET_SIZE) ? std::numeric_limits<uint32_t>::max()
                                          : ((1u << width) - 1u);
            width = 0;
            from = 0;
        } else {
            UASSERT(toIndex > fromIndex, "invalid index values");
            mask = std::numeric_limits<uint32_t>::max();
            width = width - (BUCKET_SIZE - shAmount);
            UASSERT(width >= 0, "underflow");
            from = (fromIndex + 1) * BUCKET_SIZE;
        }
        mask = mask << shAmount;
        m_bits[fromIndex] = m_bits[fromIndex] | mask;
        // std::cout << *this << std::endl;
        set(from, width);
    }
    uint32_t get(int from) const {
        int fromIndex = index(from);
        int jIndex = from % BUCKET_SIZE;
        return ((m_bits[fromIndex] >> jIndex) & 1u);
    }

    bool empty() const {
        for (auto v : m_bits) {
            if (v) return false;
        }
        return true;
    }

    std::string toString() const {
        std::stringstream ss;
        for (int ix = m_width - 1; ix >= 0; ix--) {
            ss << get(ix);
            if (ix % 8 == 0 && ix != 0) ss << "_";
        }
        return ss.str();
    }
};

using BitInterval = std::pair<int /*lsb*/, int /*msb*/>;
struct ReadWriteVec {
    BitVector m_write;
    BitVector m_read;
    std::vector<FileLine*> m_writeLoc;
    std::vector<FileLine*> m_readLoc;
    std::vector<BitInterval> m_readIntervals;
    const int m_width;
    explicit ReadWriteVec(int width)
        : m_write{width}
        , m_read{width}
        , m_width{width} {
        m_writeLoc.clear();
        m_readLoc.clear();
    }
    inline void setWrite(int from, int width, FileLine* const loc) {
        m_write.set(from, width);
        m_writeLoc.push_back(loc);
    }
    inline void setRead(int from, int width, FileLine* const loc) {
        m_read.set(from, width);
        m_readLoc.push_back(loc);
        m_readIntervals.emplace_back(from, from + width - 1);
    }
    inline bool isRW() const { return !m_write.empty() && !m_read.empty(); }
    inline bool conflict() const { return m_write.conflict(m_read); }
    std::string conflictReason() const {
        std::stringstream ss;
        ss << "\twrite pattern: " << m_write.toString() << endl;
        ss << "\tread pattern: " << m_read.toString() << endl;
        auto streamLoc = [&ss](const auto vec) {
            for (FileLine* const loc : vec) { ss << "\t\t" << loc->ascii() << endl; }
        };
        ss << "\twrite loc: " << endl;
        streamLoc(m_writeLoc);
        ss << "\tread loc: " << endl;
        streamLoc(m_readLoc);
        return ss.str();
    }
    static std::vector<BitInterval> disjoinFillGaps(const std::vector<BitInterval>& original,
                                                    int width) {
        // turn  the range of sorted intervals with gaps, with one that fully covers [0:width - 1]
        int lsb = 0, msb = 0;
        std::vector<BitInterval> no_gaps;
        if (original.front().first > 0) { no_gaps.emplace_back(0, original.front().second - 1); }
        for (int i = 0; i < original.size(); i++) {
            auto& r1 = original[i];
            no_gaps.emplace_back(r1.first, r1.second);
            if (i < original.size() - 1) {
                auto& r2 = original[i + 1];
                if (r1.second + 1 < r2.first) {
                    no_gaps.emplace_back(r1.second + 1, r2.first - 1);
                }
            }
        }
        if (original.back().second < width - 1) {
            no_gaps.emplace_back(original.back().second + 1, width - 1);
        }
        return no_gaps;
    }
    static std::vector<BitInterval> maximalDisjoint(const std::vector<BitInterval>& original) {
        UASSERT(original.size(), "empty original");
        std::deque<BitInterval> toSplit{original.begin(), original.end()};
        std::vector<BitInterval> disjoint;
        auto sortIt = [&](std::deque<BitInterval>& q) {
            std::sort(q.begin(), q.end(), [](const BitInterval& i1, const BitInterval& i2) {
                return i1.first < i2.first;
            });
        };
        sortIt(toSplit);
        // int step = 0;
        int numLeft = original.size();
        // auto print = [&]() {
        //     std::cout << "w" << step << ": \t";
        //     for (auto x : toSplit) { std::cout << toString(x) << "  "; }
        //     std::cout << "\t" << numLeft << std::endl;
        //     std::cout << "r" << step << ": \t";
        //     for (auto x : disjoint) { std::cout << toString(x) << "  "; }
        //     std::cout << std::endl << std::endl;
        //     step++;
        // };
        // sort based on the increasing order of lsb
        while (numLeft > 1) {
            // print();
            // take the first one, which has the lowest lsb
            BitInterval i1 = toSplit.front();
            numLeft--;
            toSplit.pop_front();
            // take the second one
            BitInterval i2 = toSplit.front();
            if (i1.second < i2.first) {
                // i1 is already disjoint
                disjoint.push_back(i1);
                continue;
            }
            // i1 has some intersection with i2
            // 0)
            //  i1:   =====
            //  i2:   ===
            // 1)
            //   i1:  =====
            //   i2:   ====
            // 2)
            //  i1:   ==========
            //  i2:     =============
            // 3)
            //  i1:   ==========
            //  i2:     =====
            UASSERT(i1.first <= i2.first, "not sorted!");
            if (i1.second > i2.second && i1.first == i2.first) {  // 0)
                numLeft++;
                toSplit.push_back({i2.second + 1, i1.second});
            } else if (i1.second <= i2.second && i1.first < i2.first) {
                // 1) and 2)
                numLeft++;
                toSplit.push_back({i1.first, i2.first - 1});
            } else if (i1.second > i2.second && i1.first < i2.first) {
                numLeft += 2;
                toSplit.push_back({i1.first, i2.first - 1});
                toSplit.push_back({i2.second + 1, i1.second});
            } else {
                // i1 is in i2
                //  ======
                //  ======
                // so add nothing (gets eliminated)
            }

            sortIt(toSplit);
        }
        UASSERT(numLeft == 1 && toSplit.size() == 1, "empty queue!");
        disjoint.push_back(toSplit.front());
        return disjoint;
    }
};
}  // namespace
class SplitVariableExtraVisitor : public VNVisitor {
private:
    AstSel* m_selp = nullptr;  // enclosing AstSel node

    // scoreboard keeps track of the bits each candidate reads or writes
    // cleared on SCC
    std::unordered_map<AstVarScope*, ReadWriteVec> m_scoreboard;
    // a map from each Variable scope to the disjoint set of ranges that apear as
    // an RV. We use them to split LVs of the same variable to undo V3Const optimizations
    //  comb1:
    //      v.valid = f(v.data[31:0])
    //  comb2:
    //      v.data[31:0] = g(...)
    //      v.data[63:32] = h(v.valid, ...)
    //  The above example has an induced combinational loop that can be eliminated
    //  by spliting the variable v and splitting comb2 block. However, V3Const
    //  turns comb2 into:
    //  comb2_const:
    //      v.data = {h(v.valid, ...), g(..)};
    //  which prevents V3SplitVar to split v.data into two variables.
    //  By keeping track of the ranges of the v.data as RVs, we can preemptively
    //  trun comb2_const into:
    //  comb2_const_force_split:
    //      v.data[31:0] = ...;
    //      v.data[63:32] = ...;
    //  and allow V3SplitVar to do it's work. Here the idea is to note the RV reference
    //  of v.data[31:0] in comb1 and use that to split any LV that spans the range.
    std::unordered_map<AstVarScope*, std::vector<BitInterval>> m_disjointReadRanges;

    void iterateSCC(const std::vector<V3GraphVertex*>& scc) {

        if (scc.empty()) { return; }
        // the color representing the current SCC we are invistigating
        const uint32_t sccColor = scc.front()->color();
        m_scoreboard.clear();
        for (const auto& vtxp : scc) {
            if (VarVertex* const varVtxp = dynamic_cast<VarVertex* const>(vtxp)) {
                AstNodeDType* const dtypep = varVtxp->varp()->dtypep()->skipRefp();
                UASSERT_OBJ(sccColor == varVtxp->color(), varVtxp->varp(),
                            "scc is not colored properly");
                // only consider Struct and PackArray types for splitting, unpack array
                // is rarely a cause for induced combinational loops
                if (!(VN_IS(dtypep, PackArrayDType) || VN_IS(dtypep, StructDType)
                      || VN_IS(dtypep, BasicDType))) {
                    UINFO(4, "Will not consider " << varVtxp->varp()->prettyNameQ()
                                                  << " for automatic splitting with dtype "
                                                  << dtypep->prettyNameQ() << endl);
                    continue;
                }
                if (!V3SplitVar::canSplitVar(varVtxp->varp())) { continue; }
                // probably can split this var, but we need find the best one to split
                // m_scoreboard{candidate.first->vscp(), ReadWriteVec{dtypep->width()};
                UINFO(8, "        Candidate for automatic splitting: " << varVtxp->vscp() << endl);
                m_scoreboard.emplace(varVtxp->vscp(), ReadWriteVec{dtypep->width()});
            }
        }
        // follow the edges in the SCC ro reach all the LogicVertex types
        for (V3GraphVertex* const vtxp : scc) {
            if (vtxp->color() != sccColor) continue;
            if (LogicVertex* const logicVertexp = dynamic_cast<LogicVertex* const>(vtxp)) {
                iterateChildren(logicVertexp->logicp());
            }
        }
        UINFO(3, "        In SCC" << cvtToHex(scc.front()->color()) << " :" << endl);
        for (const auto& pair : m_scoreboard) {
            if (pair.second.conflict()) {
                const auto disjointReadIntervals
                    = ReadWriteVec::maximalDisjoint(pair.second.m_readIntervals);
                if (disjointReadIntervals.size() > 1) {
                    UINFO(3, "        considering: " << pair.first->prettyName() << endl
                                                     << pair.second.conflictReason() << endl);
                    const auto disjointCovered = ReadWriteVec::disjoinFillGaps(
                        disjointReadIntervals, pair.first->dtypep()->width());
                    m_disjointReadRanges.emplace(pair.first, std::move(disjointCovered));
                } else {
                    UINFO(3, "        can not split: " << pair.first->prettyNameQ() << endl);
                }
            } else {
                UINFO(4, "        need not split:  " << pair.first->prettyName() << endl);
            }
        }
    }

    void visit(AstVarRef* vrefp) override {
        AstVarScope* const vscp = vrefp->varScopep();
        auto setIt = m_scoreboard.find(vscp);
        if (setIt == m_scoreboard.end()) {
            // not interesting to use, either a packed array or not at all
            // contributing to a combinational loop
            return;
        }
        int selLsb = 0;
        int selWidth = vscp->dtypep()->skipRefp()->width();
        if (m_selp) {
            AstConst* const lsbpConst = VN_CAST(m_selp->lsbp(), Const);
            if (!lsbpConst) {
                // can not consider this variable since the sel range is dynamic
                m_scoreboard.erase(setIt);
                UINFO(8,
                      "        dynamic selection prevents split: " << vscp->prettyName() << endl);
                return;
            }
            selLsb = lsbpConst->toSInt();

            // UASSERT_OBJ(selWidth >= m_selp->widthConst() || selLsb == 0, vrefp,
            //             "Unexpected width, can not determine selection. Is there a AstSel -> "
            //             "Concat -> VarRef?"
            //                 << endl);

            selWidth = std::min(selWidth, m_selp->widthConst());
        }
        if (vrefp->access().isWriteOrRW()) {
            setIt->second.setWrite(selLsb, selWidth, vrefp->fileline());
        }
        if (vrefp->access().isReadOrRW()) {
            setIt->second.setRead(selLsb, selWidth, vrefp->fileline());
        }
    }

    void visit(AstSel* selp) override {
        // UASSERT_OBJ(!m_selp, selp, "nested Sel! " << m_selp << endl);
        VL_RESTORER(m_selp);
        {
            if (VN_IS(selp->fromp(), Extend) || VN_IS(selp->fromp(), ExtendS)
                || VN_IS(selp->fromp(), VarRef)) {
                // perhaps a bit too conservative, but at least this way we
                // can know the ranges are computed correclty in AstVarRef
                m_selp = selp;
                iterateChildren(selp);
            }
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

    using ConnectedComponents = std::vector<V3GraphVertex*>;
    using SCCSet = std::unordered_map<int, ConnectedComponents>;

    SCCSet gatherSCCs(const std::unique_ptr<Graph>& graphp) {
        // color the strongly connected componenets
        graphp->stronglyConnected(V3GraphEdge::followAlwaysTrue);

        // gather each component in a map
        SCCSet sccs;
        for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            if (!vtxp->color()) {
                continue;  // not part of an SCC, i.e., does not contribute to a comb loop
            }
            auto it = sccs.find(vtxp->color());
            if (it == sccs.end()) {
                sccs.emplace(vtxp->color(), std::vector<V3GraphVertex*>{vtxp});
            } else {
                it->second.push_back(vtxp);
            }
        }
        return sccs;
    }

    explicit SplitVariableExtraVisitor(AstNetlist* netlistp) {
        // build a graph of the combinational loops
        std::unique_ptr<Graph> graphp = SplitVariableCombLoopsVisitor::build(netlistp);
        if (graphp->empty()) {
            // lucky us, no combinational loops
            UINFO(3, "        No combinational loops, skipping extra splitting" << endl);
            return;
        }
        // color the strongly connected components and within each loop, and
        // cut some edges to make the graph acyclic. We really do not care about
        // which edge is cut, but mostly that we have a set of SCCs
        auto sccs = gatherSCCs(graphp);
        for (const auto& scc : sccs) { iterateSCC(scc.second); }
    }

public:
    static std::unordered_map<AstVarScope*, std::vector<BitInterval>>
    computeDisjoinReadRanges(AstNetlist* netlistp) {
        SplitVariableExtraVisitor vis{netlistp};
        return std::move(vis.m_disjointReadRanges);
    }
};
class SplitExtraPackVisitor : public VNVisitor {
private:
    using ReplacementHandle = std::vector<std::pair<BitInterval, AstVarScope*>>;
    std::unordered_map<AstVarScope*, ReplacementHandle> m_substp;
    AstSel* m_selp = nullptr;

    void mkReplacements(
        const std::unordered_map<AstVarScope*, std::vector<BitInterval>>& splitIntervals,
        AstNetlist* netlistp) {
        VDouble0 numSplits{0};
        for (const auto& pair : splitIntervals) {

            // each read interval gets its owen variable, note that we expect
            // the readIntervals fully cover the width the packed variable, this
            // is done by SplitVarialbeExtraVisitor.

            AstVarScope* const oldVscp = pair.first;
            AstVar* const varp = oldVscp->varp();
            auto& bitIntervals = pair.second;

            std::vector<std::pair<BitInterval, AstVarScope*>> newSubstp;
            UASSERT_OBJ(bitIntervals.size() >= 2, oldVscp,
                        "invalid replacement handle! Need at least 2 parts to be able to split");
            for (const BitInterval& bi : bitIntervals) {
                numSplits++;
                // pretty much a copy/paste from V3SplitVar::SplitPackedVarVisitor::createVars
                int left = bi.second;
                int right = bi.first;
                AstBasicDType* basicp = oldVscp->dtypep()->basicp();
                if (basicp->littleEndian()) std::swap(left, right);
                const std::string name
                    = (left == right)
                          ? varp->name() + "__BRA__" + AstNode::encodeNumber(left) + "__KET__"
                          : varp->name() + "__BRA__" + AstNode::encodeNumber(left)
                                + AstNode::encodeName(":") + AstNode::encodeNumber(right)
                                + "__KET__";
                AstBasicDType* newDTypep;
                int newBitWidth = bi.second - bi.first + 1;
                switch (basicp->keyword()) {
                case VBasicDTypeKwd::BIT:
                    newDTypep = new AstBasicDType{varp->subDTypep()->fileline(), VFlagBitPacked{},
                                                  newBitWidth};
                    break;
                case VBasicDTypeKwd::LOGIC:
                    newDTypep = new AstBasicDType{varp->subDTypep()->fileline(),
                                                  VFlagLogicPacked{}, newBitWidth};
                    break;
                default: UASSERT_OBJ(false, basicp, "Only bit and logic are allowed");
                }
                newDTypep->rangep(new AstRange{
                    varp->fileline(), VNumRange{bi.second, bi.first, basicp->littleEndian()}});
                netlistp->typeTablep()->addTypesp(newDTypep);
                AstVar* newVarp = new AstVar{varp->fileline(), VVarType::VAR, name, newDTypep};
                newVarp->propagateAttrFrom(varp);
                newVarp->funcLocal(varp->isFuncLocal() || varp->isFuncReturn());
                varp->addNextHere(newVarp);
                UINFO(8, "Added " << newVarp->prettyNameQ() << " for " << varp->prettyNameQ()
                                  << endl);
                AstVarScope* newVscp
                    = new AstVarScope{varp->fileline(), oldVscp->scopep(), newVarp};
                oldVscp->addNextHere(newVscp);
                newSubstp.emplace_back(bi, newVscp);
            }
            m_substp.emplace(oldVscp, std::move(newSubstp));
            pushDeletep(oldVscp->unlinkFrBack());
            pushDeletep(varp->unlinkFrBack());
        }

        V3Stats::addStat("Optimizations, extra split var", numSplits - 1);
    }

    void visit(AstVarRef* vrefp) override {
        AstVarScope* const oldVscp = vrefp->varScopep();
        const auto it = m_substp.find(oldVscp);
        if (it == m_substp.end()) {
            return;  // variable reference to unsplit
        }
        // AstConcat* const concatp = new AstConcat { vrefp->fileline(), }
        UASSERT_OBJ(it->second.size() >= 2, oldVscp, "improperly split variable");
        AstConcat* concatp = new AstConcat{
            vrefp->fileline(),
            new AstVarRef{vrefp->fileline(), it->second[1].second, vrefp->access()},
            new AstVarRef{vrefp->fileline(), it->second[0].second, vrefp->access()}};
        for (int i = 2; i < it->second.size(); i++) {
            concatp = new AstConcat{
                vrefp->fileline(),
                new AstVarRef{vrefp->fileline(), it->second[i].second, vrefp->access()}, concatp};
        }
        vrefp->replaceWith(concatp);
        VL_DO_DANGLING(pushDeletep(vrefp), vrefp);
    }

    // void visit(AstSel* selp) override {
    //     VL_RESTORER(m_selp);
    //     {
    //         m_selp = selp;

    //     }
    // }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit SplitExtraPackVisitor(
        AstNetlist* netlistp,
        const std::unordered_map<AstVarScope*, std::vector<BitInterval>>& splitIntervals) {
        if (!splitIntervals.empty()) {
            mkReplacements(splitIntervals, netlistp);
            iterate(netlistp);
        }
    }
};
class SplitInsertSelOnLVVisitor : public VNVisitor {
private:
    std::unordered_map<const AstVarScope*, std::vector<BitInterval>> m_readIntervals;
    std::vector<BitInterval> m_bitSels;  // clear on NodeAssign

    AstSel* m_selp = nullptr;
    AstVarScope* m_vscp = nullptr;
    AstScope* m_scopep = nullptr;

    V3UniqueNames m_tempNames;

    void visit(AstVarRef* vrefp) override {

        if (!vrefp->access().isWriteOrRW()) {
            return;  // nothing to do
        }
        AstVarScope* const vscp = vrefp->varScopep();
        auto it = m_readIntervals.find(vscp);
        if (it == m_readIntervals.end()) {
            return;  // not needed to split write
        }
        UASSERT(vscp->dtypep()->width() > 1,
                "Can not split single bit, check this earlier in V3SplitVarExtra");
        BitInterval bitInt{0, vscp->dtypep()->width() - 1};
        if (m_selp) {
            bitInt.first = m_selp->lsbConst();  // should be Const, if not something else is broken
            bitInt.second = bitInt.first + m_selp->widthConst() - 1;
        }

        m_vscp = vscp;
        std::vector<BitInterval>& allowedIntervals = it->second;
        // bitInt should either be fully contained with one of the allowed intervals.
        // If there is and overlaps between multiple we need to split this LV access e.g.:
        //      allowed intervals: [0:1] [2:3] [4:6] [7:12] [13:31]
        //      bitInt: [2:8], i.e., v[8:2] = ...
        //      should become
        //      v[3:2] = ...
        //      v[6:4] = ...
        //      v[8:7] = ...
        // note that allowedIntervals are sorted in the increasing order of their lsb
        UASSERT(m_bitSels.empty(), "expected empty container, clear on NodeAssign");
        for (const auto& ri : allowedIntervals) {
            if (ri.first > bitInt.second) {
                break;  // no need to search further
            }
            int lsb = std::max(ri.first, bitInt.first);
            int msb = std::min(ri.second, bitInt.second);
            if (lsb <= msb) { m_bitSels.emplace_back(lsb, msb); }
        }

        if (m_bitSels.size() == 1 && m_bitSels.front().first == bitInt.first
            && m_bitSels.front().second == bitInt.second) {
            // fully contained, do nothing
            m_bitSels.pop_back();  // clear it
            return;
        }
    }
    void visit(AstScope* scopep) override {
        UASSERT_OBJ(!m_scopep, scopep, "Nested scopes!");
        VL_RESTORER(m_scopep);
        {
            m_scopep = scopep;
            iterateChildren(scopep);
        }
    }
    void visit(AstSel* selp) override {
        // UASSERT_OBJ(!m_selp, selp, "Nested AstSel");
        VL_RESTORER(m_selp);
        {
            m_selp = selp;
            iterateChildren(selp);
        }
    }
    void visit(AstNodeAssign* assignp) override {

        m_vscp = nullptr;
        m_bitSels.clear();

        // only consider lhs (LV), rhs remains as-is
        iterate(assignp->lhsp());

        if (m_bitSels.empty()) { return; }

        UASSERT_OBJ(VN_IS(assignp->lhsp(), Sel) || VN_IS(assignp->lhsp(), VarRef), assignp,
                    "unexpected node under NodeAssign: " << assignp->lhsp()->prettyTypeName());
        UASSERT_OBJ(VN_IS(assignp, AssignW) || VN_IS(assignp, Assign), assignp,
                    "unexpected NodeAssign type in comb logic " << assignp->prettyTypeName());
        AstVar* const tempVarp = new AstVar{assignp->fileline(), VVarType::MODULETEMP,
                                            m_tempNames.get(assignp), assignp->rhsp()->dtypep()};

        AstVarScope* const tempVscp = new AstVarScope{assignp->fileline(), m_scopep, tempVarp};
        m_scopep->addVarsp(tempVscp);
        m_scopep->modp()->addStmtsp(tempVarp);
        auto newAssign = [&assignp](AstNodeExpr* lhsp, AstNodeExpr* rhsp) -> AstNodeAssign* {
            AstNode* timingp = assignp->timingControlp()
                                   ? assignp->timingControlp()->cloneTree(false)
                                   : nullptr;
            if (VN_IS(assignp, AssignW)) {
                return new AstAssignW{assignp->fileline(), lhsp, rhsp, timingp};
            } else {
                return new AstAssign{assignp->fileline(), lhsp, rhsp, timingp};
            }
        };
        AstNodeAssign* const tmpAssign
            = newAssign(new AstVarRef{assignp->fileline(), tempVscp, VAccess::WRITE},
                        assignp->rhsp()->cloneTree(false));

        // replace this node with a series of assignments
        // v[x:y] = expr;
        // becomes:
        // temp = expr;
        // v[x1:y1] = temp[x1:y1];
        // v[x2:x2] = temp[x2:y2];
        // ...
        for (const BitInterval& bi : m_bitSels) {
            // V3Const::constifyEdit()
            auto newSel = [&](AstVarScope* vscp, VAccess access) {
                return new AstSel{assignp->lhsp()->fileline(),
                                  new AstVarRef{assignp->lhsp()->fileline(), vscp, VAccess::WRITE},
                                  bi.first, bi.second + 1 - bi.first};
            };
            auto newp = newAssign(newSel(m_vscp, VAccess::WRITE), newSel(tempVscp, VAccess::READ));
            tmpAssign->addNext(newp);
        }
        assignp->replaceWith(tmpAssign);
        VL_DO_DANGLING(pushDeletep(assignp), assignp);
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit SplitInsertSelOnLVVisitor(
        AstNetlist* netlistp,
        const std::unordered_map<const AstVarScope*, std::vector<BitInterval>>& intervals)
        : m_readIntervals{intervals}
        , m_tempNames{"__Vsplitsel"} {
        if (!m_readIntervals.empty()) { iterate(netlistp); }
    }
};
class SplitInsertVarScopeVisitor : public VNVisitor {
private:
    AstScope* m_scopep = nullptr;
    enum class VisitMode : uint8_t {
        FIND_EXISTING = 0,
        SET_NONEXISTING = 1
    } m_visitMode = VisitMode::FIND_EXISTING;

    const VNUser1InUse m_user1InUse;
    // STATE
    // AstVar::user1p()  -> varscope

    void visit(AstVarRef* vrefp) override {
        if (!vrefp->varp()->user1p()) {
            // create a new var scope
            AstVarScope* const newVscp
                = new AstVarScope{vrefp->varp()->fileline(), m_scopep, vrefp->varp()};
            m_scopep->addVarsp(newVscp);
            vrefp->varp()->user1p(newVscp);
        }
        if (!vrefp->varScopep()) { vrefp->varScopep(VN_AS(vrefp->varp()->user1p(), VarScope)); }
    }
    void visit(AstScope* scopep) override {
        VL_RESTORER(m_scopep);
        {
            m_scopep = scopep;
            AstNode::user1ClearTree();
            for (AstVarScope* vscp = scopep->varsp(); vscp;
                 vscp = VN_AS(vscp->nextp(), VarScope)) {
                vscp->varp()->user1p(vscp);
            }
            iterateAndNextNull(scopep->blocksp());
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    SplitInsertVarScopeVisitor(AstNetlist* nodep) { iterate(nodep); }
};

void V3SplitVarExtra::splitVariableExtra(AstNetlist* netlistp) {
    UINFO(4, __FUNCTION__ << ":" << endl);
    // find extra candidates to split
    // clear existing attributes
    // netlistp->foreach([](AstVar* varp) { varp->attrSplitVar(false); });
    // mark new ones, based on a heuristic
    auto readRanges = SplitVariableExtraVisitor::computeDisjoinReadRanges(netlistp);
    V3Global::dumpCheckGlobalTree("split_var_extra", 0, dumpTree() >= 5);
    { SplitExtraPackVisitor{netlistp, readRanges}; }
    V3Global::dumpCheckGlobalTree("split_var_extra_pack", 0, dumpTree() >= 3);
    V3Const::constifyAll(netlistp);
}