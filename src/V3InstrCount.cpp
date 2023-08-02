// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Estimate instruction count to run the logic
//                         we would generate for any given AST subtree.
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

#include "V3InstrCount.h"

#include "V3Ast.h"

#include <iomanip>

VL_DEFINE_DEBUG_FUNCTIONS;

/// Estimate the instruction cost for executing all logic within and below
/// a given AST node. Note this estimates the number of instructions we'll
/// execute, not the number we'll generate. That is, for conditionals,
/// we'll count instructions from either the 'if' or the 'else' branch,
/// whichever is larger. We know we won't run both.
class IpuInstrCountOverride final : public VNVisitor {
private:
    int m_count = 0;

    inline void set(int c) { m_count = c; }

    struct IpuCostModel {
        static constexpr int BRANCH_PENALTY = 6;
        // sign extension
        static inline int extendS(AstNode*) { return 4 /* 2 ors 1 and 1 sub*/; }
        // comparison
        static inline int gtU(AstNode* nodep) {
            if (nodep->isWide()) {
                int n = nodep->widthWords();
                return (4 * n);
            } else if (nodep->isQuad()) {
                return (4);
            } else {
                return (1);
            }
        }
        static inline int gtEqU(AstNode* nodep) { return gtU(nodep) + 1; }
        static inline int gtS(AstNode* nodep) {
            if (nodep->isWide()) {
                int n = nodep->widthWords();
                return 7 + 4 * n;
            } else if (nodep->isQuad()) {
                return 11;
            } else {
                return extendS(nodep) * 2 + 1;
            }
        }
        static inline int gtEqS(AstNode* nodep) { return gtS(nodep) + 1; }

        static inline int eq(AstNode* nodep) {
            if (nodep->isWide()) {
                return 2 * nodep->widthWords() + 1;
            } else if (nodep->isQuad()) {
                return 3;
            } else {
                return 1;
            }
        }
        static inline int neq(AstNode* nodep) { return eq(nodep) + 1; }

        static inline int shiftRL(AstNode* nodep) {
            if (nodep->isQuad()) {
                return 6;
            } else if (nodep->isWide()) {
                return nodep->widthWords() * 4 + 3;
            } else {
                return 1;
            }
        }
        static inline int shiftRS(AstNode* nodep) {
            if (nodep->isQuad()) {
                return 12;
            } else if (nodep->isWide()) {
                return nodep->widthWords() * 4 + 5;
            } else {
                return 10;
            }
        }
        static inline int shiftQConst() { return 3; }

        static inline int logAnd(AstNode* nodep) { return nodep->widthWords() * 2; }
        static inline int logNot(AstNode* nodep) { return nodep->widthWords(); }
        // insert
        static inline int insert(AstNode* nodep) {
            if (nodep->isWide()) {
                return 10 * nodep->widthWords();
            } else if (nodep->isQuad()) {
                return 10;
            } else {
                return 5;
            }
        }
        // concatenation
        static inline int concat(AstNode* nodep) {
            if (nodep->isWide()) {
                return insert(nodep) + nodep->widthInstrs();
            } else if (nodep->isQuad()) {
                return 4;
            } else {
                return 2;
            }
        }

        static inline int add(AstNode* nodep) {
            if (nodep->isQuad()) {
                return 4;  // 3 add 1 cmpltu
            } else if (nodep->isWide()) {
                int n = nodep->widthWords();
                return ((2 * n + 1) /*add*/ + (n + 1) /*cmpltu*/);
            } else {
                return 1;
            }
        }
        static inline int sub(AstNode* nodep) { return add(nodep) + 1; }

        static inline int bitwise(AstNode* nodep) { return nodep->widthWords(); }

        static inline int mul(AstNode* nodep) {
            if (nodep->isWide()) {
                const int n = nodep->widthWords();
                const int mulCount = n * n * 23; /*QData * QData takes 23 instructions on the IPU*/
                // mul carry logic is a triple for loop i = 0 to n - 1, j = 0 to n -1 and k = i + j
                // to n - 1
                const int iters = (n * (n + 1) * (n + 2)) / 6;
                const int innerCount = iters * 3;
                return n + mulCount + innerCount;
            } else if (nodep->isQuad()) {
                return 23;  // 64-bit mul is expensive
            } else {
                return 1;  // native
            }
        }
        static inline int mulS(AstNode* nodep) { return mul(nodep) + 6; }

