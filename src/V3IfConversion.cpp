// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Add temporaries, such as for delayed nodes
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
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3IfConversion.h"

#include "V3Ast.h"
#include "V3Const.h"
#include "V3Dead.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <algorithm>
#include <map>
#include <stack>
VL_DEFINE_DEBUG_FUNCTIONS;

// Simple visitor that modifies the access flag in an expression
class AccessModifierVisitor final : public VNVisitor {
private:
    const VAccess m_flag;

    void visit(AstNodeVarRef* vrefp) { vrefp->access(m_flag); }
    void visit(AstNode* nodep) { iterateChildren(nodep); }

public:
    explicit AccessModifierVisitor(AstNodeExpr* nodep, const VAccess& flag)
        : m_flag(flag) {
        iterate(nodep);
    }
};

/// Hoists all AstIf::condp() evaluation out of the loop so that every
/// condp() is strictly a AstVarRef.
/// This is required step before IfConversionVisitor, where statements inside
/// the if are also brought up into ternary statements of predicated statements.
/// If we do not "atomize" condp() expression, IfConversionVisitor will duplicated
/// them which is bad for performance and also is incorrect with impure expressions

class IfConditionAsAtomVisitor final : public VNVisitor {
private:
    V3UniqueNames m_condFreshName;  // generate unique names for if-else conditions
    AstScope* m_scopep = nullptr;
    // AstNodeModule* m_modp = nullptr;
    AstNodeProcedure* m_procp = nullptr;
    std::vector<AstNodeAssign*> m_defaultAssigns;
    /// VISITORS
    // void visit(AstModule* nodep) override {
    //     UINFO(4, "MOD  " << nodep << endl);
    //     VL_RESTORER(m_modp);
    //     {
    //         m_modp = nodep;
    //         iterateChildren(nodep);
    //     }
    // }
    void visit(AstNodeProcedure* nodep) override {
        VL_RESTORER(m_procp);
        {
            m_procp = nodep;
            m_defaultAssigns.clear();
            iterateChildren(nodep);
            if (nodep->stmtsp()) {
                AstNode* nextp = nodep->stmtsp();
                for (AstNodeAssign* newp : m_defaultAssigns) {
                    nextp->addNextHere(newp);
                    nextp = nextp->nextp();
                }
            }
        }
    }
    void visit(AstScope* nodep) override {
        VL_RESTORER(m_scopep);
        {
            m_scopep = nodep;
            m_condFreshName.reset();
            iterateChildren(nodep);
        }
    }
    void visit(AstIf* ifp) {
        if (!m_scopep || !m_procp) { return; }  // in case not in a module

        UINFO(10, "Visiting AstIf " << ifp << endl);
        AstNodeExpr* origCondp = ifp->condp();
        // no need to make the condition a VarRef if already a VarRef
        if (VN_IS(origCondp, VarRef)) { return; }

        // create a new variable
        const std::string vname = m_condFreshName.get("ifcond");
        FileLine* const fl = origCondp->fileline();
        AstVarScope* const vscp = m_scopep->createTemp(vname, 1);
        // // create a reference
        AstVarRef* const vrefp = new AstVarRef{fl, vscp, VAccess::WRITE};
        // create an assignment
        AstAssign* const assignp = new AstAssign{fl, vrefp, origCondp->cloneTree(true)};

        origCondp->replaceWith(new AstVarRef{fl, vscp, VAccess::READ});
        VL_DO_DANGLING(origCondp->deleteTree(), origCondp);

        // assign condition value to be 0
        m_defaultAssigns.push_back(new AstAssign{fl, new AstVarRef{fl, vscp, VAccess::WRITE},
                                                 new AstConst{fl, V3Number{fl, 1, 0}}});
        ifp->addHereThisAsNext(assignp);
        iterateAndNextNull(ifp->thensp());
        iterateAndNextNull(ifp->elsesp());
        // AstVarScope* vscp = new AstVar
    }
    void visit(AstNode* nodep) { iterateChildren(nodep); }

public:
    explicit IfConditionAsAtomVisitor(AstNetlist* nodep)
        : m_condFreshName("__Vlvcond") {
        iterate(nodep);
    }
};
class RemoveDelayedVisitor final : public VNVisitor {
private:
    enum class Phase : uint8_t { NOP, CHECK, REPLACE };
    Phase m_phase = Phase::NOP;
    bool m_inDly = false;
    V3UniqueNames m_oldNames;
    std::map<AstVarScope*, AstVarScope*> m_dlyd;
    AstScope* m_scopep = nullptr;

