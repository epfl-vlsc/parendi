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
#include "V3DfgOptimizer.h"
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

using BitInterval = std::pair<int /*lsb*/, int /*msb*/>;

class IntervalSet {
private:
    const int m_width;
    std::list<BitInterval> m_ordered;

    inline static void merge(std::list<BitInterval>& sorted) {
        std::list<BitInterval> res;
        res.push_back(sorted.front());
        sorted.pop_front();
        for (auto it = sorted.cbegin(); it != sorted.cend(); it++) {
            BitInterval& last = res.back();
            if (last.second < it->first) {
                // no need to merge
                res.push_back(*it);
            } else {
                // last.second > it->first --- overlapping
                auto msb = std::max(last.second, it->second);
                UASSERT(it->first >= last.first, "not sorted");
                last.second = msb;
            }
        }
        sorted = std::move(res);
    }

public:
    explicit IntervalSet(const int width)
        : m_width(width) {}

    inline int width() const { return m_width; }
    void insert(const BitInterval& interval) {

        const int lsb = interval.first;
        const int msb = interval.second;
        UASSERT(lsb <= msb, "invalid range");
        // invariant: keep m_ordered sorted
        // we have m_ordered[i].second < m_ordered[i + 1].first
        int pos = 0;
        std::list<BitInterval> high;
        for (auto it = m_ordered.cbegin(); it != m_ordered.cend(); it++) {
            if (it->first > lsb) {
                high.splice(high.cbegin(), m_ordered, it, m_ordered.cend());
                break;
            }
        }

        m_ordered.push_back(interval);
        m_ordered.splice(m_ordered.cend(), high);
        // now merge intervals
        merge(m_ordered);
    }

    IntervalSet intersect(const BitInterval& other) const {
        IntervalSet r{width()};
        for (const auto here : m_ordered) {
            if (other.second < here.first) { break; }
            // other.second >= here.first
            //   ======== other
            //====== here
            BitInterval intsct{std::max(other.first, here.first),
                               std::min(other.second, here.second)};
            r.m_ordered.push_back(intsct);
        }
        return r;
    }

    bool conflict(const IntervalSet& other) const {
        bool r = false;
        for (const auto i1 : other.m_ordered) {
            for (const auto i2 : this->m_ordered) {
                if (i1.second >= i2.first && i1.first <= i2.second) { r = true; }
            }
        }
        return r;
    }

    uint32_t get(int index) const {
        uint32_t res = 0;
        for (const auto interval : m_ordered) {
            if (interval.first >= index && interval.second <= index) {
                res = 1;
                break;
            }
        }
        return res;
    }

    bool empty() const { return m_ordered.empty(); }

    std::vector<BitInterval> intervals() const {
        return std::vector<BitInterval>{m_ordered.cbegin(), m_ordered.cend()};
    }

    friend std::ostream& operator<<(std::ostream& os, const IntervalSet& intervals);
};

std::ostream& operator<<(std::ostream& os, const IntervalSet& intervals) {
    os << "{ ";
    for (const auto i : intervals.m_ordered) { os << "[" << i.first << ":" << i.second << "] "; }
    os << "}";
    return os;
}

