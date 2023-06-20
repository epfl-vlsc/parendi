// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator BSP: handle BSP class with DPI calls
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
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3BspIpuProfile.h"

#include "V3Global.h"
#include "V3InstrCount.h"
#include "V3UniqueNames.h"

VL_DEFINE_DEBUG_FUNCTIONS;

class CycleCountInstrumentationVisitor : public VNVisitor {
private:
    // STATE
    // AstClass::user1p() -> AstVarScope instance in the top module
    // AstClass::user2p() -> AstVarScope of the profile trace object
    // AstClass::user3()  -> instruction count estimate
    VNUser1InUse m_user1InUse;
    VNUser2InUse m_user2InUse;
    VNUser3InUse m_user3InUse;

    V3UniqueNames m_newNames;
    AstClass* m_classp = nullptr;
    AstScope* m_scopep = nullptr;

    AstBasicDType* m_cycleDtp = nullptr;
    AstBasicDType* m_profileDtp = nullptr;
    AstBasicDType* m_profileVecDtp = nullptr;

    void visit(AstCFunc* cfuncp) {
        if (!m_classp || cfuncp->name() != "compute") { return; }
        if (!v3Global.opt.ipuProfile()) { return; }
        /*  create the following code
            AstFunc compute
                Var start;
                start.time()
                ...
                Var end;
                end.time();
                prof.push(end -start);
        */
        // first create a member variable that holds the profile trace
        AstVar* const profVarp = new AstVar{cfuncp->fileline(), VVarType::MEMBER,
                                            m_newNames.get("prof"), m_profileDtp};
        profVarp->bspFlag({VBspFlag::MEMBER_HOSTREAD, VBspFlag::MEMBER_HOSTWRITE});
        profVarp->lifetime(VLifetime::STATIC);
        AstVarScope* const profVscp = new AstVarScope{cfuncp->fileline(), m_scopep, profVarp};
        m_scopep->addVarsp(profVscp);
        m_classp->stmtsp()->addHereThisAsNext(profVarp);

        // We also need two function local variables to measure the start and end times
        auto mkCycleVar = [this, &cfuncp](const std::string& name) {
            AstVar* const varp = new AstVar{cfuncp->fileline(), VVarType::MEMBER,
                                            m_newNames.get(name), m_cycleDtp};
            varp->funcLocal(true);
            varp->lifetime(VLifetime::AUTOMATIC);
            // don't add to the function now
            AstVarScope* const vscp = new AstVarScope{cfuncp->fileline(), m_scopep, varp};
            m_scopep->addVarsp(vscp);
            return vscp;
        };
        auto mkCall = [this](AstVarScope* const vscp, const std::string& name,
                             const std::vector<AstNodeExpr*> argsp) {
            AstCMethodHard* const callp = new AstCMethodHard{
                vscp->fileline(), new AstVarRef{vscp->fileline(), vscp, VAccess::WRITE}, name,
                nullptr};
            callp->dtypeSetVoid();
            callp->pure(false);
            for (const auto& argp : argsp) { callp->addPinsp(argp); }
            AstStmtExpr* const stmtp = callp->makeStmt();
            return stmtp;
        };
        AstVarScope* const startVscp = mkCycleVar("start");
        // capture start cycle
        cfuncp->stmtsp()->addHereThisAsNext(mkCall(startVscp, "time", {}));
        cfuncp->stmtsp()->addHereThisAsNext(startVscp->varp());
        AstVarScope* const endVscp = mkCycleVar("end");
        cfuncp->addStmtsp(endVscp->varp());
        cfuncp->addStmtsp(mkCall(endVscp, "time", {}));
        cfuncp->addStmtsp(
            mkCall(profVscp, "log",
                   {
                       new AstVarRef{startVscp->fileline(), startVscp, VAccess::READ},
                       new AstVarRef{endVscp->fileline(), endVscp, VAccess::READ},
                   }));
        m_classp->user2p(profVscp);
        m_classp->user3(V3InstrCount::count(cfuncp, true));
    }

    void visit(AstClass* classp) {
        if (!classp->flag().isBsp()) { return; }
        UASSERT_OBJ(classp->user1p(), classp, "class has no instance!");
        m_classp = classp;
        iterateChildren(classp);
    }

    void visit(AstScope* scopep) {
        if (m_classp) {
            m_scopep = scopep;
            iterateChildren(scopep);
            m_scopep = nullptr;
        }
    }