    /// VISITORS
    void visit(AstAlways* nodep) {
        VL_RESTORER(m_phase);
        {
            m_phase = Phase::CHECK;
            m_dlyd.clear();
            // iterate the children and collect all VarScopes that are an lhs
            iterateChildren(nodep);
            UASSERT_OBJ(m_scopep, nodep,
                        "Expected valid scope in procecural block " << nodep << endl);
            // we now have a list all VarScopes that are an lvalue in some AssignDly
            // for each one, we create a new "oldValue" VarScope and replace the
            // lvalue references of them in the block
            for (auto& subst : m_dlyd) {
                auto oldp
                    = m_scopep->createTempLike(m_oldNames.get(subst.first->name()), subst.first);
                subst.second = oldp;
            }
            // iterate again to replace the rvalue references, and turn every AssignDly to Assign
            m_phase = Phase::REPLACE;
            iterateChildren(nodep);
            // now initialize the "oldValues"
            for (const auto& subst : m_dlyd) {

                AstVarRef* const lp
                    = new AstVarRef{subst.second->fileline(), subst.second, VAccess::WRITE};
                AstVarRef* const lSelfp = lp->cloneTree(true);
                lSelfp->access(VAccess::READ);
                AstVarRef* const rp
                    = new AstVarRef{subst.first->fileline(), subst.first, VAccess::READ};

                AstVarRef* const rSelfp = rp->cloneTree(true);
                rSelfp->access(VAccess::WRITE);
                // for every x <= expr add the following to the begining of the procedure
                // x_old = x
                // x = x_old
                // note the reverse order of adding statements
                nodep->stmtsp()->addHereThisAsNext(new AstAssign{rp->fileline(), rSelfp, lSelfp});
                nodep->stmtsp()->addHereThisAsNext(new AstAssign{rp->fileline(), lp, rp});
                // x = x_old is a redundant assignment, but we add it so that the code generated
                // by the IfConverionVisitor is slighty more understandable.
            }
        }
    }

    void visit(AstAssignDly* nodep) {
        VL_RESTORER(m_inDly);
        if (m_phase == Phase::CHECK) {
            m_inDly = true;
            iterate(nodep->lhsp());
        } else if (m_phase == Phase::REPLACE) {
            m_inDly = false;
            iterateChildren(nodep);
            nodep->replaceWith(new AstAssign{nodep->fileline(), nodep->lhsp()->cloneTree(true),
                                             nodep->rhsp()->cloneTree(true)});
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
        }
    }
    void visit(AstVarRef* nodep) {
        if (m_phase == Phase::CHECK && m_inDly && nodep->access().isWriteOrRW()
            && VN_IS(nodep->varScopep()->dtypep(), BasicDType)) {
            // lvalue in a delayed assignment that is a simple type, i.e., only
            // packed arrays
            m_dlyd.emplace(nodep->varScopep(), nullptr);
        } else if (m_phase == Phase::REPLACE && nodep->access().isReadOnly()) {
            // rvalue need to be replaced
            auto it = m_dlyd.find(nodep->varScopep());
            if (it != m_dlyd.end()) {
                AstVarScope* newVscp = (*it).second;
                nodep->replaceWith(new AstVarRef{nodep->fileline(), newVscp, nodep->access()});
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
            }
        }
    }
    void visit(AstScope* nodep) {
        VL_RESTORER(m_scopep);
        {
            m_scopep = nodep;
            // m_dlyd.clear();
            iterateChildren(nodep);
        }
    }

    void visit(AstNode* nodep) { iterateChildren(nodep); }

public:
    explicit RemoveDelayedVisitor(AstNetlist* nodep)
        : m_oldNames("__VrvOld") {
        iterate(nodep);
    }
};
class IfConversionVisitor final : public VNVisitor {
private:
    // STATE
    // Cleared on module
    // AstAssign::user1() -> bool. Set true if already processed

    const VNUser1InUse m_inuser1;