        static inline int negate(AstNode* nodep) {
            if (nodep->isWide()) {
                return 3 * nodep->widthWords();
            } else if (nodep->isQuad()) {
                return 5;
            } else {
                return 2;
            }
        }
        // RedAnd/Or
        static inline int reduceAndOr(AstNode* nodep) {
            if (nodep->isWide()) {
                return 4 + nodep->widthWords();
            } else if (nodep->isQuad()) {
                return 2;
            } else {
                return 1;
            }
        }

        static inline int reduceXor(AstNode* nodep) {
            if (nodep->isWide()) {
                return nodep->widthWords() + 10;
            } else if (nodep->isWide()) {
                return 20;
            } else {
                return 10;
            }
        }

        static inline int vref(AstVarRef* nodep) {
            if (const AstCMethodHard* const callp = VN_CAST(nodep->backp(), CMethodHard)) {
                if (callp->fromp() == nodep) return 1;
            }
            if (nodep->varp()->isFuncLocal()) {
                return nodep->widthWords();
            } else {
                return nodep->widthWords() + 1;
            }
        }
    };

#define VL_IPU_COST(N, FN) \
    void visit(Ast##N* nodep) override { set(IpuCostModel::FN(nodep)); }

    /// COMPARISON cost
    VL_IPU_COST(Gt, gtU);
    VL_IPU_COST(GtS, gtS);
    VL_IPU_COST(Gte, gtEqU);
    VL_IPU_COST(GteS, gtEqS);

    VL_IPU_COST(Lt, gtU);
    VL_IPU_COST(LtS, gtS);
    VL_IPU_COST(Lte, gtEqU);
    VL_IPU_COST(LteS, gtEqS);

    VL_IPU_COST(EqWild, eq);
    VL_IPU_COST(Eq, eq);
    VL_IPU_COST(EqCase, eq);
    VL_IPU_COST(NeqWild, neq);
    VL_IPU_COST(Neq, neq);
    VL_IPU_COST(NeqCase, neq);
    // TODO: handle T, D, N, and Eq/NeqLog as well

    VL_IPU_COST(LogAnd, logAnd);
    VL_IPU_COST(LogOr, logAnd);
    VL_IPU_COST(LogIf, logAnd);
    VL_IPU_COST(LogNot, logNot);

    VL_IPU_COST(Concat, concat);

    VL_IPU_COST(Add, add);
    VL_IPU_COST(Sub, sub);
    VL_IPU_COST(And, bitwise);
    VL_IPU_COST(Or, bitwise);
    VL_IPU_COST(Xor, bitwise);
    VL_IPU_COST(Mul, mul);
    VL_IPU_COST(MulS, mulS);
    VL_IPU_COST(Not, bitwise);
    VL_IPU_COST(Negate, negate);
    // TODO: handle (D)ouble and (N)string as well?

    VL_IPU_COST(RedAnd, reduceAndOr);
    VL_IPU_COST(RedOr, reduceAndOr);
    VL_IPU_COST(RedXor, reduceXor);

    VL_IPU_COST(ShiftL, shiftRL);
    VL_IPU_COST(ShiftR, shiftRL);
    VL_IPU_COST(ShiftRS, shiftRS);

    VL_IPU_COST(VarRef, vref);
    // TODO: AstReplicate

    void visit(AstNodeIf* nodep) override { set(IpuCostModel::BRANCH_PENALTY); }
    void visit(AstNodeCond* nodep) override {
        if (AstNodeAssign* const assignp = VN_CAST(nodep->backp(), NodeAssign)) {
            AstNodeVarRef* const lvp = VN_CAST(assignp->lhsp(), NodeVarRef);
            AstNodeVarRef* const elsep = VN_CAST(nodep->elsep(), NodeVarRef);
            if (lvp && elsep && lvp->varp() == elsep->varp()) {
                set(3);  // can become movz
                return;
            }
        }
        set(IpuCostModel::BRANCH_PENALTY);
    }
    void visit(AstConst* nodep) override { set(0); }

#undef VL_IPU_COST
    // default resolution, falls back to verilator's internal cost model
    void visit(AstNode* nodep) override { m_count = nodep->instrCount(); }

    explicit IpuInstrCountOverride(AstNode* nodep) { iterate(nodep); }

public:
    static int count(AstNode* nodep) {
        const IpuInstrCountOverride vi{nodep};
        return vi.m_count;
    }
};
class InstrCountVisitor final : public VNVisitor {
private:
    // NODE STATE
    //  AstNode::user4()        -> int.  Path cost + 1, 0 means don't dump
    //  AstNode::user5()        -> bool. Processed if assertNoDups
    const VNUser4InUse m_inuser4;

