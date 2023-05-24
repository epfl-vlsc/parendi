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

#include "V3BspDpi.h"

#include "V3Ast.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

VL_DEFINE_DEBUG_FUNCTIONS;

class BspDpiDelegationVisitor final : public VNVisitor {
private:
    // STATE
    // AstCCall::user1()   -> processed
    VNUser1InUse m_user1InUse;  // clear on CFunc

    AstCFunc* m_dpiHandlep = nullptr;
    AstClass* m_classp = nullptr;
    AstVarScope* m_instp = nullptr;
    AstScope* m_scopep = nullptr;
    AstCFunc* m_nbaTopp = nullptr;
    AstNodeStmt* m_stmtp = nullptr;

    std::vector<std::pair<AstNodeStmt*, AstCCall*>> m_dpis;

    V3UniqueNames m_dpiNames;

    void visit(AstNodeStmt* stmtp) override {
        m_stmtp = stmtp;
        iterateChildren(stmtp);
    }

    void visit(AstCCall* callp) override {
        if (!m_classp) {
            return;
        } else if (callp->funcp()->dpiImportPrototype()) {
            callp->v3error("Did not expected DPI prototype here!");
            return;
        } else if (!callp->funcp()->dpiImportWrapper()) {
            return;  // nothing to do
        } else if (callp->user1()) {
            return;  // done
        }
        callp->user1(true);
        UASSERT_OBJ(m_nbaTopp, callp, "not under nbaTop function");
        UASSERT_OBJ(m_stmtp, callp, "can not be expression!");
        UASSERT_OBJ(VN_IS(m_stmtp, StmtExpr), callp, "DPI not in statement position");
        UASSERT_OBJ(
            [](AstCCall* callp) -> bool {
                for (AstNodeExpr* argp = callp->argsp(); argp;
                     argp = VN_AS(argp->nextp(), NodeExpr)) {
                    if (!(VN_IS(argp, Const) || VN_IS(argp, VarRef))) return false;
                }
                return true;
            }(callp),
            callp, "not in normal form");
        m_dpis.emplace_back(m_stmtp, callp);
        // // don't call this function, rather create a jump label right after the
        // // call site
        // AstJumpBlock* const jblockp = new AstJumpBlock{callp->fileline(), nullptr};
        // AstJumpLabel* const labelp = new AstJumpLabel{callp->fileline(), jblockp};
        // jblockp->labelp(labelp);
        // jblockp->labelNum(m_labelNum++);
        // jblockp->addStmtsp(labelp);
        // if (m_stmtp->nextp()) {
        //     auto nextp = m_stmtp->nextp()->unlinkFrBackWithNext();
        //     jblockp->addStmtsp(nextp);
        //     m_stmtp->addNext(jblockp);
        // } else {
        //     m_stmtp->addNext(jblockp);
        // }
    }

