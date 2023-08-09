// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************//
// DESCRIPTION: Verilator BSP: Reduce the size of large copy operations
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2005-2023 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
// Author: Mahyar Emami
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3BspDifferential.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3BspModules.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <set>
#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;
namespace {
// A scratchpad data structure to keep the state of the transformation
struct UnpackUpdate {
    uint32_t numUpdates = 0;
    uint32_t diffCost = 0;
    struct {
        std::vector<AstNode*> recipesp;
        std::vector<AstVarScope*> rvsp;  // ensure unique elements
        AstVarScope* condp = nullptr;
        AstVarScope* condInitp = nullptr;
    } subst;
    AstVarScope* origVscp = nullptr;
    AstClass* const classp = nullptr;
    AstUnpackArrayDType* const dtypep = nullptr;

    UnpackUpdate(AstClass* const clsp, AstUnpackArrayDType* const tp)
        : classp{clsp}
        , dtypep{tp}
        , numUpdates{0}
        , diffCost{0} {}
    operator bool() const { return (classp != nullptr && dtypep != nullptr); }
};

// We have a scratchpad for each written Unpack variable (AstVar* is the var in the producer)
using UnpackUpdateMap = std::unordered_map<AstVar*, UnpackUpdate>;

class DifferentialUnpackVisitor;

/// Simple visitor to check whether it makes sense to turn a blind exchange into
/// one in which only "changes" are propagated.
/// TODO: Estimate the cost of sending the diffs versus sending the whole variable
///
class UnpackWriteAnalysisVisitor final : public VNVisitor {
private:
    UnpackUpdateMap& m_updates;
    bool m_inDynamicBlock = false;
    bool m_inAssign = false;

    // enum {
    //     E_COUNT_WRITES = 0,
    //     E_COUNT_COST = 1,
    // } m_state
    //     = E_COUNT_WRITES;

    void iterateDynamic(AstNode* nodep) {
        VL_RESTORER(m_inDynamicBlock);
        {
            m_inDynamicBlock = true;
            iterateChildren(nodep);
        }
    }
    void visit(AstArraySel* aselp) override {
        // get the base VarRef for this ArraySel
        AstNode* const baseFromp = AstArraySel::baseFromp(aselp, false);
        if (VN_IS(baseFromp, Const)) { return; }
        const AstNodeVarRef* const vrefp = VN_CAST(baseFromp, NodeVarRef);
        UASSERT_OBJ(vrefp, aselp, "No VarRef under ArraySel");
        const bool lvalue = vrefp->access().isWriteOrRW();
        AstVar* const varp = vrefp->varp();
        if (lvalue && m_updates.count(vrefp->varp())) {
            if (m_inDynamicBlock) {
                // we can not accurately count the number of times the variable
                // is updated (e.g., inside a while loop). So we don't consider it for optimization
                UINFO(4, "Will not be optimized: "
                             << varp->prettyNameQ()
                             << ", cannot determine number of updates statically" << endl);
                m_updates.erase(varp);
            } else if (m_inAssign) {
                m_updates.find(varp)->second.numUpdates += 1;
            } else {
                // not in an assignment, perhaps LV but as function argument
                UINFO(4, "Will not be optimized: " << varp->prettyNameQ()
                                                   << ", not in an assignment" << endl);
                m_updates.erase(varp);
            }
        }
    }
    void visit(AstNodeVarRef* vrefp) {
        AstVar* const varp = vrefp->varp();
        if (vrefp->access().isWriteOrRW() && m_updates.count(varp)) {
            // unpack variable is being updated as a whole, cannot do diff exchange
            UINFO(4, "Will not be optimized: " << varp->prettyNameQ()
                                               << ", unpack array updated as a whole" << endl);
            m_updates.erase(varp);
        }
    }

    void visit(AstNodeAssign* assignp) override {
        VL_RESTORER(m_inAssign);
        {
            m_inAssign = true;
            iterateChildren(assignp);
        }
    }
    // Blocks with dynamic behavior
    void visit(AstWhile* whilep) override { iterateDynamic(whilep); }
    void visit(AstDoWhile* whilep) override { iterateDynamic(whilep); }
    void visit(AstJumpBlock* jblockp) override { iterateDynamic(jblockp); }
    void visit(AstForeach* foreachp) override { iterateDynamic(foreachp); }
    void visit(AstRepeat* repeatp) override { iterateDynamic(repeatp); }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

