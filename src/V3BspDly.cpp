// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Turn Assign into AssignDly in clocked blocks
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
// Turns some blocking assignment into non-blocking ones to increase parallelism
// for BSP.
// ACTIVE clock
//      AstAssing x expr1;
//      AstIf
//         AstNodeExpr cond
//         AstAssign x x+1
// becomes
// ACTIVE clock
//      AstAssign x_0  x
//      AstAssign x_0  expr1;
//      AstIf cond
//          AstAssign x_0 x_0+1
//      AstAssignDly x x_0
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3BspDly.h"

#include "V3Ast.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <map>
#include <set>

VL_DEFINE_DEBUG_FUNCTIONS;

class BspDlyInsertVisitor final : public VNVisitor {
private:
    // NODE STATE
    //  clear on scope
    //  AstVarScope::user1()     -> number of always blocks that write to this variable
    //  AstVarScope::user2p()    -> last always block pointer
    VNUser1InUse user1InUse;
    VNUser2InUse user2InUse;

    V3UniqueNames m_lvNames;
    AstScope* m_scope = nullptr;
    bool m_inClocked = false;

    void visit(AstActive* nodep) override {
        m_inClocked = nodep->hasClocked();
        iterateChildren(nodep);
    }
    void visit(AstAlways* nodep) override {
        UASSERT_OBJ(m_scope, nodep, "No scope!");
        // collect all the non-AstAssignDly assignments
        if (!m_inClocked) { return; }  // nothing to do
        // use map to preserve order, performance might be slightly worse but
        // the Always block is supposed to be very small anyways
        std::map<AstVarScope*, AstVarScope*> blockingVscp;
        // collect the LHS of all (blocking) assignments, and replace them
        // on the LHS side, note that we can not replace them on the RHS
        nodep->foreach([&](AstAssign* assignp) {
            assignp->foreach([&](const AstVarRef* vrefp) {
                if (vrefp->access().isWriteOrRW() /*is lhs*/
                    && VN_IS(vrefp->varScopep()->dtypep(), BasicDType) /*is not memory*/
                    && vrefp->varp()->lifetime() == VLifetime::STATIC
                    && vrefp->varp()->varType() == VVarType::VAR /*is not temp*/) {
                    // create a temp variable
                    if (vrefp->varScopep()->user1()
                        > 1) {  // don't promote to delayed assignment if it has multiple drivers
                        vrefp->v3warn(MULTIDRIVEN, "Variable may have multiple drivers: "
                                                       << vrefp->varScopep()->user1() << endl);
                    } else if (blockingVscp.find(vrefp->varScopep()) == blockingVscp.end()) {
                        // create a temp variable
                        AstVarScope* const subst = m_scope->createTempLike(
                            m_lvNames.get(vrefp->name()), vrefp->varScopep());
                        UINFO(4, "register subst " << vrefp->varScopep()->name() << " -> "
                                                   << subst->name() << endl);
                        blockingVscp.emplace(vrefp->varScopep(), subst);
                    }
                }
            });
        });
        // now substitute all the references: lhs and rhs
        nodep->foreach([&](AstVarRef* vrefp) {
            auto it = blockingVscp.find(vrefp->varScopep());
            if (it != blockingVscp.end()) {
                AstVarScope* const subst = it->second;
                AstVarRef* newp = new AstVarRef{vrefp->fileline(), subst, vrefp->access()};
                vrefp->replaceWith(newp);
                UINFO(4, "replacing " << vrefp->name() << endl);
                pushDeletep(vrefp);
            }
        });

        // for every (old, new) pair in blockingVscp we need to add
        // AstAssign newp = oldp
        // to the start of the always block and
        // AstAssignDly oldp <= newp
        // to the end of the always block
        // Note that AstAssign new = old is necessary because we essetially limited
        // the liveness scope of new to the scope this always block. That is,
        // as if newp is just a wire so that it can be treated like comb logic later
        // by the BSP parallelization pass
        // AstAssignDly is also necessary because we have not changed anything outside
        // of the scope of this clocked block. Therefore there are still references to
        // oldp, in AstAssignW or other clocked/comb blocks. If these references
        // are on the LHS, then the behavior is essentially racy from the source and
        // we are picking one possible behavior out of many. If all the references
        // are on the RHS, the everything is just fine with the addition of the
        // non-blocking assignment at the end

        // handle AstAssign new = old
        for (const auto& substPair :
             vlstd::reverse_view(blockingVscp) /*reverse view not really necessary*/) {
            FileLine* flp = substPair.first->fileline();
            nodep->stmtsp()->addHereThisAsNext(
                new AstAssign{flp, new AstVarRef{flp, substPair.second /*newp*/, VAccess::WRITE},
                              new AstVarRef{flp, substPair.first /*oldp*/, VAccess::READ}});
        }

        // handle AstAssignDly old <= new
        for (const auto& substPair : blockingVscp) {

            FileLine* flp = substPair.second->fileline();
            nodep->addStmtsp(
                new AstAssignDly{flp, new AstVarRef{flp, substPair.first, VAccess::WRITE},
                                 new AstVarRef{flp, substPair.second, VAccess::READ}}

            );
        }
    }
    void visit(AstScope* nodep) override {
        VL_RESTORER(m_scope);
        m_scope = nodep;
        m_lvNames.reset();
        AstNode::user1ClearTree();
        AstNode::user2ClearTree();
        // mark the variables with the number of always blocks that modify them
        nodep->foreach([](AstAlways* alwaysp) {
            alwaysp->foreach([alwaysp](AstVarRef* vrefp) {
                if (vrefp->access().isWriteOrRW() && vrefp->varScopep()->user2p() != alwaysp) {
                    vrefp->varScopep()->user1(vrefp->varScopep()->user1() + 1);
                    vrefp->varScopep()->user2p(alwaysp);
                }
            });
        });

        iterateChildren(nodep);
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit BspDlyInsertVisitor(AstNetlist* nodep)
        : m_lvNames("__VlbspLv") {
        iterate(nodep);
    }
};

void V3BspDly::mkDlys(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { BspDlyInsertVisitor{nodep}; /*destroy on de-scope */ }
    V3Global::dumpCheckGlobalTree("bspdly", 0, dumpTree() >= 1);
}