    void visit(AstNode* nodep) { iterateChildren(nodep); }

public:
    explicit CycleCountInstrumentationVisitor(AstNetlist* netlistp)
        : m_newNames{VL_UNIQUENAMES("")} {
        if (v3Global.opt.ipuProfile()) {
            m_cycleDtp = new AstBasicDType{netlistp->fileline(), VBasicDTypeKwd::IPU_CYCLE,
                                           VSigning::UNSIGNED, 2 * VL_EDATASIZE, 2 * VL_EDATASIZE};
            // clang-format off
            int profileDataStructureSize
                = v3Global.opt.ipuProfile() * VL_EDATASIZE /*std::array<IData, SIZE>*/ +
                  2 * VL_EDATASIZE /*uint64_t m_total*/ +
                  VL_EDATASIZE  /*uint32_t m_count*/ +
                  VL_EDATASIZE; /*uint32_t m_head*/
            // clang-format on
            m_profileDtp = new AstBasicDType{netlistp->fileline(),
                                             VBasicDTypeKwd::IPU_PROFILE_TRACE, VSigning::UNSIGNED,
                                             profileDataStructureSize, profileDataStructureSize};
        }
        m_profileVecDtp = new AstBasicDType{
            netlistp->fileline(), VBasicDTypeKwd::IPU_PROFILE_TRACE_VEC, VSigning::UNSIGNED, 1, 1};
        netlistp->typeTablep()->addTypesp(m_cycleDtp);
        netlistp->typeTablep()->addTypesp(m_profileDtp);
        netlistp->typeTablep()->addTypesp(m_profileVecDtp);
        // add a "profileTrace method to the top module"
        AstCFunc* const profTraceFuncp
            = new AstCFunc{netlistp->fileline(), "profileTrace", netlistp->topScopep()->scopep()};
        profTraceFuncp->dontCombine(true);
        profTraceFuncp->isInline(false);
        profTraceFuncp->slow(true);
        profTraceFuncp->rtnType(m_profileVecDtp->cType("", false, false));
        netlistp->topScopep()->scopep()->addBlocksp(profTraceFuncp);

        AstCFunc* const profInitFuncp = new AstCFunc{netlistp->fileline(), "profileInit",
                                                     netlistp->topScopep()->scopep(), "void"};
        profInitFuncp->dontCombine(true);
        netlistp->topScopep()->scopep()->addBlocksp(profInitFuncp);

        // create a functional local a variable that accumulates all the traces
        AstVar* const profVecVarp = new AstVar{netlistp->fileline(), VVarType::BLOCKTEMP,
                                               m_newNames.get("vec"), m_profileVecDtp};
        profVecVarp->funcLocal(true);
        profVecVarp->funcReturn(true);
        profTraceFuncp->addStmtsp(profVecVarp);

        AstVarScope* profVecVscp
            = new AstVarScope{netlistp->fileline(), profTraceFuncp->scopep(), profVecVarp};
        profTraceFuncp->scopep()->addVarsp(profVecVscp);
        AstCReturn* const returnp
            = new AstCReturn{profVecVarp->fileline(),
                             new AstVarRef{profVecVarp->fileline(), profVecVscp, VAccess::READ}};
        if (v3Global.opt.ipuProfile() == 0) {
            profTraceFuncp->addStmtsp(returnp);
            return;
        }
        // iterate through the top scope and set user1p
        AstNode::user1ClearTree();
        AstNode::user2ClearTree();
        AstNode::user3ClearTree();
        for (AstVarScope* vscp = netlistp->topScopep()->scopep()->varsp(); vscp;
             vscp = VN_AS(vscp->nextp(), VarScope)) {
            const AstClassRefDType* const classTypep = VN_CAST(vscp->dtypep(), ClassRefDType);
            if (classTypep && classTypep->classp()->flag().isBsp()) {
                classTypep->classp()->user1p(vscp);

                AstClass* const classp = classTypep->classp();
                // visit class and optionally append profile counters
                m_newNames.reset();
                visit(classp);
                UASSERT_OBJ(classp->user2p(), classp, "epxected user2p()");
                // user2p now contains the AstVarScope that is the profile trace
                AstVarScope* const profVscp = VN_AS(classp->user2p(), VarScope);
                // need to insert the following code:
                //     CMethodHard
                //         profVecVarp.append(classInst.provVarp, "name", estimation);
                AstMemberSel* const memselp = new AstMemberSel{
                    profVscp->fileline(), new AstVarRef{vscp->fileline(), vscp, VAccess::READ},
                    VFlagChildDType{}, profVscp->varp()->name()};
                memselp->varp(profVscp->varp());
                memselp->dtypeFrom(profVscp->varp());
                AstCMethodHard* const callp = new AstCMethodHard{
                    profVscp->fileline(),
                    new AstVarRef{vscp->fileline(), profVecVscp, VAccess::WRITE}, "append",
                    nullptr};
                AstConst* const tileIdp
                    = new AstConst{classp->fileline(), classp->flag().tileId()};
                AstConst* const workerIdp
                    = new AstConst{classp->fileline(), classp->flag().workerId()};
                AstConst* const namep
                    = new AstConst{classp->fileline(), AstConst::String{}, classp->name()};
                AstConst* const estimatep
                    = new AstConst{classp->fileline(), static_cast<uint32_t>(classp->user3())};
                for (AstNodeExpr* argp :
                     std::vector<AstNodeExpr*>{memselp, namep, tileIdp, workerIdp, estimatep}) {
                    callp->addPinsp(argp);
                }
                callp->dtypeSetVoid();
                profTraceFuncp->addStmtsp(callp->makeStmt());
                AstMemberSel* const memselClonep = memselp->cloneTree(false);
                VN_AS(memselClonep->fromp(), VarRef)->access(VAccess::WRITE);
                memselClonep->varp(profVscp->varp());
                memselClonep->dtypeFrom(profVscp->varp());
                profInitFuncp->addStmtsp(
                    new AstAssign{profInitFuncp->fileline(), memselClonep,
                                  new AstConst{memselp->fileline(), AstConst::WidthedValue{},
                                               profVscp->dtypep()->width(), 0}});
            }
        }
        profTraceFuncp->addStmtsp(returnp);
    }
};
void V3BspIpuProfile::instrument(AstNetlist* netlistp) {

    { CycleCountInstrumentationVisitor{netlistp}; }
    V3Global::dumpCheckGlobalTree("ipu_instrument", 0, dumpTree() > 3);
}