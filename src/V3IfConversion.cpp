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

    AstNodeProcedure* m_procp = nullptr;
    AstScope* m_scopep = nullptr;
    std::map<AstVarScope*, AstVarScope*> m_subst;
    std::vector<std::pair<AstVarRef*, AstNodeExpr*>> m_assignps;
    struct Lhs {
        AstSel* selp = nullptr;
        AstNodeAssign* assignp = nullptr;
        AstArraySel* aselp = nullptr;
        AstNodeExpr* rhsp = nullptr;
        AstNodeExpr* lhsp = nullptr;
        bool valid() const { return lhsp != nullptr; }
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
    void visit(AstVarRef* nodep) {

        if (!m_scopep || nodep->user1()) { return; }  // not in a module
        UINFO(3, "visiting VarRef " << nodep << endl);
        nodep->user1(true);
        if (!m_lhsp.valid()) {
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
                if (m_subst.find(nodep->varScopep()) == m_subst.end()) {
                    // if the variable has not been assigned yet, there is no need to rename it
                    // renaming it would be correct but is less efficient
                    if (!m_lhsp.dly()) { updateSubst(nodep->varScopep(), nodep->varScopep()); }
                } else {
                    // there has been at least one assignment to the variable
                    // create a fresh variable and replace the reference, then notify
                    // m_subst of the new name
                    AstVarScope* const vscp = m_scopep->createTempLike(
                        m_lvFreshName.get(nodep->name()), nodep->varScopep());
                    AstVarRef* const newp = new AstVarRef{nodep->fileline(), vscp, VAccess::WRITE};
                    newp->user1(true);

                    if (!m_lhsp.dly()) { updateSubst(nodep->varScopep(), vscp); }
                    VL_DO_DANGLING(pushDeletep(nodep), nodep);
                }
            } else if (!m_lhsp.aselp && m_lhsp.selp) {
                // read-modify-write on scalar 1-d(packed array)
                // we rename the variable whether it has been assigned before or not
                AstNodeExpr* const newExprp = readModifyWriteExpr(
                    flp, new AstVarRef{flp, renamed(nodep->varScopep()), VAccess::READ},
                    static_cast<int>(nodep->varScopep()->width()),
                    static_cast<int>(VN_AS(m_lhsp.selp->widthp(), Const)->num().toUInt()));
                AstNodeExpr* oldLhsp = m_lhsp.assignp->lhsp();
                AstNodeExpr* oldRhsp = m_lhsp.assignp->rhsp();
                AstVarScope* const vscp
                    = m_scopep->createTempLike(m_lvFreshName.get(nodep), nodep->varScopep());
                AstVarRef* const newp = new AstVarRef{flp, vscp, VAccess::WRITE};
                newp->user1(true);
                oldLhsp->replaceWith(newp);
                oldRhsp->replaceWith(newExprp);
                if (!m_lhsp.dly()) { updateSubst(nodep->varScopep(), vscp); }
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
                oldLhsp->replaceWith(oldLhsp->cloneTree(true));
                oldRhsp->replaceWith(oldRhsp->cloneTree(true));
                // VL_DO_DANGLING(pushDeletep(oldLhsp), oldLhsp);
                // VL_DO_DANGLING(pushDeletep(oldRhsp), oldRhsp);
            }
        }
    }
    V3Number mkMask(FileLine* const flp, int w) {
        V3Number m{flp, w, 0};
        m.setMask(w);
        return m;
    }
    AstNodeExpr* readModifyWriteExpr(FileLine* const flp, AstNodeExpr* const oldValuep,
                                     int lhsWidth, int sliceWidth) {
        V3Number lhsMask = mkMask(flp, lhsWidth);
        V3Number rhsMask = mkMask(flp, sliceWidth);

        AstNodeExpr* const lsbpShiftedp
            = new AstShiftL{flp, new AstConst{flp, rhsMask}, m_lhsp.selp->lsbp()->cloneTree(true)};
        // clang-format off
        AstAnd* const oldExprp = new AstAnd{
            flp,
            oldValuep,
            new AstSub{
                flp,
                new AstConst{flp, lhsMask},
                new AstShiftL{
                    flp,
                    new AstConst{flp, rhsMask},
                    m_lhsp.selp->lsbp()->cloneTree(true),
                    lhsWidth
                }
            }
        };
        AstOr* const newExprp = new AstOr{
            flp,
            oldExprp,
            new AstShiftL{flp,
                m_lhsp.rhsp->cloneTree(true),
                m_lhsp.selp->lsbp()->cloneTree(true),
                sliceWidth
            }
        };
        // clang-format on
        return newExprp;
    }
    void visit(AstVarScope* nodep) {
        auto dims = nodep->dtypep()->dimensions(true);
        if (dims.first > 1 || dims.second > 1) {
            nodep->v3warn(E_UNSUPPORTED,
                          "multidimensional (packed/unpacked) arrays not supported");
        }
    }
    void visit(AstSel* nodep) {
        VL_RESTORER(m_lhsp);
        {
            // default iteration of fromp[lsbp :+ widthp] is fromp, lsbp, then widthp.
            // We want to first visit lsbp though, since we are renaming it and the
            // fromp needs to see the renamed version
            m_lhsp.selp = nodep;
            iterate(nodep->lsbp());
            iterate(nodep->fromp());
        }
    }

    void visit(AstArraySel* nodep) {
        VL_RESTORER(m_lhsp);
        {
            // reverse iteration order
            // UASSERT_OBJ(!m_lhsp.aselp, nodep, "multidimensional unpacked array not supported
            // yet\n");
            m_lhsp.aselp = nodep;
            // iterate the bitp first, i.e., reverse order.
            // This helps with the AstVarRef visitor since we first rename
            // bitp(), and then fromp() which is essentially a left value.
            iterate(nodep->bitp());
            iterate(nodep->fromp());
        }
    }

    void visit(AstNodeAssign* nodep) {
        if (!m_procp) { return; }  // nothing to do
        VL_RESTORER(m_lhsp);
        {
            iterate(nodep->rhsp());
            m_lhsp.lhsp = nodep->lhsp();  // set after iteration, don't move up
            m_lhsp.rhsp = nodep->rhsp();  // set after iteration

            m_lhsp.assignp = nodep;
            iterate(nodep->lhsp());
        }
    }

    void visit(AstScope* nodep) {
        VL_RESTORER(m_scopep);
        {
            m_scopep = nodep;
            m_subst.clear();
            AstNode::user1ClearTree();
            iterateChildren(nodep);
        }
    }
    void visit(AstNodeProcedure* nodep) {
        UINFO(3, "Visiting Always" << nodep);
        VL_RESTORER(m_procp);
        {
            m_procp = nodep;
            m_subst.clear();
            iterateChildren(nodep);
        }
    }

    /// FALLBACK VISITOR
    void visit(AstNode* nodep) { iterateChildren(nodep); }

public:
    explicit SingleAssignmentVisitor(AstNetlist* nodep)
        : m_lvFreshName("__Vlvsa") {
        iterate(nodep);
    }
};
void V3IfConversion::predicatedAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { IfConditionAsAtomVisitor{nodep}; }  // desctory the visitor
    V3Global::dumpCheckGlobalTree("ifcondtion", 0, dumpTree() >= 1);
    { IfConversionVisitor{nodep}; }  // desctory the visitor
    V3Global::dumpCheckGlobalTree("ifconversion", 0, dumpTree() >= 1);
    { SingleAssignmentVisitor{nodep}; }  // destroy the visitor
    V3Global::dumpCheckGlobalTree("singleAssignment", 0, dumpTree() >= 1);
}