    AstNodeProcedure* m_procp = nullptr;  // current enclosing always block

    AstScope* m_scopep = nullptr;  // current scope

    VDouble0 m_statsRemoved;  // number of removed ifs, for statistics tracking

    std::pair<AstIf*, bool> m_inIf;  // last if node, then (true) or else branch (false)
    std::vector<AstNodeStmt*> m_hoisted;  // hoisted statements

    bool m_isArraySel = false;
    // VISITORS
    void visit(AstNetlist* nodep) override {
        // in a new module, so clear state up
        iterateChildren(nodep);
    }

    void visit(AstNodeProcedure* nodep) override {

        VL_RESTORER(m_procp);
        {
            m_procp = nodep;
            iterateChildren(nodep);
            V3Const::constifyEdit(nodep);
        }
    }

    void visit(AstScope* nodep) override {
        VL_RESTORER(m_scopep);
        {
            m_scopep = nodep;

            iterateChildren(nodep);
        }
    }
    void visit(AstArraySel*) override { m_isArraySel = true; }
    void visit(AstNodeAssign* assignp) override {
        if (!m_inIf.first) { return; }  // not in an if statement

        UINFO(20, assignp << endl);
        m_isArraySel = false;
        // iterate and check whether this is an assignment to a memory (unpacked array)
        iterate(assignp->lhsp());
        FileLine* fl = assignp->fileline();
        if (m_isArraySel) {
            // assignment to a memory, need to predicate it
            predicate(assignp);
        } else {
            AstNodeExpr* defaultp = assignp->lhsp()->cloneTree(true);
            { AccessModifierVisitor{defaultp, VAccess::READ}; }

            AstNodeExpr* condp = nullptr;
            AstNodeExpr* origCondp = m_inIf.first->condp()->cloneTree(true);
            if (m_inIf.second) {  // then branch
                condp = origCondp;
            } else {
                condp = new AstLogNot{origCondp->fileline(), origCondp};
            }

            AstNodeExpr* rhsp = assignp->rhsp()->cloneTree(true);
            AstCond* ternaryp = new AstCond{fl, condp, assignp->rhsp()->cloneTree(true), defaultp};
            AstNodeExpr* lhsp = assignp->lhsp()->cloneTree(true);
            AstNodeAssign* newAssignp = assignp->cloneType(lhsp, ternaryp);
            m_hoisted.push_back(newAssignp);

            // delete the transformed assignment
            UINFO(20, "Unlinking " << assignp << endl);
            assignp->unlinkFrBack();
            VL_DO_DANGLING(pushDeletep(assignp), assignp);
        }
    }

    void predicate(AstNodeStmt* stmtp) {
        AstNodeExpr* condp = nullptr;
        AstNodeExpr* origCondp = m_inIf.first->condp()->cloneTree(true);
        if (m_inIf.second) {  // then branch
            condp = origCondp;
        } else {
            condp = new AstLogNot{origCondp->fileline(), origCondp};
        }
        AstNodeStmt* stmtCopyp = stmtp->cloneTree(
            /*Don't clone next, want to wrap individual statements*/
            false);

        m_hoisted.push_back(new AstPredicatedStmt(stmtp->fileline(), condp, stmtCopyp));
        UINFO(20, "Unlinking " << stmtp << endl);
        // delete it
        stmtp->unlinkFrBack();
        VL_DO_DANGLING(pushDeletep(stmtp), stmtp);
    }

    void visit(AstNodeStmt* stmtp) override {
        if (m_inIf.first) {
            UINFO(20, stmtp << endl);
            predicate(stmtp);
        }
    }

    void visit(AstIf* ifp) override {
        if (!m_scopep || !m_procp) { return; }  // nothing to do
        VL_RESTORER(m_inIf);
        AstNode* nextp = ifp;
        {
            VL_RESTORER(m_hoisted);
            m_inIf = std::make_pair(ifp, /* in then */ true);
            UINFO(10, "(got if) " << ifp << endl);
            UINFO(10, "Visiting THEN" << endl);
            m_hoisted.clear();  // clear the stack of assignments
            iterateAndNextNull(ifp->thensp());
            for (AstNodeStmt* newp : m_hoisted) {
                nextp->addNextHere(newp);
                nextp = nextp->nextp();
            }
        }
        if (ifp->elsesp()) {
            VL_RESTORER(m_hoisted);
            m_inIf = std::make_pair(ifp, false /*not in then, in else*/);
            m_hoisted.clear();
            UINFO(10, "Visiting ELSE" << endl);
            iterateAndNextNull(ifp->elsesp());
            for (AstNodeStmt* newp : m_hoisted) {
                nextp->addNextHere(newp);
                nextp = nextp->nextp();
            }
            // iterateAndNextNull(ifp->elsesp());
        }
    }