    explicit UnpackWriteAnalysisVisitor(AstClass* classp, UnpackUpdateMap& updateMap)
        : m_updates(updateMap) {
        // m_state = E_COUNT_WRITES;
        iterate(classp);
        // m_state = E_COUNT_COST;
    }

    friend class DifferentialUnpackVisitor;
};

/// Simple visitor to substitute every variable with the one given in user3p.
/// Only used inernally by DifferentialUnpackVisitor
class SubstVisitor final : public VNVisitor {
private:
    void visit(AstVarRef* vrefp) override {

        AstVarScope* const substp = VN_CAST(vrefp->varScopep()->user3p(), VarScope);
        UASSERT_OBJ(substp, vrefp, "no subst for " << vrefp->prettyNameQ() << endl);
        vrefp->name(substp->varp()->name());
        vrefp->varp(substp->varp());
        // UINFO(3, "Set vscp " << substp << endl);
        vrefp->varScopep(substp);
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }
    explicit SubstVisitor(AstNode* nodep) { iterate(nodep); }
    friend class DifferentialUnpackVisitor;
};
/// Main visitor:
/// 1) Finds all the unpack variables that are exchanged.
/// 2) In the source side, it creates a "enCond" variable:
///     CFUNC nbaTop:
///         enCond = 0;
///             ....
///         (the ith update)
///         unpack[x][y][z][u+:b] = v; promote x,y,z,u,v to member variables if not
///         enCond[i] = 1'b1
/// 2) In clsInit::compute:
//          enCondInit = 0;
/// 3) In any consumer of unpack
///     CFUNC nbaTop:
///         if (enCond[i])
///              unpack[x][y][z][u+:b] = v;
///         rest of the code
/// 4) In exchange:
///         delete any target.unpack = source.unpack
///         add        target.x,y,z,u,v = source.x,y,z,u,v
/// 5) In initial exchange:
///         add target.enCond = enCondInit
/// Obviously this could back-fire if unpack < sizeof({x, y, z, u, v})
/// So only do it for larger unpacks (e.g. more than 64 words)

class DifferentialUnpackVisitor final : public VNVisitor {
private:
    V3UniqueNames m_newNames;

    AstClass* m_classp = nullptr;
    AstCFunc* m_funcp = nullptr;
    AstCFunc* m_nbaFuncp = nullptr;
    AstNodeAssign* m_assignp = nullptr;
    AstNetlist* m_netlistp = nullptr;

    AstCFunc* m_initComputep = nullptr;
    AstClass* m_initClassp = nullptr;
    AstCFunc* m_initExchangep = nullptr;
    AstCFunc* m_exchangep = nullptr;

    VDouble0 m_statsNumOpt;
    VDouble0 m_statsNumCandidates;
    // stack of ArraySel nodes above
    std::vector<AstArraySel*> m_aselp;

    enum VisitMode { VU_NONE = 0, VU_WRITER = 1, VU_READER = 2 };

    const VNUser1InUse m_user1InUse;
    const VNUser2InUse m_user2InUse;
    const VNUser3InUse m_user3InUse;

    // STATE:
    // AstClass::user1()         -> needs visiting
    // AstVar::user1()           -> true if is class member var
    // AstNodeAssign::user1()    -> already processed
    // AstVar::user2p()          -> AstVar in the sender
    // AstClass::user2p()        -> AstVarScope that instantiates it in the top module
    // AstVarScope::user3p()     -> substitution AstVarScope, clear on cfuncp

    UnpackUpdateMap m_updates;

    bool marked(AstVar* const varp) { return m_updates.count(varp); }
    bool marked(AstVarScope* const vscp) { return m_updates.count(vscp->varp()); }
    UnpackUpdate& getScratchpad(AstVar* const varp) { return m_updates.find(varp)->second; }
    // Find all unpack variables exchanged between processes