struct ReadWriteVec {
    // BitVector m_write;
    // BitVector m_read;
    IntervalSet m_write, m_read;
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
        m_write.insert({from, from + width - 1});
        m_writeLoc.push_back(loc);
    }
    inline void setRead(int from, int width, FileLine* const loc) {
        m_read.insert({from, from + width - 1});
        m_readLoc.push_back(loc);
        // m_readIntervals.emplace_back(from, from + width - 1);
    }
    inline bool isRW() const { return !m_write.empty() && !m_read.empty(); }
    inline bool conflict() const { return m_write.conflict(m_read); }
    std::string conflictReason() const {
        std::stringstream ss;
        ss << "\twrite pattern: " << m_write << endl;
        ss << "\tread pattern: " << m_read << endl;
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
        if (original.front().first > 0) { no_gaps.emplace_back(0, original.front().first - 1); }
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
                if (!V3SplitVar::canSplitVar(varVtxp->varp())) {
                    UINFO(4, "Can not consider " << varVtxp->varp()->prettyNameQ()
                                                 << " for automatic splitting with dtype "
                                                 << dtypep->prettyNameQ() << endl);
                    continue;
                }
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
        UINFO(4, "        In SCC" << cvtToHex(scc.front()->color()) << " :" << endl);
        for (const auto& pair : m_scoreboard) {
            if (pair.second.conflict()) {
                const auto disjointReadIntervals
                    = ReadWriteVec::maximalDisjoint(pair.second.m_read.intervals());

                const auto disjointCovered = ReadWriteVec::disjoinFillGaps(
                    disjointReadIntervals, pair.first->dtypep()->width());
                auto splitStr = [&disjointCovered]() {
                    std::stringstream ss;
                    ss << "        ";
                    for (const auto& bi : vlstd::reverse_view(disjointCovered)) {
                        ss << "[" << bi.second << ":" << bi.first << "],  ";
                    }
                    ss << endl;
                    return ss.str();
                };
                if (disjointCovered.size() > 1) {
                    if (debug() >= 4) {
                        std::stringstream ss;
                        ss << "        ";
                        for (const auto& bi : vlstd::reverse_view(disjointCovered)) {
                            ss << "[" << bi.second << ":" << bi.first << "],  ";
                        }
                        ss << endl;

                        UINFO(4, "        considering: " << pair.first->prettyName() << endl
                                                         << splitStr() << endl);
                    }
                    m_disjointReadRanges.emplace(pair.first, std::move(disjointCovered));
                } else {
                    UINFO(4, "        can not split: " << pair.first->prettyNameQ() << endl
                                                       << splitStr() << endl);
                }
            } else {
                UINFO(5, "        need not split:  " << pair.first->prettyName()
                                                     << pair.second.conflictReason() << endl);
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
        UASSERT_OBJ(!m_selp, selp, "nested Sel! " << m_selp << endl);
        // SEL(EXTEND(VARREF)) or SEL(VARREF) will recieve a narrowed down
        // range but anything else should be read/written as a whole. Note that
        // as long as we do the following we ensure that we never wrongly compute
        // the read/write range on VarRef but we may miss some optimization opportunities.
        // E.g., SEL(CONCAT(VARREF, VARREF)), we could still determine extactly which
        // bits are being read in each VarRef but instead we end up thinking all bits
        // are being written/read
        AstExtend* const extp = VN_CAST(selp->fromp(), Extend);
        AstExtendS* const extsp = VN_CAST(selp->fromp(), ExtendS);
        AstVarRef* const vrefp = VN_CAST(selp->fromp(), VarRef);
        if (vrefp || (extp && VN_IS(extp->lhsp(), VarRef))
            || (extsp && VN_IS(extsp->lhsp(), VarRef))) {
            m_selp = selp;
            iterate(selp->fromp());
            // do not visit the rest with m_selp set since the sel range only applies to the
            // variable referenced directly below but not the ones in lsbp
            m_selp = nullptr;
        } else {
            // SEL(EXPR(VARREF)) will view VarRef as it was read/written as a whole.
            // This is conservative, but correct.
            iterate(selp->fromp());
        }
        iterate(selp->lsbp());
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

class SplitExtraWideVisitor : public VNVisitor {
private:
    std::unordered_map<AstVarScope*, std::vector<BitInterval>> m_readIntervals;
    std::unordered_set<AstVarScope*> m_unopt;
    bool isSplittable(AstVar* const varp) const {
        AstNodeDType* const dtypep = varp->dtypep();
        if (!dtypep->isWide()) { return false; }
        if (!(VN_IS(dtypep, PackArrayDType) || VN_IS(dtypep, StructDType)
              || VN_IS(dtypep, BasicDType))) {
            return false;
        }
        if (!V3SplitVar::canSplitVar(varp)) { return false; }
        return true;
    }

    void visit(AstNodeVarRef* vrefp) override {
        if (vrefp->access().isReadOrRW()) {
            // read as a whole, do not split?
            // m_unopt.insert(vrefp->varScopep());
        }
    }
    void visit(AstSel* selp) override {

        // iterate lsbp, but not from. Lsbp may contain another Sel internally that
        // wraps around some VarRef, that has nothing to do with the range selection here
        iterate(selp->lsbp());

        // not try to determine the range selection on the fromp
        if (!VN_IS(selp->lsbp(), Const)) {
            // cannot determine the selection statically
            return;
        }

        // SEL(EXTEND(VARREF)) or SEL(VARREF) will recieve a narrowed down
        // range but anything else should be read/written as a whole. Note that
        // as long as we do the following we ensure that we never wrongly compute
        // the read/write range on VarRef but we may miss some optimization opportunities.
        // E.g., SEL(CONCAT(VARREF, VARREF)), we could still determine extactly which
        // bits are being read in each VarRef but instead we end up thinking all bits
        // are being written/read
        std::function<AstNodeVarRef*(AstNode*)> findBase
            = [&findBase](AstNode* nodep) -> AstNodeVarRef* {
            if (VN_IS(nodep, NodeVarRef)) {
                return VN_AS(nodep, NodeVarRef);
            } else if (auto extp = VN_CAST(nodep, Extend)) {
                return findBase(extp->lhsp());
            } else if (auto extsp = VN_CAST(nodep, ExtendS)) {
                return findBase(extp->lhsp());
            } else {
                return nullptr;
            }
        };
        AstNodeVarRef* const fromp = findBase(selp->fromp());
        if (!fromp) {
            // could not find the fromp VarRef. This is some pattern that we
            // could not optimize
            UINFO(3, "Could not determine fromp VarRef " << selp << ", will not try to optimize..."
                                                         << endl);
            // willl see any VarRef under here as full range selection
            iterate(selp->fromp());
            return;
        }
        const bool canSplit = isSplittable(fromp->varp());
        const int lsb = selp->lsbConst();
        const int width = selp->widthConst();
        if (canSplit && fromp->access().isReadOrRW() && width < fromp->varp()->width()) {
            UINFO(10, "Wide selection " << selp << " of variable " << fromp->varp()->prettyNameQ()
                                        << endl);
            m_readIntervals[fromp->varScopep()].emplace_back(lsb, lsb + width - 1);
        }
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

private:
    explicit SplitExtraWideVisitor(AstNetlist* netlistp) { iterate(netlistp); }

public:
    static std::unordered_map<AstVarScope*, std::vector<BitInterval>>
    findExtraSplittable(AstNetlist* netlistp) {
        const SplitExtraWideVisitor impl{netlistp};
        std::unordered_map<AstVarScope*, std::vector<BitInterval>> reads;
        for (const auto& it : impl.m_readIntervals) {

            if (impl.m_unopt.count(it.first)) continue;

            const auto disjoint = ReadWriteVec::maximalDisjoint(it.second);
            const auto filled = ReadWriteVec::disjoinFillGaps(disjoint, it.first->width());
            UINFO(8, "Variable" << it.first->prettyNameQ() << " has " << disjoint.size()
                                << " disjoint reads" << endl);
            if (filled.size() > 1) {
                UINFO(4, "Will split " << it.first->prettyNameQ() << " into " << filled.size()
                                       << " parts" << endl);
                reads.emplace(it.first, filled);
            }
        }
        return reads;
    }
};
class SplitExtraPackVisitor : public VNVisitor {
private:
    using ReplacementHandle = std::vector<std::pair<BitInterval, AstVarScope*>>;
    std::unordered_map<AstVarScope*, ReplacementHandle> m_substp;

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
            UINFO(5, "    Splitting " << oldVscp->prettyNameQ() << " to " << bitIntervals.size()
                                      << " variables " << endl);
        }

        V3Stats::addStat("Optimizations, extra split var", numSplits - 1);
    }

    bool intersects(const BitInterval& p1, const BitInterval& p2) {
        const int lsb1 = p1.first, msb1 = p1.second;
        const int lsb2 = p2.first, msb2 = p2.second;
        if (msb1 < lsb2 || lsb1 > msb2) {
            return false;
        } else {
            return true;
        }
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

void V3SplitVarExtra::splitVariableExtra(AstNetlist* netlistp) {
    UINFO(4, __FUNCTION__ << ":" << endl);
    // find extra candidates to split
    // clear existing attributes
    // netlistp->foreach([](AstVar* varp) { varp->attrSplitVar(false); });
    // mark new ones, based on a heuristic
    auto readRanges = SplitVariableExtraVisitor::computeDisjoinReadRanges(netlistp);
    V3Global::dumpCheckGlobalTree("split_var_extra_pre", 0, dumpTree() >= 5);

    { SplitExtraPackVisitor{netlistp, readRanges}; }
    V3Global::dumpCheckGlobalTree("split_var_extra_pack_loop", 0, dumpTree() >= 3);
    // Call V3Const to clean up ASSIGN(CONCAT(CONCAT(...))) = CONCAT(CONCAT(...))
    V3Const::constifyAll(netlistp);
    V3DfgOptimizer::optimize(netlistp, "post split loop extra");

    if (v3Global.opt.fSplitExtraWide()) {
        auto extraReadRanges = SplitExtraWideVisitor::findExtraSplittable(netlistp);
        while (extraReadRanges.size()) {  // TODO do it in a while loop
            UINFO(3, "Trying to split extra non-loop variabbles " << endl);
            { SplitExtraPackVisitor{netlistp, extraReadRanges}; }
            V3Global::dumpCheckGlobalTree("split_var_extra_pack_wide", 0, dumpTree() >= 3);
            V3Const::constifyAll(netlistp);
            V3DfgOptimizer::optimize(netlistp, "post split extra");
            extraReadRanges = SplitExtraWideVisitor::findExtraSplittable(netlistp);
        }
    }

    V3Global::dumpCheckGlobalTree("split_var_extra_final", 0, dumpTree() >= 3);

    if (dump() >= 3) {
        const auto loopsp = SplitVariableCombLoopsVisitor::build(netlistp);
        if (!loopsp->empty()) { loopsp->dumpDotFilePrefixedAlways("split_var_extra_loops_left"); }
    }
}