    // MEMBERS
    uint32_t m_instrCount = 0;  // Running count of instructions
    const AstNode* const m_startNodep;  // Start node of count
    bool m_tracingCall = false;  // Iterating into a CCall to a CFunc
    bool m_inCFunc = false;  // Inside AstCFunc
    bool m_ignoreRemaining = false;  // Ignore remaining statements in the block
    const bool m_assertNoDups;  // Check for duplicates
    const std::ostream* const m_osp;  // Dump file

    // TYPES
    // Little class to cleanly call startVisitBase/endVisitBase
    class VisitBase final {
    private:
        // MEMBERS
        uint32_t m_savedCount;
        AstNode* const m_nodep;
        InstrCountVisitor* const m_visitor;

    public:
        // CONSTRUCTORS
        VisitBase(InstrCountVisitor* visitor, AstNode* nodep)
            : m_nodep{nodep}
            , m_visitor{visitor} {
            m_savedCount = m_visitor->startVisitBase(nodep);
        }
        ~VisitBase() { m_visitor->endVisitBase(m_savedCount, m_nodep); }

    private:
        VL_UNCOPYABLE(VisitBase);
    };

public:
    // CONSTRUCTORS
    InstrCountVisitor(AstNode* nodep, bool assertNoDups, std::ostream* osp)
        : m_startNodep{nodep}
        , m_assertNoDups{assertNoDups}
        , m_osp{osp} {
        if (nodep) iterate(nodep);
    }
    ~InstrCountVisitor() override = default;

    // METHODS
    uint32_t instrCount() const { return m_instrCount; }

private:
    void reset() {
        m_instrCount = 0;
        m_ignoreRemaining = false;
    }
    uint32_t startVisitBase(AstNode* nodep) {
        UASSERT_OBJ(!m_ignoreRemaining, nodep, "Should not reach here if ignoring");
        if (m_assertNoDups && !m_inCFunc) {
            // Ensure we don't count the same node twice
            //
            // We only enable this assert for the initial LogicMTask counts
            // in V3Order. We can't enable it for the 2nd pass in V3EmitC,
            // as we expect mtasks to contain common logic after V3Combine,
            // so this would fail.
            //
            // Also, we expect some collisions within calls to CFuncs
            // (which at the V3Order stage represent verilog tasks, not to
            // the CFuncs that V3Order will generate.) So don't check for
            // collisions in CFuncs.
            UASSERT_OBJ(!nodep->user5p(), nodep,
                        "Node originally inserted below logic vertex "
                            << static_cast<AstNode*>(nodep->user5p()));
            nodep->user5p(const_cast<void*>(reinterpret_cast<const void*>(m_startNodep)));
        }

        // Save the count, and add it back in during ~VisitBase This allows
        // debug prints to show local cost of each subtree, so we can see a
        // hierarchical view of the cost when in debug mode.
        const uint32_t savedCount = m_instrCount;
        m_instrCount
            = v3Global.opt.poplar() ? IpuInstrCountOverride::count(nodep) : nodep->instrCount();
        return savedCount;
    }
    void endVisitBase(uint32_t savedCount, AstNode* nodep) {
        UINFO(8, "cost " << std::setw(6) << std::left << m_instrCount << "  " << nodep << endl);
        markCost(nodep);
        if (!m_ignoreRemaining) m_instrCount += savedCount;
    }
    void markCost(AstNode* nodep) {
        if (m_osp) nodep->user4(m_instrCount + 1);  // Else don't mark to avoid writeback
    }