    //-------------------
    // Fallback
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit IfConversionVisitor(AstNetlist* nodep) { iterate(nodep); }
    ~IfConversionVisitor() override { V3Stats::addStat("If removal, number of ifs", 0); }
};

class SingleAssignmentVisitor final : public VNVisitor {
private:
    // STATE
    // clear on AstVarScope
    // AstVarRef::user1() -> bool. Set true if already processed

    const VNUser1InUse m_inuser1;

    V3UniqueNames m_lvFreshName;

    AstAlways* m_procp = nullptr;
    AstScope* m_scopep = nullptr;
    std::map<AstVarScope*, AstVarScope*> m_subst;

    struct Lhs {
        AstSel* selp = nullptr;
        AstNodeAssign* assignp = nullptr;
        AstArraySel* aselp = nullptr;
        AstNodeExpr* rhsp = nullptr;
        AstNodeExpr* lhsp = nullptr;
        bool dly() const { return VN_IS(assignp, AssignDly); }
    } m_lhsp;
    /// VISITORS

    AstVarScope* renamed(AstVarScope* vscp) const {
        auto it = m_subst.find(vscp);
        if (it == m_subst.end()) {
            return vscp;
        } else {
            return (*it).second;
        }
    }
    void updateSubst(AstVarScope* origp, AstVarScope* newp) {
        if (m_subst.find(origp) != m_subst.end()) { m_subst.erase(origp); }
        m_subst.insert({origp, newp});
    }
    void visit(AstVarRef* nodep) override {
        bool isLvalue = nodep->access().isWriteOrRW();
        if (!m_scopep || nodep->user1()) { return; }  // not in a module
        UINFO(3, "visiting VarRef " << nodep << endl);
        nodep->user1(true);
        if (!isLvalue) {
            auto substIt = m_subst.find(nodep->varScopep());
            if (substIt != m_subst.end()) {
                // replace the var ref
                AstVarScope* subst = (*substIt).second;
                AstVarRef* const newp = new AstVarRef{nodep->fileline(), subst, nodep->access()};
                newp->user1(true);
                nodep->replaceWith(newp);
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
            } else {
                // no need to substitute
            }
        } else {
            // clang-format off
            // left hand side, need to create a new name:
            // 1. m[x][y+:w] = rhs; ArraySel and AstSel
            // becomes
            //      fresh2    = m[renamed(x)];
            //      fresh3    = fresh2 & ((1 << width(fresh2)) - 1) - (((1 << w) - 1) <<renamed(y)));
            //      fresh4    = (renamed(rhs) << renamed(y)) & ((1 << w) - 1); fresh5
            //      = fresh3 | fresh4; m[renamed[x]] = fresh5; or <=
            // 2. m[x] = rhs
            // becomes
            //    m[renamed(x)] = renamed(rhs); or <=
            // 3. s[y+:w] = rhs;  only AstSel
            // becomes
            //      fresh1 = (renamed(rhs) << renamed(y)) & ((1 << w) - 1);
            //      fresh2 = renamed(s) & (((1 << width(s)) - 1) - ((1 << w) - 1) << renamed(y)));
            //      fresh3 = fresh2 | fresh1;
            //      rename s to fresh3
            // 4. s = rhs;
            // becomes
            //      fresh1 = renamed(rhs); // or <=
            //      rename s to fresh1;
            // clang-format on
            // let's handle the easy case first
            FileLine* flp = nodep->fileline();
            if (!m_lhsp.aselp && !m_lhsp.selp) {

                // create a fresh variable and replace the reference, then notify
                // m_subst of the new name.
                // Note this may as well be the first assignment, and eventhough there is
                // no real need for renaming we do it so that at the end the "original name"
                // is used as a single assignment, non-blocking style.
                AstVarScope* const vscp = m_scopep->createTempLike(
                    m_lvFreshName.get(nodep->name()), nodep->varScopep());
                AstVarRef* const newp = new AstVarRef{nodep->fileline(), vscp, VAccess::WRITE};
                newp->user1(true);
                UASSERT_OBJ(
                    !m_lhsp.dly(), nodep,
                    "Did not expect blocking assignment here. Make sure blocking assignments "
                    "are removed.");

                updateSubst(nodep->varScopep(), vscp);

                nodep->replaceWith(newp);
                VL_DO_DANGLING(pushDeletep(nodep), nodep);

            } else if (!m_lhsp.aselp && m_lhsp.selp) {
                // read-modify-write on scalar 1-d(packed array)
                // we rename the variable whether it has been assigned before or not
                AstNodeExpr* const newExprp = readModifyWriteExpr(
                    flp, new AstVarRef{flp, renamed(nodep->varScopep()), VAccess::READ},
                    static_cast<int>(nodep->varScopep()->width()),
                    static_cast<int>(VN_AS(m_lhsp.selp->widthp(), Const)->num().toUInt()));
                AstNodeExpr* oldLhsp = m_lhsp.assignp->lhsp();
                AstNodeExpr* oldRhsp = m_lhsp.assignp->rhsp();
                AstVarScope* const vscp = m_scopep->createTempLike(
                    m_lvFreshName.get(nodep->name()), nodep->varScopep());
                AstVarRef* const newp = new AstVarRef{flp, vscp, VAccess::WRITE};
                newp->user1(true);
                oldLhsp->replaceWith(newp);
                oldRhsp->replaceWith(newExprp);
                UASSERT_OBJ(!m_lhsp.dly(), nodep, "Make sure blocking assignments are removed!");
                updateSubst(nodep->varScopep(), vscp);
                VL_DO_DANGLING(pushDeletep(oldLhsp), oldLhsp);
                VL_DO_DANGLING(pushDeletep(oldRhsp), oldRhsp);
            } else if (m_lhsp.aselp && !m_lhsp.selp) {
                // write to a an array nothing to do
                UINFO(10, "Simple write to array" << endl);
            } else {  // m_lhsp.aselp && m_lhsp.selp
                // only works for 1-d unpacked arrays, VN_AS fails otherwise
                AstVarRef* const readFromp = VN_AS(m_lhsp.aselp->fromp(), VarRef)->cloneTree(true);
                readFromp->access(VAccess::READ);

                AstArraySel* const oldValuep
                    = new AstArraySel{flp, readFromp, m_lhsp.aselp->bitp()->cloneTree(true)};
                AstNodeExpr* const newExprp = readModifyWriteExpr(
                    flp, oldValuep, static_cast<int>(readFromp->width()),
                    static_cast<int>(VN_AS(m_lhsp.selp->widthp(), Const)->num().toUInt()));
                AstArraySel* const newp
                    = new AstArraySel{flp, m_lhsp.aselp->fromp()->cloneTree(true),
                                      m_lhsp.aselp->bitp()->cloneTree(true)};
                newp->fromp()->user1(true);
                AstNodeExpr* oldLhsp = m_lhsp.assignp->lhsp();
                AstNodeExpr* oldRhsp = m_lhsp.assignp->rhsp();
                oldLhsp->replaceWith(newp);
                oldRhsp->replaceWith(newExprp);
                VL_DO_DANGLING(pushDeletep(oldLhsp), oldLhsp);
                VL_DO_DANGLING(pushDeletep(oldRhsp), oldRhsp);
            }
        }
    }