    AstNode* mkCopyOp(AstVarScope* tVscp, AstVarScope* tInstp, AstVarScope* sVscp,
                      AstVarScope* sInstp) {

        auto mkMSel = [](AstVarScope* vscp, AstVarScope* instp, VAccess access) {
            AstMemberSel* const selp = new AstMemberSel{
                vscp->fileline(), new AstVarRef{vscp->fileline(), instp, access},
                VFlagChildDType{}, vscp->varp()->name()};
            selp->varp(vscp->varp());
            selp->dtypeFrom(vscp->varp());
            return selp;
        };

        AstMemberSel* const tSelp = mkMSel(tVscp, tInstp, VAccess::WRITE);
        AstMemberSel* const sSelp = mkMSel(sVscp, sInstp, VAccess::READ);
        AstAssign* const assignp = new AstAssign{tVscp->fileline(), tSelp, sSelp};
        return assignp;
    }
    void addLogicToReader(AstCFunc* cfuncp) {

        AstScope* const scopep = cfuncp->scopep();
        AstNode::user3ClearTree();

        // AstNode::user4ClearTree();
        // // set user4p for every varp received (remote) to point to the local var
        // for (AstVarScope* vscp = scopep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
        //     AstVar* const varp = vscp->varp();
        //     AstVar* const sourcep = varp->user2p();
        //     if (sourcep) { sourcep->user4p(vscp); }
        // }
        // iterate every local vscp and create new variables for the ones that are being
        // made into differentials
        for (AstVarScope* vscp = scopep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
            AstVar* const varp = vscp->varp();
            AstVar* const sourcep = VN_CAST(varp->user2p(), Var);
            UASSERT(sourcep != varp, "can not self exchange");
            if (!sourcep) {
                // not coming from outside
                continue;
            }
            if (!marked(sourcep)) {
                // not about to change
                continue;
            }
            // receives the diffs
            // create the condition bitvector for the update
            auto& scratchpad = getScratchpad(sourcep);
            AstVar* const condClonep = scratchpad.subst.condp->varp()->cloneTree(false);
            condClonep->bspFlag({VBspFlag::MEMBER_INPUT});
            condClonep->lifetime(VLifetime::STATIC);
            scopep->modp()->addStmtsp(condClonep);
            AstVarScope* const condVscp
                = new AstVarScope{condClonep->fileline(), scopep, condClonep};
            scopep->addVarsp(condVscp);

            AstVarScope* const clsInstp = VN_AS(m_classp->user2p(), VarScope);
            AstVarScope* const srcInstp = VN_AS(scratchpad.classp->user2p(), VarScope);
            // in the initialize function, set this.condVscp = init.condVscp
            m_initExchangep->addStmtsp(mkCopyOp(condVscp, clsInstp, scratchpad.subst.condInitp,
                                                VN_AS(m_initClassp->user2p(), VarScope)));
            // but also copy from the writer
            m_exchangep->addStmtsp(mkCopyOp(condVscp, clsInstp, scratchpad.subst.condp, srcInstp));
            /// prepare for clone and subst, set user3p to the new Vscp
            scratchpad.origVscp->user3p(vscp);
            scratchpad.subst.condp->user3p(condVscp);
            for (AstVarScope* const rvSourcep : scratchpad.subst.rvsp) {

                // TODO: we may already have this variable clone here, so optimize it...
                AstVar* const cloneVarp = new AstVar{rvSourcep->fileline(), VVarType::MEMBER,
                                                     m_newNames.get(rvSourcep->varp()->name()),
                                                     rvSourcep->varp()->dtypep()};
                cloneVarp->bspFlag({VBspFlag::MEMBER_INPUT});
                cloneVarp->lifetime(VLifetime::STATIC);
                scopep->modp()->addStmtsp(cloneVarp);
                AstVarScope* const cloneVscp
                    = new AstVarScope{rvSourcep->fileline(), scopep, cloneVarp};
                scopep->addVarsp(cloneVscp);
                // UINFO(3, "log subst \n" << rvSourcep << "\n=>\n" << cloneVscp << endl);
                rvSourcep->user3p(cloneVscp);

                // create the copy operation from the source to here
                m_exchangep->addStmtsp(mkCopyOp(cloneVscp, clsInstp, rvSourcep, srcInstp));
            }

            AstNode* const preUpdatep = new AstComment{vscp->fileline(), "pre-update"};
            for (int ix = 0; ix < scratchpad.subst.recipesp.size(); ix++) {
                AstNode* stmtp = scratchpad.subst.recipesp[ix];
                AstSel* const guardp
                    = new AstSel{scratchpad.subst.condp->fileline(),
                                 new AstVarRef{scratchpad.subst.condp->fileline(),
                                               scratchpad.subst.condp, VAccess::READ},
                                 ix, 1};
                AstIf* const ifp
                    = new AstIf{stmtp->fileline(), guardp, stmtp->cloneTree(false), nullptr};
                preUpdatep->addNext(ifp);
            }
            // if (debug() >= 5) { preUpdatep->dumpTree("-     subst this\n"); }
            // unlink and link again, this is a hack to make the SubstVisitor work
            AstNode* existingp = cfuncp->stmtsp()->unlinkFrBackWithNext();
            cfuncp->addStmtsp(preUpdatep);
            {
                SubstVisitor{cfuncp};
            }  // need to pass the func to the visitor, preUpdatep won't work
            if (existingp) { cfuncp->addStmtsp(existingp); }
        }
    }