    void visit(AstCFunc* funcp) override {
        UASSERT_OBJ(m_classp, funcp, "expected enclosing class");
        if (funcp->name() == "nbaTop") {
            m_nbaTopp = funcp;
        } else if (m_classp->flag().isBspInit() && funcp->name() == "compute") {
            m_nbaTopp = funcp;
        } else {
            m_nbaTopp = nullptr;
            return;  // do not modify anything other than the top compute or nbaTop function
        }

        iterateChildren(funcp);
        if (m_dpis.size() == 0) return /*no DPI calls inside*/;

        AstVar* const rentryVarp = new AstVar{m_classp->fileline(), VVarType::MEMBER,
                                              m_dpiNames.get("rentry"), VFlagBitPacked{}, 1};
        rentryVarp->bspFlag({VBspFlag::MEMBER_INPUT});
        m_classp->stmtsp()->addHereThisAsNext(rentryVarp);
        AstVarScope* const rentryVscp
            = new AstVarScope{m_classp->fileline(), m_scopep, rentryVarp};
        m_scopep->addVarsp(rentryVscp);

        AstVar* const dpiPointVarp
            = new AstVar{m_classp->fileline(), VVarType::MEMBER, m_dpiNames.get("dpiPoint"),
                         VFlagBitPacked{}, static_cast<int>(m_dpis.size()) + 1};
        dpiPointVarp->bspFlag({VBspFlag::MEMBER_OUTPUT, VBspFlag::MEMBER_HOSTREAD});
        m_classp->stmtsp()->addHereThisAsNext(dpiPointVarp);

        AstVarScope* const dpiPointVscp
            = new AstVarScope{m_classp->fileline(), m_scopep, dpiPointVarp};
        m_scopep->addVarsp(dpiPointVscp);

        AstJumpBlock* const jblockExitp = new AstJumpBlock{m_classp->fileline(), nullptr};
        AstJumpLabel* const exitLabelp = new AstJumpLabel{m_classp->fileline(), jblockExitp};
        jblockExitp->labelp(exitLabelp);
        jblockExitp->addEndStmtsp(exitLabelp);
        AstJumpBlock* const jblockStartp = new AstJumpBlock{m_classp->fileline(), nullptr};
        AstJumpLabel* const startLabelp = new AstJumpLabel{m_classp->fileline(), jblockStartp};
        jblockStartp->labelp(startLabelp);
        // jblockStartp->addStmtsp(startLabelp);

        AstIf* const jumpControlp
            = new AstIf{m_classp->fileline(),
                        new AstNot{m_classp->fileline(),
                                   new AstVarRef{m_classp->fileline(), rentryVscp, VAccess::READ}},
                        new AstJumpGo{m_classp->fileline(), startLabelp}};

        AstIf* lastIfp = jumpControlp;
        int dpiIndex = 0;
        AstJumpBlock* lastJBlockp = jblockStartp;
        for (const auto& dpiCallp : m_dpis) {
            AstNodeStmt* const stmtp = dpiCallp.first;
            AstCCall* const callp = dpiCallp.second;
            AstJumpBlock* const jblockp = new AstJumpBlock{callp->fileline(), nullptr};
            AstJumpLabel* const labelp = new AstJumpLabel{callp->fileline(), jblockp};
            jblockp->labelp(labelp);
            lastJBlockp->addStmtsp(jblockp);
            lastJBlockp = jblockp;

            // analyse the arguments and flag class members with necessary information
            // Input arguments are either VarRef or Const:
            // 1. Const arguments are not saved to be transmitted.
            // 2. VarRef arguments that are function local need to be saved to
            //    class member (persitent) variables.
            //    Eariler passes usually leaves us with DPIIW function whose
            //    return type is void but take a the last argument as an LV reference
            UASSERT_OBJ(VN_IS(stmtp, StmtExpr), stmtp, "Expected AstStmtExpr around DPI wrapper");
            UASSERT_OBJ(VN_AS(stmtp, StmtExpr)->exprp() == callp, stmtp,
                        "Expected AstCCall child");
            for (AstNodeExpr* argp = callp->argsp(); argp;) {
                UASSERT_OBJ(VN_IS(argp, Const) || VN_IS(argp, VarRef), argp,
                            "Unexpected argument type \"" << argp->prettyTypeName()
                                                          << "\" is not in normal form" << endl);
                AstNodeExpr* const nextArgp = VN_AS(argp->nextp(), NodeExpr);
                if (AstVarRef* const argVRefp = VN_CAST(argp, VarRef)) {
                    if (argVRefp->varp()->isFuncLocal()) {
                        // make it a class member
                        m_classp->stmtsp()->addHereThisAsNext(argVRefp->varp()->unlinkFrBack());
                    }
                    VBspFlag flag = argVRefp->varp()->bspFlag();

                    if (argVRefp->access().isWriteOnly()) {
                        flag.append(VBspFlag::MEMBER_INPUT).append(VBspFlag::MEMBER_HOSTWRITE);
                    } else if (argVRefp->access().isWriteOrRW()) {
                        flag.append(VBspFlag::MEMBER_INPUT)
                            .append(VBspFlag::MEMBER_OUTPUT)
                            .append(VBspFlag::MEMBER_HOSTREAD)
                            .append(VBspFlag::MEMBER_HOSTWRITE);
                    } else {
                        flag.append(VBspFlag::MEMBER_OUTPUT).append(VBspFlag::MEMBER_HOSTREAD);
                    }
                    argVRefp->varp()->bspFlag(flag);

                    // replace arg with a MemberSel
                    AstMemberSel* const memselp = new AstMemberSel{
                        argVRefp->fileline(),
                        new AstVarRef{argVRefp->fileline(), m_instp, argVRefp->access()},
                        VFlagChildDType{}, argVRefp->varp()->name()};
                    memselp->dtypeFrom(argVRefp->varp());
                    memselp->varp(argVRefp->varp());
                    argp->replaceWith(memselp);
                    VL_DO_DANGLING(pushDeletep(argp), argp);
                }
                argp = nextArgp;
            }

            V3Number dpiPoint{callp->fileline(), dpiPointVscp->width(), 0};
            // dpiPoint[0] --> dpi enabled
            // dpiPoint[1 + dpiIndex] -> which dpi call
            dpiPoint.setBit(0, 1);
            dpiPoint.setBit(++dpiIndex, 1);
            stmtp->addNextHere(new AstAssign{
                stmtp->fileline(), new AstVarRef{stmtp->fileline(), dpiPointVscp, VAccess::WRITE},
                new AstConst{stmtp->fileline(), dpiPoint}});
            stmtp->nextp()->addNextHere(new AstJumpGo{stmtp->fileline(), exitLabelp});
            stmtp->nextp()->nextp()->addNextHere(labelp);
            if (dpiIndex != 1) {
                labelp->addNextHere(
                    new AstAssign{stmtp->fileline(),
                                  new AstVarRef{stmtp->fileline(), dpiPointVscp, VAccess::WRITE},
                                  new AstConst{stmtp->fileline(), AstConst::WidthedValue{},
                                               dpiPoint.width(), 0}});
            }
            labelp->addNextHere(
                new AstComment{labelp->fileline(), "Re-entry for " + callp->funcp()->name()});
            // dpiPoint.
            AstIf* ifp = new AstIf{
                callp->fileline(),
                new AstEq{callp->fileline(),
                          new AstVarRef{callp->fileline(), dpiPointVscp, VAccess::READ},
                          new AstConst{callp->fileline(), dpiPoint}},
                new AstJumpGo{callp->fileline(), labelp}, nullptr};
            lastIfp->addElsesp(ifp);
            lastIfp = ifp;

            // append the actual call to the host handle
            AstMemberSel* const dpiSelp = new AstMemberSel{
                stmtp->fileline(), new AstVarRef{stmtp->fileline(), m_instp, VAccess::READ},
                VFlagChildDType{}, dpiPointVarp->name()};
            dpiSelp->dtypeFrom(dpiPointVarp);
            dpiSelp->varp(dpiPointVarp);
            m_dpiHandlep->addStmtsp(new AstIf{
                stmtp->fileline(),
                new AstEq{stmtp->fileline(), dpiSelp, new AstConst{stmtp->fileline(), dpiPoint}},
                stmtp->unlinkFrBack(), nullptr});
        }
        lastIfp->addElsesp(new AstJumpGo{m_classp->fileline(), exitLabelp});
        lastJBlockp->addStmtsp(jumpControlp);
        lastJBlockp->addStmtsp(startLabelp);
        lastJBlockp->addStmtsp(funcp->stmtsp()->unlinkFrBackWithNext());
        jblockExitp->addStmtsp(jblockStartp);
        funcp->addStmtsp(jblockExitp);
        // funcp->addStmtsp(jblockStartp);
    }