    // VISITORS
    void visit(AstNodeSel* nodep) override {
        if (m_ignoreRemaining) return;
        // This covers both AstArraySel and AstWordSel
        //
        // If some vector is a bazillion dwords long, and we're selecting 1
        // dword to read or write from it, our cost should be small.
        //
        // Hence, exclude the child of the AstWordSel from the computation,
        // whose cost scales with the size of the entire (maybe large) vector.
        const VisitBase vb{this, nodep};
        iterateAndNextNull(nodep->bitp());
    }
    void visit(AstSel* nodep) override {
        if (m_ignoreRemaining) return;
        // Similar to AstNodeSel above, a small select into a large vector
        // is not expensive. Count the cost of the AstSel itself (scales with
        // its width) and the cost of the lsbp() and widthp() nodes, but not
        // the fromp() node which could be disproportionately large.
        const VisitBase vb{this, nodep};
        if (!VN_IS(nodep->fromp(), NodeVarRef)) {
            // there is actual computation going on
            iterateAndNextNull(nodep->fromp());
        }
        iterateAndNextNull(nodep->lsbp());
        iterateAndNextNull(nodep->widthp());
    }
    void visit(AstSliceSel* nodep) override {  // LCOV_EXCL_LINE
        nodep->v3fatalSrc("AstSliceSel unhandled");
    }
    void visit(AstMemberSel* nodep) override {  // LCOV_EXCL_LINE
        nodep->v3fatalSrc("AstMemberSel unhandled");
    }
    void visit(AstConcat* nodep) override {
        if (m_ignoreRemaining) return;
        // Nop.
        //
        // Ignore concat. The problem with counting concat is that when we
        // have many things concatted together, it's not a single
        // operation, but this:
        //
        //  concat(a, concat(b, concat(c, concat(d, ... ))))
        //
        // Then if we account a cost to each 'concat' that scales with its
        // width, this whole operation ends up with a cost accounting that
        // scales with N^2. Of course, the real operation isn't that
        // expensive: we won't copy each element over and over, we'll just
        // copy it once from its origin into its destination, so the actual
        // cost is linear with the size of the data. We don't need to count
        // the concat at all to reflect a linear cost; it's already there
        // in the width of the destination (which we count) and the sum of
        // the widths of the operands (ignored here).
        markCost(nodep);
    }
    void visit(AstNodeIf* nodep) override {
        if (m_ignoreRemaining) return;
        const VisitBase vb{this, nodep};
        iterateAndNextNull(nodep->condp());
        const uint32_t savedCount = m_instrCount;

        UINFO(8, "thensp:\n");
        reset();
        iterateAndNextNull(nodep->thensp());
        uint32_t ifCount = m_instrCount;
        if (nodep->branchPred().unlikely()) ifCount = 0;

        UINFO(8, "elsesp:\n");
        reset();
        iterateAndNextNull(nodep->elsesp());
        uint32_t elseCount = m_instrCount;
        if (nodep->branchPred().likely()) elseCount = 0;

        reset();
        if (ifCount >= elseCount) {
            m_instrCount = savedCount + ifCount;
            if (nodep->elsesp()) nodep->elsesp()->user4(0);  // Don't dump it
        } else {
            m_instrCount = savedCount + elseCount;
            if (nodep->thensp()) nodep->thensp()->user4(0);  // Don't dump it
        }
    }
    void visit(AstNodeCond* nodep) override {
        if (m_ignoreRemaining) return;
        // Just like if/else above, the ternary operator only evaluates
        // one of the two expressions, so only count the max.
        const VisitBase vb{this, nodep};
        iterateAndNextNull(nodep->condp());
        const uint32_t savedCount = m_instrCount;

        UINFO(8, "?\n");
        reset();
        iterateAndNextNull(nodep->thenp());
        const uint32_t ifCount = m_instrCount;

        UINFO(8, ":\n");
        reset();
        iterateAndNextNull(nodep->elsep());
        const uint32_t elseCount = m_instrCount;

        reset();
        if (ifCount < elseCount) {
            m_instrCount = savedCount + elseCount;
            if (nodep->thenp()) nodep->thenp()->user4(0);  // Don't dump it
        } else {
            m_instrCount = savedCount + ifCount;
            if (nodep->elsep()) nodep->elsep()->user4(0);  // Don't dump it
        }
    }
    void visit(AstCAwait* nodep) override {
        if (m_ignoreRemaining) return;
        iterateChildren(nodep);
        // Anything past a co_await is irrelevant
        m_ignoreRemaining = true;
    }
    void visit(AstFork* nodep) override {
        if (m_ignoreRemaining) return;
        const VisitBase vb{this, nodep};
        uint32_t totalCount = m_instrCount;
        // Sum counts in each statement until the first await
        for (AstNode* stmtp = nodep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            reset();
            iterate(stmtp);
            totalCount += m_instrCount;
        }
        m_instrCount = totalCount;
        m_ignoreRemaining = false;
    }
    void visit(AstActive* nodep) override {
        // You'd think that the OrderLogicVertex's would be disjoint trees
        // of stuff in the AST, but it isn't so: V3Order makes an
        // OrderLogicVertex for each ACTIVE, and then also makes an
        // OrderLogicVertex for each statement within the ACTIVE.
        //
        // To avoid double-counting costs, stop recursing and short-circuit
        // the computation for each ACTIVE.
        //
        // Our intent is that this only stops at the root node of the
        // search; there should be no actives beneath the root, as there
        // are no actives-under-actives.  In any case, check that we're at
        // root:
        markCost(nodep);
        UASSERT_OBJ(nodep == m_startNodep, nodep, "Multiple actives, or not start node");
    }
    void visit(AstNodeCCall* nodep) override {
        if (m_ignoreRemaining) return;
        const VisitBase vb{this, nodep};
        iterateChildren(nodep);
        m_tracingCall = true;
        iterate(nodep->funcp());
        UASSERT_OBJ(!m_tracingCall, nodep, "visit(AstCFunc) should have cleared m_tracingCall.");
    }
    void visit(AstCFunc* nodep) override {
        // Don't count a CFunc other than by tracing a call or counting it
        // from the root
        UASSERT_OBJ(m_tracingCall || nodep == m_startNodep, nodep,
                    "AstCFunc not under AstCCall, or not start node");
        UASSERT_OBJ(!m_ignoreRemaining, nodep, "Should not be ignoring at the start of a CFunc");
        m_tracingCall = false;
        VL_RESTORER(m_inCFunc);
        {
            m_inCFunc = true;
            const VisitBase vb{this, nodep};
            iterateChildren(nodep);
        }
        m_ignoreRemaining = false;
    }
    void visit(AstNode* nodep) override {
        if (m_ignoreRemaining) return;
        const VisitBase vb{this, nodep};
        iterateChildren(nodep);
    }