    AstNodeExpr* readModifyWriteExpr(FileLine* const flp, AstNodeExpr* const oldValuep,
                                     int lhsWidth, int sliceWidth) {
        V3Number lhsMask{flp, lhsWidth, 0};
        V3Number rhsMask{flp, lhsWidth, 0};
        lhsMask.setMask(lhsWidth);
        rhsMask.setMask(sliceWidth);

        AstNodeExpr* const bitMaskp
            = new AstSub{flp, new AstConst{flp, lhsMask},
                         new AstShiftL{flp, new AstConst{flp, rhsMask},
                                       m_lhsp.selp->lsbp()->cloneTree(true), lhsWidth}};

        AstAnd* const oldExprp = new AstAnd{flp, oldValuep, bitMaskp};
        AstShiftL* const rhsShiftedp = new AstShiftL{
            flp, m_lhsp.rhsp->cloneTree(true), m_lhsp.selp->lsbp()->cloneTree(true), lhsWidth};
        AstAnd* const newContrp
            = new AstAnd{flp, rhsShiftedp, new AstNot{flp, bitMaskp->cloneTree(true)}};

        AstOr* const newExprp = new AstOr{flp, oldExprp, newContrp};
        // return newExprp;
        return V3Const::constifyEdit(newExprp);
    }
    void visit(AstVarScope* nodep) override {
        auto dims = nodep->dtypep()->dimensions(true);
        if (dims.first > 1 || dims.second > 1) {
            nodep->v3warn(E_UNSUPPORTED,
                          "multidimensional (packed/unpacked) arrays not supported");
        }
    }
    void visit(AstSel* nodep) override {
        // default iteration of fromp[lsbp :+ widthp] is fromp, lsbp, then widthp.
        // We want to first visit lsbp though, since we are renaming it and the
        // fromp needs to see the renamed version
        iterate(nodep->lsbp());
        VL_RESTORER(m_lhsp);
        {
            m_lhsp.selp = nodep;
            iterate(nodep->fromp());
        }
    }

