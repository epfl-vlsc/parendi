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
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <algorithm>
#include <map>
#include <stack>
VL_DEFINE_DEBUG_FUNCTIONS;

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

class IfConversionVisitor final : public VNVisitor {
private:
    // STATE
    // Cleared on module
    // AstAssign::user1() -> bool. Set true if already processed

    const VNUser1InUse m_inuser1;

    AstActive* m_activep = nullptr;  // curret activate block

    bool m_inIf = false;
    bool m_inLoop = false;  // In a loop, do not convert the ifs
    bool m_inInitial = false;  // in initial block;
    bool m_inDly = false;  // inside a delayed assignment
    AstNodeModule* m_modp = nullptr;  // current module
    AstScope* m_scopep = nullptr;  // current scope
    using VarMap = std::map<const std::pair<AstNodeModule*, std::string>, AstVar*>;
    VarMap m_modVarMap;  // table of new variable names created under module
    VDouble0 m_statsRemoved;  // number of removed ifs, for statistics tracking
    std::unordered_map<const AstVarScope*, int> m_scopeVecMap;  //
    AstIf* m_ifp = nullptr;  // last if node
    std::vector<AstNodeAssign*>
        m_assignCollection;  // collection of assignment in either thensp or elsesp
    // std::vector<AstAssignDly*> m_assignDlyCollections; // like the one above, but for delayed
    // assignments (i.e., lhs <= rhs)
    std::vector<AstNode*> m_hoisted;
    V3UniqueNames m_lvFreshName;  // For generating fresh names for temprary variable names

    // VISITORS
    void visit(AstNetlist* nodep) override {
        // in a new module, so clear state up
        m_modVarMap.clear();
        iterateChildren(nodep);
    }

    void visit(AstActive* nodep) override {
        m_activep = nodep;
        VL_RESTORER(m_inInitial);
        {
            AstSenTree* const senTreep = nodep->sensesp();
            m_inInitial = senTreep->hasStatic() || senTreep->hasInitial();
            iterateChildren(nodep);
        }
    }

    AstNodeAssign* mkWithRhsp(AstNodeAssign* nodep, AstNodeExpr* rhsp) {
        return nodep->cloneType(nodep->lhsp()->cloneTree(true), rhsp);
    }

    void visit(AstNodeAssign* assignp) override {
        if (m_ifp) {
            UINFO(20, assignp << endl);
            m_assignCollection.push_back(assignp);
        }
    }

    void visit(AstIf* ifp) override {
        VL_RESTORER(m_ifp);
        {
            m_ifp = ifp;
            UINFO(10, "(got if) " << ifp << endl);
            VL_RESTORER(m_assignCollection);
            UINFO(10, "Visiting THEN" << endl);
            m_assignCollection.clear();  // clear the stack of assignments
            iterateAndNextNull(ifp->thensp());
            AstBegin* assignBlockp = nullptr;
            AstNode* nextp = ifp;
            for (AstNodeAssign* assignp : m_assignCollection) {
                FileLine* fl = assignp->fileline();
                AstNodeExpr* defaultp = assignp->lhsp()->cloneTree(true);
                { AccessModifierVisitor{defaultp, VAccess::READ}; }

                AstNodeExpr* rhsp = assignp->rhsp()->cloneTree(true);
                AstCond* ternaryp = new AstCond{fl, ifp->condp()->cloneTree(true),
                                                assignp->rhsp()->cloneTree(true), defaultp};
                AstNodeExpr* lhsp = assignp->lhsp()->cloneTree(true);
                AstNodeAssign* newAssignp = assignp->cloneType(lhsp, ternaryp);

                // newAssignp->user1(true);
                // AstNode* b = ifp->backp()
                // AstNode* n = ifp->isUnlinkFrBackWithNext();
                // append the ternary assignment right after the if-then-else block

                nextp->addNextHere(newAssignp);
                nextp = nextp->nextp();
                // remove the original assignment inside the if
                UINFO(20, "Unlinking " << assignp << endl);
                assignp->unlinkFrBack();

                VL_DO_DANGLING(pushDeletep(assignp), assignp);

                // assignp
            }
        }
        {
            VL_RESTORER(m_assignCollection);
            m_ifp = ifp;
            UINFO(10, "Visiting ELSE" << endl);
            // iterateAndNextNull(ifp->elsesp());
        }
    }

    void visit(AstModule* nodep) override {
        UINFO(4, "MOD  " << nodep << endl);
        VL_RESTORER(m_modp);
        {
            m_modp = nodep;
            m_lvFreshName.reset();
            AstNode::user1ClearTree();
            iterateChildren(nodep);
        }
    }
    void visit(AstScope* nodep) override {
        VL_RESTORER(m_scopep);
        {
            m_scopep = nodep;
            iterateChildren(nodep);
        }
    }
    //-------------------
    // Fallback
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit IfConversionVisitor(AstNetlist* nodep)
        : m_lvFreshName("__Vlvif") {
        iterate(nodep);
    }
    ~IfConversionVisitor() override { V3Stats::addStat("If removal, number of ifs", 0); }
};

void V3IfConversion::predicatedAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { IfConversionVisitor{nodep}; }  // desctory the visitor
    V3Global::dumpCheckGlobalTree("ifconversion", 0, dumpTree() >= 1);
}