    VL_UNCOPYABLE(InstrCountVisitor);
};

// Iterate the graph printing the critical path marked by previous visitation
class InstrCountDumpVisitor final : public VNVisitor {
private:
    // NODE STATE
    //  AstNode::user4()        -> int.  Path cost, 0 means don't dump

    // MEMBERS
    std::ostream* const m_osp;  // Dump file
    unsigned m_depth = 0;  // Current tree depth for printing indent

public:
    // CONSTRUCTORS
    InstrCountDumpVisitor(AstNode* nodep, std::ostream* osp)
        : m_osp{osp} {
        // No check for nullptr output, so...
        UASSERT_OBJ(osp, nodep, "Don't call if not dumping");
        if (nodep) iterate(nodep);
    }
    ~InstrCountDumpVisitor() override = default;

private:
    // METHODS
    string indent() const { return string(m_depth, ':') + " "; }
    void visit(AstNode* nodep) override {
        ++m_depth;
        if (unsigned costPlus1 = nodep->user4()) {
            *m_osp << "  " << indent() << "cost " << std::setw(6) << std::left << (costPlus1 - 1)
                   << "  " << nodep << '\n';
            iterateChildren(nodep);
        }
        --m_depth;
    }

    VL_UNCOPYABLE(InstrCountDumpVisitor);
};

uint32_t V3InstrCount::count(AstNode* nodep, bool assertNoDups, std::ostream* osp) {
    const InstrCountVisitor visitor{nodep, assertNoDups, osp};
    if (osp) InstrCountDumpVisitor dumper{nodep, osp};
    return visitor.instrCount();
}