    void visit(AstModule* modp) override {
        // accelerate, we dont need to go inside the module (which is the top)
    }

    void visit(AstClass* classp) override {

        if (!classp->user1()) {
            // not marked to be visited
            UINFO(5, "Will not visit " << classp->prettyNameQ() << endl);
            return;  // do not need to visit
        }
        UASSERT_OBJ(classp->flag().isBsp() && !classp->flag().isBspInit()
                        && !classp->flag().isBspCond(),
                    classp, "should not visit");
        UASSERT_OBJ(!m_classp, classp, "should not nest classes");
        VL_RESTORER(m_classp);
        if (classp->user1() == VU_WRITER) {
            UINFO(4, "Visiting writer class " << classp->prettyNameQ() << endl);
            m_classp = classp;
            // count the times each variable is updated, if cannot determine the count statically,
            // remove the unpack variable from the scratchpad
            { UnpackWriteAnalysisVisitor{classp, m_updates}; }
            // mark the member variables, so that we know we do not need to promote them
            // Perhpas we could also check AstVar::isFuncLocal() instead?
            for (AstNode* nodep = classp->stmtsp(); nodep; nodep = nodep->nextp()) {
                if (AstVar* const varp = VN_CAST(nodep, Var)) { varp->user1(true); }
            }
            // iterate children and create new variables
            iterateChildren(classp);
        } else if (classp->user1() == VU_READER) {
            UINFO(4, "visiting reader class " << classp->prettyNameQ() << endl);
            m_classp = classp;
            classp->foreach([this](AstCFunc* cfuncp) {
                if (cfuncp->name() == "nbaTop") { addLogicToReader(cfuncp); }
            });
        }
    }

    void visit(AstCFunc* cfuncp) override {
        if (cfuncp->user1()) return;  // already visited
        VL_RESTORER(m_funcp);
        VL_RESTORER(m_nbaFuncp);
        UASSERT_OBJ(m_classp, cfuncp, "not under class");
        UASSERT_OBJ(m_classp->user1() == VU_WRITER, cfuncp, "should not be here as the reader");
        {
            m_funcp = cfuncp;
            cfuncp->user1(true);
            if (cfuncp->name() == "nbaTop" && m_classp) { m_nbaFuncp = cfuncp; }
            iterateChildren(cfuncp);
        }
    }