    void visit(AstClass* classp) override {
        m_classp = classp;
        AstNode::user1ClearTree();
        m_dpis.clear();
        iterateChildren(classp);
    }

    void visit(AstScope* scopep) override {
        m_scopep = scopep;
        iterateChildren(scopep);
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit BspDpiDelegationVisitor(AstNetlist* netlistp)
        : m_dpiNames{"__VbspDpi"} {

        if (v3Global.dpi()) {
            m_dpiHandlep = new AstCFunc{netlistp->fileline(), "dpiHandle",
                                        netlistp->topScopep()->scopep(), "void"};
            m_dpiHandlep->dontCombine(true);
            m_dpiHandlep->isInline(false);
            m_dpiHandlep->isMethod(true);
            netlistp->topScopep()->scopep()->addBlocksp(m_dpiHandlep);
        }
        netlistp->topModulep()->foreach([this](AstVarScope* vscp) {
            AstVar* const varp = vscp->varp();
            AstClassRefDType* const clsDTypep = VN_CAST(varp->dtypep(), ClassRefDType);
            if (!clsDTypep || !clsDTypep->classp()->flag().isBsp()) /*not a bsp class*/
                return;
            AstClass* const classp = clsDTypep->classp();
            m_instp = vscp;
            visit(classp);
        });
    }
};

void V3BspDpi::delegateAll(AstNetlist* nodep) {
    { BspDpiDelegationVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("bspDpi", 0, dumpTree() >= 1);
}