    void visit(AstArraySel* nodep) override {
        // iterate the bitp first, i.e., reverse order.
        // This helps with the AstVarRef visitor since we first rename
        // bitp(), and then fromp() which is essentially a left hand-side value.
        iterate(nodep->bitp());
        VL_RESTORER(m_lhsp);
        {
            m_lhsp.aselp = nodep;
            iterate(nodep->fromp());
        }
    }


    void visit(AstNodeAssign* nodep) override {
        if (!m_procp) { return; }  // nothing to do, AssignW handled differently
        iterate(nodep->rhsp());
        VL_RESTORER(m_lhsp);
        {
            m_lhsp.lhsp = nodep->lhsp();  // set after iteration, don't move up
            m_lhsp.rhsp = nodep->rhsp();  // set after iteration
            m_lhsp.assignp = nodep;
            iterate(nodep->lhsp());
        }
    }

    void visit(AstScope* nodep) override {
        VL_RESTORER(m_scopep);
        {
            m_scopep = nodep;
            m_subst.clear();
            AstNode::user1ClearTree();
            iterateChildren(nodep);
        }
    }
    void visit(AstAlways* nodep) override {
        UINFO(3, "Visiting Always" << nodep);
        VL_RESTORER(m_procp);
        {
            m_procp = nodep;
            m_subst.clear();
            iterateChildren(nodep);
            // persist every substitution
            // e.g., r = r_last_assign
            for (auto it = m_subst.crbegin(); it != m_subst.crend(); it++) {
                FileLine* const flp = it->first->fileline();
                AstVarRef* const lp = new AstVarRef{flp, it->first, VAccess::WRITE};
                AstVarRef* const rp = new AstVarRef{flp, it->second, VAccess::READ};
                AstAssign* const newp = new AstAssign{it->first->fileline(), lp, rp};
                nodep->addStmtsp(newp);
            }
        }
    }

    /// FALLBACK VISITOR
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit SingleAssignmentVisitor(AstNetlist* nodep)
        : m_lvFreshName("__Vlvsa") {
        iterate(nodep);
    }
};

/// Turns the nested expression into something that resembles three address code, e.g.,
/// a = expr1 + (expr2 | (expr3 & expr4 ));
/// becomes
/// at1 = expr1;
/// at2 = expr2;
/// ....
/// aa = at3 & at4;
/// ab = at2 | aa;
/// ac = at1 + at2;
class ThreeAddressCodeConversionVisitor final : public VNVisitor {
private:
    /// STATE
    /// clear on varscope
    /// AstNodeExpr::user1() -> bool true if node can not be simplified
    const VNUser1InUse m_inuser1;

    V3UniqueNames m_rvTempExpr;
    AstNodeStmt* m_stmtp = nullptr;
    AstNodeExpr* m_exprp = nullptr;
    AstScope* m_vscp = nullptr;
    bool m_expand = false;