    void visit(AstArraySel* aselp) override {
        // get the base VarRef for this ArraySel
        AstNode* const baseFromp = AstArraySel::baseFromp(aselp, false);
        if (VN_IS(baseFromp, Const)) { return; }
        const AstNodeVarRef* const vrefp = VN_CAST(baseFromp, NodeVarRef);
        UASSERT_OBJ(vrefp, aselp, "No VarRef under ArraySel");

        const bool lvalue = vrefp->access().isWriteOrRW();

        if (!lvalue || !marked(vrefp->varp())) {
            return;  // we don't care about this ArraySel, does not need optimization or cannot be
                     // optimized
        }
        UnpackUpdate& scratchpad = getScratchpad(vrefp->varp());
        UASSERT_OBJ(scratchpad.numUpdates, vrefp, "no write observed!");
        // create the write condition
        if (!scratchpad.subst.condp) {
            m_statsNumOpt += 1;
            AstNodeDType* condDTypep = m_netlistp->findBitDType(
                static_cast<int>(scratchpad.numUpdates), static_cast<int>(scratchpad.numUpdates),
                VSigning::UNSIGNED);

            AstVar* const condVarp = new AstVar{vrefp->fileline(), VVarType::MEMBER,
                                                m_newNames.get("en"), condDTypep};
            condVarp->lifetime(VLifetime::STATIC);
            condVarp->bspFlag({VBspFlag::MEMBER_OUTPUT});
            condVarp->user1(true);
            AstVarScope* const condVscp
                = new AstVarScope{vrefp->fileline(), vrefp->varScopep()->scopep(), condVarp};
            condVscp->scopep()->addVarsp(condVscp);
            condVscp->scopep()->modp()->addStmtsp(condVarp);
            scratchpad.subst.condp = condVscp;

            // set it to zero before anything else runs
            UASSERT_OBJ(m_nbaFuncp, aselp, "not under nbaTop");
            AstAssign* const assignClearp = new AstAssign{
                vrefp->fileline(), new AstVarRef{vrefp->fileline(), condVscp, VAccess::WRITE},
                new AstConst{vrefp->fileline(), AstConst::WidthedValue{}, condDTypep->width(), 0}};
            if (m_nbaFuncp->stmtsp()) {
                m_nbaFuncp->stmtsp()->addHereThisAsNext(assignClearp);
            } else {
                m_nbaFuncp->addStmtsp(assignClearp);
            }

            // initialize it to zero in the initial class, this is used to ensure no write takes
            // place in the receiver before actually sending from here
            AstVar* const condVarInitp = condVarp->cloneTree(false);
            condVarInitp->lifetime(VLifetime::STATIC);
            condVarInitp->bspFlag({VBspFlag::MEMBER_OUTPUT});
            AstVarScope* const condInitVscp
                = new AstVarScope{vrefp->fileline(), m_initComputep->scopep(), condVarInitp};
            m_initComputep->scopep()->addVarsp(condInitVscp);
            m_initComputep->scopep()->modp()->addStmtsp(condVarInitp);
            scratchpad.subst.condInitp = condInitVscp;
            AstAssign* const initClearp = new AstAssign{
                vrefp->fileline(), new AstVarRef{vrefp->fileline(), condInitVscp, VAccess::WRITE},
                new AstConst{vrefp->fileline(), AstConst::WidthedValue{}, condDTypep->width(), 0}};
            if (m_initComputep->stmtsp()) {
                m_initComputep->stmtsp()->addHereThisAsNext(initClearp);
            } else {
                m_initComputep->addStmtsp(initClearp);
            }
        }
        AstNodeAssign* const parentAssignp = [aselp]() {
            // find the parent statement (should be NodeAssign)
            AstNode* parentp = aselp;
            while (!VN_IS(parentp, NodeStmt) && parentp) { parentp = parentp->backp(); }
            UASSERT_OBJ(parentp, aselp, "no parent stmt");
            return VN_AS(parentp, NodeAssign);
        }();
        if (parentAssignp->user1()) {
            return; // already processed
        }
        parentAssignp->user1(true);
        // if rhs is not simple varref, make it. Pathologically the rhs could be ArraySel itself,
        // so we may end up copying the whole array on the rhs if we don't "capture" its selection here.
        if (!VN_IS(parentAssignp->rhsp(), VarRef)) {
            UINFO(4, "Making rhs of assign a varref " << parentAssignp << endl);
            AstVar* rhsVarp = new AstVar{parentAssignp->rhsp()->fileline(), VVarType::MEMBER,
                                         m_newNames.get(parentAssignp->rhsp()),
                                         parentAssignp->rhsp()->dtypep()};
            rhsVarp->bspFlag({VBspFlag::MEMBER_OUTPUT, VBspFlag::MEMBER_LOCAL});
            rhsVarp->lifetime(VLifetime::STATIC);
            rhsVarp->user1(true);  // is a member
            vrefp->varScopep()->scopep()->modp()->addStmtsp(rhsVarp);
            AstVarScope* const rhsVscp
                = new AstVarScope{rhsVarp->fileline(), vrefp->varScopep()->scopep(), rhsVarp};
            vrefp->varScopep()->scopep()->addVarsp(rhsVscp);
            AstAssign* const rhsAssignp
                = new AstAssign{parentAssignp->fileline(),
                                new AstVarRef{rhsVscp->fileline(), rhsVscp, VAccess::WRITE},
                                parentAssignp->rhsp()->unlinkFrBack()};
            parentAssignp->addHereThisAsNext(rhsAssignp);
            parentAssignp->rhsp(new AstVarRef{rhsVarp->fileline(), rhsVscp, VAccess::READ});
        }
        // insert
        // condVcsp[ith] = 1'b1;
        // right after parentp
        AstAssign* const condAssignp = new AstAssign{
            vrefp->fileline(),
            new AstSel{vrefp->fileline(),
                       new AstVarRef{vrefp->fileline(), scratchpad.subst.condp, VAccess::WRITE},
                       static_cast<int>(scratchpad.subst.recipesp.size()), 1},
            new AstConst{vrefp->fileline(), AstConst::WidthedValue{}, 1, 1}};

        parentAssignp->addNextHere(condAssignp);

        scratchpad.subst.recipesp.push_back(parentAssignp);
        // any VarRef under parentp that is an RV should be captured as a class member
        scratchpad.origVscp = vrefp->varScopep();
        parentAssignp->foreach([&](AstNodeVarRef* rvp) {
            if (rvp->access().isWriteOrRW() && rvp != vrefp) {
                parentAssignp->v3fatal("Multiple LVs " << rvp->varp()->prettyNameQ() << " and "
                                                       << vrefp->varp()->prettyNameQ() << endl);
            } else if (rvp->access().isReadOrRW() && rvp != vrefp) {
                if (!rvp->varp()->user1()) {  // not a class member, should be made one
                    UINFO(3, "Promoting " << rvp->varp()->prettyNameQ() << " to member " << endl);
                    rvp->varp()->user1(true);
                    vrefp->varScopep()->scopep()->modp()->addStmtsp(rvp->varp()->unlinkFrBack());
                }
                scratchpad.subst.rvsp.push_back(rvp->varScopep());
            }
        });
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit DifferentialUnpackVisitor(AstNetlist* netlistp)
        : m_newNames{"__Vbspdiff"}
        , m_netlistp{netlistp} {

        AstNode::user1ClearTree();
        AstNode::user2ClearTree();
        AstNode::user3ClearTree();

        // find the compute method in the init class
        for (AstNode* nodep = netlistp->modulesp(); nodep; nodep = nodep->nextp()) {
            AstClass* const classp = VN_CAST(nodep, Class);
            if (classp && classp->flag().isBspInit()) {
                classp->foreach([&](AstCFunc* funcp) {
                    if (funcp->name() == "compute") {
                        m_initComputep = funcp;
                        m_initClassp = classp;
                    }
                });
            }
        }

        // set userp2 of every AstClass to point to its unique instance at the top scope
        for (AstVarScope* vscp = netlistp->topScopep()->scopep()->varsp(); vscp;
             vscp = VN_AS(vscp->nextp(), VarScope)) {
            AstClassRefDType* const dtypep = VN_CAST(vscp->varp()->dtypep(), ClassRefDType);
            if (dtypep && dtypep->classp()->flag().isBsp()) { dtypep->classp()->user2p(vscp); }
        }

        // find the exchange and initialize(Exchange) functions, will modify them later
        // incrementally
        for (AstNode *nodep = netlistp->topScopep()->scopep()->blocksp(), *nextp; nodep;
             nodep = nodep->nextp()) {
            AstCFunc* funcp = VN_CAST(nodep, CFunc);
            if (funcp && funcp->name() == "exchange") {
                m_exchangep = funcp;
            } else if (funcp && funcp->name() == "initialize") {
                m_initExchangep = funcp;
            }
        }

        UASSERT(m_exchangep, "could not find exchange");
        UASSERT(m_initExchangep, "could not find initialize");
        UASSERT(m_initComputep, "could not find initial class");
        UASSERT(m_initClassp, "init class not found");

        auto foreachCopyp = [this](auto&& fn) {
            for (AstAssign *copyp = VN_AS(m_exchangep->stmtsp(), Assign), *nextp; copyp;
                 copyp = nextp) {
                nextp = VN_AS(copyp->nextp(), Assign);
                fn(copyp);
            }
        };

        auto getClass = [](AstMemberSel* mselp) {
            return VN_AS(VN_AS(mselp->fromp(), VarRef)->varp()->dtypep(), ClassRefDType)->classp();
        };

        // Visit every copy operation in "exchange", mark larger ones to be analyzed
        foreachCopyp([&](AstAssign* copyp) {
            // targetClassp.targetVarp  = sourceClassp.sourceVarp
            AstMemberSel* const sourcep = VN_AS(copyp->rhsp(), MemberSel);
            AstUnpackArrayDType* const unpackDTypep = VN_CAST(sourcep->dtypep(), UnpackArrayDType);
            if (!unpackDTypep) {
                // not our concern
                return;
            }
            auto numWords = unpackDTypep->arrayUnpackedElements() * unpackDTypep->widthWords();
            if (numWords < v3Global.opt.diffExchangeThreshold()) {
                UINFO(4, "Will not optimize unpack array "
                             << unpackDTypep << " with " << numWords
                             << " words which is smaller than --diff-exchange-threshold "
                             << v3Global.opt.diffExchangeThreshold() << endl);
                return;
            }

            AstClass* const classp = getClass(sourcep);
            AstVar* const unpackVarp = sourcep->varp();
            if (!marked(unpackVarp)) {
                // emplace it in the scratchpad to be transformed
                m_updates.emplace(unpackVarp, UnpackUpdate{classp, unpackDTypep});
                m_statsNumCandidates += 1;
                classp->user1(VU_WRITER);  // mark this class as being a writer
            }
        });
        // iterate any class marked as VU_WRITER, analyze it to ensure we can determine the number
        // of writes to the selected unpack variables statically and then sample write conditions
        // for potential readers
        iterateChildren(netlistp);

        // Go through all the copy operations again, clear the writer classes and mark
        // the readers (a write may also be reader of some other variable, or some variable of its
        // own).
        foreachCopyp([&](AstAssign* copyp) {
            AstMemberSel* const sourcep = VN_AS(copyp->rhsp(), MemberSel);
            AstMemberSel* const targetp = VN_AS(copyp->lhsp(), MemberSel);
            AstClass* const sourceClassp = getClass(sourcep);
            AstClass* const targetClassp = getClass(targetp);
            // map back to the source variable
            targetp->varp()->user2p(sourcep->varp());
            if (sourcep->user1() == VU_WRITER) {
                sourceClassp->user1(VU_NONE);  // unmark
            }
            if (!marked(sourcep->varp())) { return; }

            UINFO(4, "class " << targetClassp->prettyNameQ() << " is a reader for "
                              << sourcep->varp()->prettyNameQ() << endl);
            targetClassp->user1(VU_READER);  // mark
            // delete the operation, will be
            VL_DO_DANGLING(copyp->unlinkFrBack()->deleteTree(), copyp);
        });
        // Iterate the VU_READER classes and add the differential logic
        iterateChildren(netlistp);
    }

    ~DifferentialUnpackVisitor() override {
        V3Stats::addStat("Optimizations, ipu differential exchanges applied", m_statsNumOpt);
        V3Stats::addStat("Optimizations, ipu differential exchange candidates",
                         m_statsNumCandidates);
    }
};
}  // namespace
void V3BspDifferential::differentialUnpack(AstNetlist* netlistp) {

    UINFO(3, "Optimizing exchange" << endl);

    { DifferentialUnpackVisitor{netlistp}; }

    v3Global.dumpCheckGlobalTree("bspdiff", 0, dumpTree() >= 3);
}

uint32_t V3BspDifferential::countWords(AstNodeDType* const dtypep) {
    if (AstUnpackArrayDType* const unpackp = VN_CAST(dtypep, UnpackArrayDType)) {
        auto numWords = unpackp->arrayUnpackedElements() * unpackp->widthWords();
        if (numWords >= v3Global.opt.diffExchangeThreshold()) {
            // probably can optimize, therefore the cost is "estimated" to be the
            // cost of sending an address, the data, and a bit.
            // We can multiply it by the number of times we expect an unpack array
            // but we expect the number of writes to be 1 (single-port memory)
            return 2 + unpackp->widthWords();
        } else {
            return numWords;
        }
    } else {
        return dtypep->widthWords();
    }
}