    inline bool isAtom(const AstNode* const nodep) const {
        return VN_IS(nodep, VarRef) || VN_IS(nodep, Const);
    }
    inline bool isSimpleExpr(const AstNodeExpr* const nodep) const {

        if (auto biop = VN_CAST(nodep, NodeBiop)) {
            return isAtom(biop->lhsp()) && isAtom(biop->rhsp());
        } else if (auto triop = VN_CAST(nodep, NodeTriop)) {
            return isAtom(triop->lhsp()) && isAtom(triop->rhsp()) && isAtom(triop->thsp());
        } else if (auto unop = VN_CAST(nodep, NodeUniop)) {
            return isAtom(unop->lhsp());
        } else {
            return isAtom(nodep);
        }
    }

    void visit(AstNodeExpr* nodep) override {
        if (isAtom(nodep) || nodep->user1() == true) {
            // cannot simplify an atom, or have already tried simplifying, e.g., with SFormatF
            return;
        }
        const bool simple = isSimpleExpr(nodep);
        if ((simple && !VN_IS(nodep->abovep(), NodeAssign)) || !simple) {
            UINFO(3, "Simplifiying " << nodep << "with type " << nodep->dtypep() << endl);
            nodep->user1(true);
            iterateChildren(nodep);
            bool memoryWrite
                = VN_IS(nodep, ArraySel)  // is it memory access
                  && VN_IS(VN_AS(nodep, ArraySel)->fromp(), VarRef)  // is memory access simple
                  && VN_AS(VN_AS(nodep, ArraySel)->fromp(), VarRef)
                         ->access()
                         .isWriteOrRW();  // is it a write (LV)?
            if (VN_IS(nodep->dtypep(), BasicDType)
                && VN_AS(nodep->dtypep(), BasicDType)->keyword().isIntNumeric() && !memoryWrite) {
                // only create assignments for basic "int" types
                // this include: bit, logic, byte, int, ...
                // but does not include string, or unpacked arrays (memories)
                AstVarScope* const newlvp
                    = m_vscp->createTemp(m_rvTempExpr.get("rvExpr"), nodep->dtypep());
                AstNodeAssign* assignp = nullptr;
                AstVarRef* lhsp = new AstVarRef{nodep->fileline(), newlvp, VAccess::WRITE};
                AstNodeExpr* rhsp = nodep->cloneTree(true);
                if (VN_IS(m_stmtp, AssignW)) {
                    assignp = new AstAssignW{nodep->fileline(), lhsp, rhsp};
                } else {
                    assignp = new AstAssign{nodep->fileline(), lhsp, rhsp};
                }
                nodep->replaceWith(new AstVarRef{nodep->fileline(), newlvp, VAccess::READ});
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
                m_stmtp->addHereThisAsNext(assignp);
            }
        }
    }

    void visit(AstScope* nodep) override {
        VL_RESTORER(m_vscp);
        {
            m_vscp = nodep;
            AstNode::user1ClearTree();
            iterateChildren(nodep);
        }
    }
    void visit(AstNodeStmt* nodep) override {
        VL_RESTORER(m_stmtp);
        {
            m_stmtp = nodep;
            iterateChildren(nodep);
        }
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit ThreeAddressCodeConversionVisitor(AstNetlist* nodep)
        : m_rvTempExpr("__VrvTmpExpr") {
        iterate(nodep);
    }
};
void V3IfConversion::predicatedAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { RemoveDelayedVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("dlyremove", 0, dumpTree() >= 1);
    { IfConditionAsAtomVisitor{nodep}; }  // desctory the visitor
    V3Global::dumpCheckGlobalTree("ifcondtion", 0, dumpTree() >= 1);
    { IfConversionVisitor{nodep}; }  // desctory the visitor
    V3Global::dumpCheckGlobalTree("ifconversion", 0, dumpTree() >= 1);
    { SingleAssignmentVisitor{nodep}; }  // destroy the visitor
    V3Global::dumpCheckGlobalTree("singleAssignment", 0, dumpTree() >= 1);
    { ThreeAddressCodeConversionVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("tac", 0, dumpTree() >= 1);
    V3Const::constifyAll(nodep);
    V3Dead::deadifyAll(nodep);
}
