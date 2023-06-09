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
#include "V3AstUserAllocator.h"
#include "V3BspModules.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"
VL_DEFINE_DEBUG_FUNCTIONS;

class BspDpiDelegationVisitor;
class BspDpiClosureVisitor;

class BspBaseDpiVisitor VL_NOT_FINAL : public VNVisitor {
protected:
    AstClass* m_classp = nullptr;
    AstScope* m_scopep = nullptr;
    AstNetlist* m_netlistp = nullptr;

    void visit(AstNodeModule* modp) override { iterateChildren(modp); }

    void visit(AstClass* classp) override {

        if (classp->flag().isBsp()) {
            m_classp = classp;
            iterateChildren(classp);
        }
        m_classp = nullptr;
    }
    void visit(AstScope* scopep) override {
        VL_RESTORER(m_scopep);
        {
            m_scopep = scopep;
            iterateChildren(scopep);
        }
    }
    void visit(AstNetlist* nodep) override {
        VL_RESTORER(m_netlistp);
        {
            m_netlistp = nodep;
            iterateChildren(nodep);
        }
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }
};
namespace {
enum DpiSemantics {
    DPI_NONE = 0,
    DPI_STRICT = 1,
    DPI_BUFFERED = 2,
};
struct DpiInfo {
    DpiSemantics semantics = DPI_NONE;
    uint32_t numCalls = 0;
    AstVarScope* reEntryp = nullptr;
    AstVarScope* dpiPointp = nullptr;
    void append(DpiSemantics s) {
        semantics = static_cast<DpiSemantics>(semantics | s);
        numCalls++;
    }
};
struct DpiRecord {
private:
    using ClassDpiInfo = std::unordered_map<AstClass*, DpiInfo>;
    ClassDpiInfo classes;
    std::unordered_map<AstClass*, AstVarScope*> instances;
    DpiInfo netlist;

public:
    DpiRecord() {
        classes.clear();
        instances.clear();
    }
    void append(AstClass* const classp, DpiSemantics const s) {
        UASSERT(classp, "expected non-null classp");
        classes[classp].append(s);
        netlist.append(s);
    }
    void setInst(AstClass* const classp, AstVarScope* const vscp) {
        UASSERT(classp && vscp, "expected non-null arguments");
        instances[classp] = vscp;
    }

    DpiInfo getInfo(AstClass* const classp) {
        UASSERT(classp, "expected non-null");
        // UASSERT(classes.count(classp), "Class not analyzed!");
        return classes[classp];
    }
    void setDpi(AstClass* const classp, AstVarScope* const vscp) {
        UASSERT(classp, "expected non-null");
        UASSERT(classes.count(classp), "not analayzed!");
        classes[classp].dpiPointp = vscp;
    }
    void setReEntry(AstClass* const classp, AstVarScope* const vscp) {
        UASSERT(classp, "expected non-null");
        UASSERT(classes.count(classp), "not analayzed!");
        classes[classp].reEntryp = vscp;
    }

    AstVarScope* getInst(AstClass* const classp) const {
        UASSERT(classp, "expected non-null");
        UASSERT(instances.count(classp), "no class instance");
        return (*instances.find(classp)).second;
    }

    auto& getClasses() { return classes; }
};
};  // namespace
class BspDpiAnalysisVisitor final : public BspBaseDpiVisitor {
private:
    DpiRecord m_record;

private:
    inline void append(DpiSemantics s) { m_record.append(m_classp, s); }

    void visit(AstCCall* callp) {
        if (!m_classp) { return; }
        if (callp->funcp()->dpiContext() || callp->funcp()->dpiExportDispatcher()
            || callp->funcp()->dpiImportPrototype() || callp->funcp()->dpiExportImpl()
            || callp->funcp()->dpiTraceInit()) {
            callp->v3warn(E_UNSUPPORTED, "Unsupported DPI feature");
        } else if (callp->funcp()->dpiImportWrapper()) {
            append(DPI_STRICT);
        }
    }
    void visit(AstDisplay* nodep) override { append(DPI_BUFFERED); }
    void visit(AstFinish* nodep) override { append(DPI_BUFFERED); }
    void visit(AstStop* nodep) override { append(DPI_BUFFERED); }
    // void visit(AstNodeReadWriteMem* nodep) override { append(DPI_STRICT); }
    void visit(AstVarScope* vscp) {
        auto const dtypep = VN_CAST(vscp->dtypep(), ClassRefDType);
        if (dtypep && dtypep->classp()->flag().isBsp()) {
            m_record.setInst(dtypep->classp(), vscp);
        }
    }
    explicit BspDpiAnalysisVisitor(AstNetlist* netlistp) { iterate(netlistp); }

public:
    static DpiRecord analyze(AstNetlist* netlistp) {

        return BspDpiAnalysisVisitor{netlistp}.m_record;
    }
};

class BspDpiClosureVisitor final : public BspBaseDpiVisitor {
private:
    AstCFunc* m_cfuncp = nullptr;
    bool m_inArgs = false;

    DpiRecord& m_records;
    V3UniqueNames& m_closureNames;

    VNUser1InUse m_user1InUse;
    VNUser2InUse m_user2InUse;
    // STATE
    // AstVar::user1()   -> true if newly created
    // AstVar::user2p()  -> the VarScope
    void replaceFuncArg(AstVar* argp) {
        UASSERT_OBJ(m_scopep, argp, "expected scope");
        if (!argp->direction().isReadOnly()) {
            argp->v3error("Can not handle non-readony argument type in nbaTop");
        }
        UINFO(3, "Replacing func arg " << argp << endl);
        AstVar* const newp = argp->cloneTree(false);
        newp->user1(true);
        newp->name(m_closureNames.get("arg"));
        AstVarScope* const newVscp = new AstVarScope{newp->fileline(), m_scopep, newp};
        newp->user2p(newVscp);
        m_scopep->addVarsp(newVscp);

        argp->replaceWith(newp);
        m_classp->stmtsp()->addHereThisAsNext(argp);
        argp->direction(VDirection::NONE);
        argp->funcLocal(false);
        argp->funcReturn(false);
        argp->bspFlag({VBspFlag::MEMBER_LOCAL});
        AstVarScope* const argVscp = VN_AS(argp->user2p(), VarScope);
        UASSERT_OBJ(argVscp, argp, "VarScope not set on user2p!");
        AstAssign* const copyp = new AstAssign{
            argp->fileline(), new AstVarRef{argp->fileline(), argVscp, VAccess::WRITE},
            new AstVarRef{argp->fileline(), newVscp, VAccess::READ}};

        m_cfuncp->stmtsp()->addHereThisAsNext(copyp);
    }

    void replaceFuncLocal(AstVar* varp) {
        UINFO(3, "capture " << varp->name() << endl);
        m_classp->stmtsp()->addHereThisAsNext(varp->unlinkFrBack());
        varp->funcLocal(false);
        varp->bspFlag({VBspFlag::MEMBER_LOCAL});
    }

    void visit(AstVar* varp) override {
        if (!m_cfuncp || varp->user1()) { return; /* not func local */ }
        if (m_records.getInfo(m_classp).semantics == DPI_NONE) {
            return; /*class never calls a dpi function*/
        }
        UASSERT_OBJ(varp->isFuncLocal(), varp, "Expected function local variable");
        if (m_inArgs) {
            replaceFuncArg(varp);
        } else {
            replaceFuncLocal(varp);
        }
    }
    void visit(AstVarScope* vscp) { vscp->varp()->user2p(vscp); }
    void visit(AstCFunc* cfuncp) override {
        if (!m_classp) { return; }
        if (cfuncp->name() != "nbaTop"
            && !(m_classp->flag().isBspInit() && cfuncp->name() == "compute")) {
            return;
        }
        VL_RESTORER(m_cfuncp);
        {
            // ensure all variables are captured as class member variables,
            // arguments to the nbaTop function are also manually persisted since
            // they may not be set within the function body
            AstNode::user1ClearTree();
            m_cfuncp = cfuncp;
            m_inArgs = true;
            iterateAndNextNull(cfuncp->argsp());
            m_inArgs = false;
            // others are just promoted to class members rather than function local
            // variables
            iterateAndNextNull(cfuncp->initsp());
            iterateAndNextNull(cfuncp->stmtsp());
            iterateAndNextNull(cfuncp->finalsp());
            // effectively, here the function body should not have any non-member
            // variables, save only for the arguments that are manually captured
        }
    }

    explicit BspDpiClosureVisitor(AstNetlist* netlistp, DpiRecord& records, V3UniqueNames& unames)
        : m_records(records)
        , m_closureNames{unames} {
        iterate(netlistp);
    }

    friend class V3BspDpi;
};

class BspDpiDelegateVisitor final : public VNVisitor {
private:
    AstCFunc* m_dpiHandlep = nullptr;
    AstCFunc* m_cfuncp = nullptr;
    AstScope* m_scopep = nullptr;
    AstClass* m_classp = nullptr;
    // STATE
    // AstCFunc::user1() -> true if processed
    VNUser1InUse m_user1InUse;
    struct ReEntryKit {
        AstVarScope* dpiPoint = nullptr;
        AstVarScope* reEntryp = nullptr;
        std::vector<std::pair<AstNodeStmt*, AstCCall*>> callsp;
        void kill() {
            dpiPoint = nullptr;
            reEntryp = nullptr;
            callsp.clear();
        }
        operator bool() const { return reEntryp; }
    } m_dpiKit;
    AstNodeStmt* m_stmtp = nullptr;

    V3UniqueNames& m_dpiNames;
    DpiRecord& m_records;

    void initKit() {
        if (m_dpiKit) return;
        UASSERT(m_scopep && m_classp, "expected scope and class");
        const DpiInfo info = m_records.getInfo(m_classp);
        AstVar* const reEntryVarp = new AstVar{m_classp->fileline(), VVarType::MEMBER,
                                               m_dpiNames.get("reEntry"), VFlagBitPacked{}, 1};
        reEntryVarp->bspFlag({VBspFlag::MEMBER_INPUT});
        m_classp->stmtsp()->addHereThisAsNext(reEntryVarp);
        AstVarScope* const reEntryVscp
            = new AstVarScope{m_classp->fileline(), m_scopep, reEntryVarp};
        m_scopep->addVarsp(reEntryVscp);
        m_dpiKit.reEntryp = reEntryVscp;
        m_records.setReEntry(m_classp, reEntryVscp);  // used by the  BspDpiCondVisitor
        if (info.numCalls > 0) {
            AstVar* const dpiPointVarp
                = new AstVar{m_classp->fileline(), VVarType::MEMBER, m_dpiNames.get("dpiPoint"),
                             VFlagBitPacked{}, static_cast<int>(info.numCalls + 1)};
            dpiPointVarp->bspFlag(
                {VBspFlag::MEMBER_OUTPUT, VBspFlag::MEMBER_HOSTREAD, VBspFlag::MEMBER_HOSTREQ});
            m_classp->stmtsp()->addHereThisAsNext(dpiPointVarp);

            AstVarScope* const dpiPointVscp
                = new AstVarScope{m_classp->fileline(), m_scopep, dpiPointVarp};
            m_scopep->addVarsp(dpiPointVscp);
            m_dpiKit.dpiPoint = dpiPointVscp;

            m_records.setDpi(m_classp, dpiPointVscp);  // used by the BspDpiCondVisitr
        }
    }

    void killKit() { m_dpiKit.kill(); }

    void guardTrigger(AstCFunc* trigEvalp) {
        AstIf* guardp = nullptr;
        for (AstNode* stmtp = trigEvalp->stmtsp(); stmtp;) {
            AstNode* const nextp = stmtp->nextp();
            AstCReturn* const retp = VN_CAST(stmtp, CReturn);
            AstVar* const varp = VN_CAST(stmtp, Var);
            if (retp) {
            } else if (varp && varp->isFuncReturn()) {
                UASSERT_OBJ(varp->dtypep()->basicp() && varp->dtypep()->basicp()->isTriggerVec(),
                            varp, "expected TriggerVec");
                guardp = new AstIf{m_classp->fileline(),
                                   new AstLogNot{m_classp->fileline(),
                                                 new AstVarRef{m_classp->fileline(),
                                                               m_dpiKit.reEntryp, VAccess::READ}},
                                   nullptr, nullptr};
                varp->addNextHere(guardp);
            } else if (retp) {
                UASSERT_OBJ(!retp->nextp(), retp, "did not expect nextp");
                UASSERT_OBJ(guardp, trigEvalp, "expected to have found the trigger");
                // guardp->addNext();
                guardp->addNextHere(retp);
            } else if (guardp) {
                guardp->addThensp(stmtp->unlinkFrBack());
            }
            stmtp = nextp;
        }
    }

    AstNodeExpr* delegateArg(AstNodeStmt* const stmtp, AstNodeExpr* argp) {

        AstNodeExpr* const nextArgp = VN_AS(argp->nextp(), NodeExpr);
        AstVarRef* const argVRefp = VN_CAST(argp, VarRef);
        AstVarScope* const instVscp = m_records.getInst(m_classp);
        UASSERT_OBJ(instVscp, m_classp, "expected instance");
        AstVarScope* argVscp = nullptr;
        if (argVRefp) {
            // already a variable, so no need to create another one
            // but we need to add extra flags to it
            VBspFlag flag = argVRefp->varp()->bspFlag();
            if (argVRefp->access().isWriteOnly()) {
                flag.append(VBspFlag::MEMBER_INPUT).append(VBspFlag::MEMBER_HOSTWRITE);
            } else if (argVRefp->access().isWriteOrRW()) {
                if (flag.hasLocal()) flag = {VBspFlag::MEMBER_NA};
                flag.append(VBspFlag::MEMBER_INPUT)
                    .append(VBspFlag::MEMBER_OUTPUT)
                    .append(VBspFlag::MEMBER_HOSTREAD)
                    .append(VBspFlag::MEMBER_HOSTWRITE);
            } else {
                flag.append(VBspFlag::MEMBER_OUTPUT).append(VBspFlag::MEMBER_HOSTREAD);
            }
            argVRefp->varp()->bspFlag(flag);
            argVscp = argVRefp->varScopep();
        } else if (!VN_IS(argp, Const)) {
            // arbitrary expression need to be save in a variable
            AstVar* const newVarp = new AstVar{argp->fileline(), VVarType::MEMBER,
                                               m_dpiNames.get("arg"), argp->dtypep()};
            newVarp->bspFlag({VBspFlag::MEMBER_OUTPUT, VBspFlag::MEMBER_HOSTREAD});
            m_classp->stmtsp()->addHereThisAsNext(newVarp);

            AstVarScope* const newVscp = new AstVarScope{argp->fileline(), m_scopep, newVarp};
            m_scopep->addVarsp(newVscp);
            argVscp = newVscp;
            AstAssign* const assignp = new AstAssign{
                argp->fileline(), new AstVarRef{argp->fileline(), newVscp, VAccess::WRITE},
                argp->cloneTree(false)};
            stmtp->addHereThisAsNext(assignp);
        }
        if (argVscp) {
            // replace arg with a MemberSel
            VAccess access = argVRefp ? argVRefp->access() : VAccess{VAccess::READ};
            AstMemberSel* const memselp = new AstMemberSel{
                argp->fileline(), new AstVarRef{argp->fileline(), instVscp, access},
                VFlagChildDType{}, argVscp->varp()->name()};
            memselp->dtypeFrom(argVscp->varp());
            memselp->varp(argVscp->varp());
            argp->replaceWith(memselp);
            VL_DO_DANGLING(pushDeletep(argp), argp);
        } else {
            UASSERT_OBJ(VN_IS(argp, Const), argp,
                        "expected to be Const but got " << argp->prettyTypeName() << endl);
        }
        return nextArgp;
    }

    inline void delegateDisplay(AstDisplay* displayp) {
        AstSFormatF* fmtp = displayp->fmtp();
        UASSERT(!fmtp->scopeNamep() || fmtp->scopeNamep()->forall([fmtp](const AstNode* nodep) {
            return VN_IS(nodep, Text) || fmtp->scopeNamep() == nodep;
        }),
                "did not expect op2 on AstFormatF " << fmtp << endl);
        for (AstNodeExpr* argp = fmtp->exprsp(); argp; argp = delegateArg(displayp, argp)) {}
        // now create an empty stub
    }

    inline void delegateDpi(AstCCall* callp, AstNodeStmt* stmtp) {

        for (AstNodeExpr* argp = callp->argsp(); argp; argp = delegateArg(stmtp, argp)) {}
    }

    void injectReEntry(AstCFunc* cfuncp) {
        UASSERT_OBJ(m_scopep, cfuncp, "expected scope");
        UINFO(3,
              "Injecting reentry point in " << m_classp->name() << "::" << cfuncp->name() << endl);
        const DpiInfo info = m_records.getInfo(m_classp);
        if (info.semantics == DPI_NONE) {
            // simple case, guard the whole function body with the reEntry variable
            AstIf* const guardp
                = new AstIf{m_classp->fileline(),
                            new AstLogNot{m_classp->fileline(),
                                          new AstVarRef{m_classp->fileline(), m_dpiKit.reEntryp,
                                                        VAccess::READ}},
                            cfuncp->stmtsp()->unlinkFrBackWithNext()};
            cfuncp->addStmtsp(guardp);
            return;
        }
        // else need to create GOTO statments
        AstJumpBlock* const jblockExitp = new AstJumpBlock{m_classp->fileline(), nullptr};
        AstJumpLabel* const exitLabelp = new AstJumpLabel{m_classp->fileline(), jblockExitp};
        jblockExitp->labelp(exitLabelp);
        jblockExitp->addEndStmtsp(exitLabelp);
        AstJumpBlock* const jblockStartp = new AstJumpBlock{m_classp->fileline(), nullptr};
        AstJumpLabel* const startLabelp = new AstJumpLabel{m_classp->fileline(), jblockStartp};
        jblockStartp->labelp(startLabelp);
        // jblockStartp->addStmtsp(startLabelp);

        AstIf* const jumpControlp = new AstIf{
            m_classp->fileline(),
            new AstNot{m_classp->fileline(),
                       new AstVarRef{m_classp->fileline(), m_dpiKit.reEntryp, VAccess::READ}},
            new AstJumpGo{m_classp->fileline(), startLabelp}};

        AstIf* lastIfp = jumpControlp;
        int dpiIndex = 0;
        AstJumpBlock* lastJBlockp = jblockStartp;
        AstVarScope* instVscp = m_records.getInst(m_classp);
        UASSERT_OBJ(instVscp, m_classp, "Could not find instance!");
        for (const auto& dpiCallp : m_dpiKit.callsp) {
            AstNodeStmt* const stmtp = dpiCallp.first;
            AstCCall* const callp = dpiCallp.second;
            UASSERT(stmtp, "expected statement");
            AstNodeStmt* const stmtClonep = stmtp->cloneTree(false);
            UINFO(3, "Replacing call " << callp->name() << endl);
            bool needReEntry = true;
            if (stmtp && callp) {
                // DPI call
                UASSERT_OBJ(VN_IS(stmtp, StmtExpr), stmtp,
                            "Expected AstStmtExpr around DPI wrapper");
                UASSERT_OBJ(VN_AS(stmtp, StmtExpr)->exprp() == callp, stmtp,
                            "Expected AstCCall child");
                delegateDpi(callp, stmtp);
            } else if (VN_IS(stmtp, Stop) || VN_IS(stmtp, Finish)) {
                // delegateTermination(stmtp, dpiPoint, exitLabelp);
                needReEntry = false;
            } else if (auto const dispp = VN_CAST(stmtp, Display)) {
                delegateDisplay(dispp);
            } else {
                // error?
                UASSERT_OBJ(false, stmtp, "Can not handle delegation");
            }
            V3Number dpiPoint{stmtp->fileline(), m_dpiKit.dpiPoint->width(), 0};
            // dpiPoint is bit vector whose LSB signifies that there is a DPI
            // call and the rest of the bits are used as DPI identifiers
            // dpiPoint[0] --> dpi enabled
            // dpiPoint[1 + dpiIndex] -> dpi id (one-hot)
            dpiPoint.setBit(0, 1);
            dpiPoint.setBit(++dpiIndex, 1);

            AstAssign* const dpiSetp = new AstAssign{
                stmtp->fileline(),
                new AstVarRef{stmtp->fileline(), m_dpiKit.dpiPoint, VAccess::WRITE},
                new AstConst{stmtp->fileline(), dpiPoint}};
            stmtp->replaceWith(dpiSetp);
            AstJumpGo* const goExitp = new AstJumpGo{stmtp->fileline(), exitLabelp};
            dpiSetp->addNextHere(goExitp);
            dpiSetp->addHereThisAsNext(new AstDelegate(stmtp->fileline(), stmtClonep));
            // link the stmtp to the host function
            AstMemberSel* const dpiSelp = new AstMemberSel{
                stmtp->fileline(), new AstVarRef{stmtp->fileline(), instVscp, VAccess::READ},
                VFlagChildDType{}, m_dpiKit.dpiPoint->varp()->name()};
            dpiSelp->dtypeFrom(m_dpiKit.dpiPoint->varp());
            dpiSelp->varp(m_dpiKit.dpiPoint->varp());
            m_dpiHandlep->addStmtsp(new AstIf{
                stmtp->fileline(),
                new AstEq{stmtp->fileline(), dpiSelp, new AstConst{stmtp->fileline(), dpiPoint}},
                stmtp, nullptr});

            if (!needReEntry) {
                // $finish and $stop terminate execution and do not need rentry points
                continue;
            }

            AstJumpBlock* const jblockp = new AstJumpBlock{stmtp->fileline(), nullptr};
            AstJumpLabel* const labelp = new AstJumpLabel{stmtp->fileline(), jblockp};
            jblockp->labelp(labelp);
            lastJBlockp->addStmtsp(jblockp);

            lastJBlockp = jblockp;

            goExitp->addNextHere(labelp);

            AstAssign* const dpiResetp = new AstAssign{
                stmtp->fileline(),
                new AstVarRef{stmtp->fileline(), m_dpiKit.dpiPoint, VAccess::WRITE},
                new AstConst{stmtp->fileline(), AstConst::WidthedValue{}, dpiPoint.width(), 0}};
            labelp->addNextHere(dpiResetp);

            labelp->addNextHere(new AstComment{
                labelp->fileline(),
                "Re-entry for "
                    + (callp ? "DPI " + callp->funcp()->name() : stmtp->prettyTypeName())});

            // entry point calculation:
            AstIf* const ifp = new AstIf{
                stmtp->fileline(),
                new AstEq{stmtp->fileline(),
                          new AstVarRef{stmtp->fileline(), m_dpiKit.dpiPoint, VAccess::READ},
                          new AstConst{stmtp->fileline(), dpiPoint}},
                new AstJumpGo{stmtp->fileline(), labelp}, nullptr};
            lastIfp->addElsesp(ifp);
            lastIfp = ifp;
        }
        lastIfp->addElsesp(new AstJumpGo{m_classp->fileline(), exitLabelp});
        lastJBlockp->addStmtsp(jumpControlp);
        lastJBlockp->addStmtsp(startLabelp);
        lastJBlockp->addStmtsp(cfuncp->stmtsp()->unlinkFrBackWithNext());
        jblockExitp->addStmtsp(jblockStartp);
        cfuncp->addStmtsp(jblockExitp);
    }
    void visit(AstNodeStmt* nodep) override {
        VL_RESTORER(m_stmtp);
        {
            m_stmtp = nodep;
            iterateChildren(nodep);
        }
    }

    void visit(AstClass* classp) override {
        VL_RESTORER(m_classp);
        if (classp->flag().isBsp()) {
            m_classp = classp;
            iterateChildren(classp);
        }
    }
    void visit(AstScope* scopep) override {
        if (!m_classp) { return; }
        VL_RESTORER(m_scopep);
        {
            m_scopep = scopep;
            initKit();
            iterateChildren(scopep);
            killKit();
        }
    }
    void visit(AstCCall* callp) override {
        if (!m_classp || !m_cfuncp) { return; }
        if (!callp->funcp()->dpiImportWrapper()) {
            callp->v3warn(E_UNSUPPORTED, "Unsupported DPI call");
            return;
        }
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
        m_dpiKit.callsp.emplace_back(m_stmtp, callp);
    }
    void visit(AstDisplay* dispp) override { m_dpiKit.callsp.emplace_back(dispp, nullptr); }
    void visit(AstFinish* finishp) override { m_dpiKit.callsp.emplace_back(finishp, nullptr); }
    void visit(AstStop* stopp) override { m_dpiKit.callsp.emplace_back(stopp, nullptr); }

    void visit(AstNodeReadWriteMem* rwMemp) override {
        m_dpiKit.callsp.emplace_back(rwMemp, nullptr);
    }

    void visit(AstNodeModule* modp) override { /*nothing to do*/
    }

    void visit(AstCFunc* cfuncp) override {
        if (!m_classp) { return; /* nothing to do*/ }
        if (cfuncp->user1()) { return; /* all done */ }
        UASSERT(m_scopep && m_classp, "expected none-null!");
        VL_RESTORER(m_cfuncp);
        {
            cfuncp->user1(true);
            if (cfuncp->name() == "triggerEval") {
                guardTrigger(cfuncp);
            } else if (cfuncp->name() == "nbaTop"
                       || (m_classp->flag().isBspInit() && cfuncp->name() == "compute")) {
                m_cfuncp = cfuncp;
                // iterate and collect all the DPI call wrappers
                iterateChildren(cfuncp);
                injectReEntry(cfuncp);
            }
        }
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }
    explicit BspDpiDelegateVisitor(AstNetlist* netlistp, V3UniqueNames& newNames,
                                   DpiRecord& records)
        : m_dpiNames(newNames)
        , m_records(records) {
        m_dpiHandlep = new AstCFunc{netlistp->fileline(), "hostHandle",
                                    netlistp->topScopep()->scopep(), "void"};
        m_dpiHandlep->dontCombine(true);
        m_dpiHandlep->isInline(false);
        m_dpiHandlep->isMethod(true);
        netlistp->topScopep()->scopep()->addBlocksp(m_dpiHandlep);
        iterate(netlistp);
    }

    friend class V3BspDpi;
};

class BspDpiCondVisitor final : public VNVisitor {
private:
    AstNetlist* const m_netlistp;
    DpiRecord& m_records;
    V3UniqueNames& m_freshNames;

    void go() {

        FileLine* const flp = m_netlistp->fileline();
        AstClass* newClsp = new AstClass{flp, m_freshNames.get("condeval")};
        newClsp->classOrPackagep(new AstClassPackage{flp, m_freshNames.get("condeval_pkg")});
        newClsp->classOrPackagep()->classp(newClsp);

        AstClassRefDType* dtypep = new AstClassRefDType{flp, newClsp, nullptr};
        AstPackage* parentPkgp = nullptr;
        AstScope* parentScopep = nullptr;
        m_netlistp->foreach([&parentPkgp, &parentScopep](AstPackage* pkgp) {
            if (pkgp->name() == V3BspSched::V3BspModules::builtinBspPkg) {
                parentPkgp = pkgp;
                pkgp->foreach([&parentScopep](AstScope* scopep) { parentScopep = scopep; });
            }
        });

        dtypep->classOrPackagep(parentPkgp);
        m_netlistp->typeTablep()->addTypesp(dtypep);

        newClsp->level(4);
        newClsp->flag(VClassFlag{}
                          .append(VClassFlag::BSP_BUILTIN)
                          .append(VClassFlag::BSP_COND_BUILTIN)
                          .withTileId(0)
                          .withWorkerId(0));
        AstVar* classInstp
            = new AstVar{flp, VVarType::VAR, m_freshNames.get("condevalinst"), dtypep};
        classInstp->lifetime(VLifetime::STATIC);
        AstVarScope* instVscp
            = new AstVarScope{flp, m_netlistp->topScopep()->scopep(), classInstp};
        m_netlistp->topScopep()->scopep()->addVarsp(instVscp);
        m_netlistp->topModulep()->addStmtsp(classInstp);

        AstCell* parentCellp = nullptr;
        m_netlistp->topModulep()->foreach([&parentCellp, parentPkgp](AstCell* cellp) {
            if (cellp->modp() == parentPkgp) { parentCellp = cellp; }
        });
        AstScope* scopep = new AstScope{flp, newClsp, parentScopep->name() + "." + newClsp->name(),
                                        parentScopep, parentCellp};

        // create the compute func
        AstCFunc* compFuncp = new AstCFunc{flp, "compute", scopep, "void"};
        // find the exchange function
        compFuncp->isInline(true);
        compFuncp->dontCombine(true);
        compFuncp->isMethod(true);

        // The compute method should compute the disjunction of all the dpi
        // conditions from both the Init and non-Init classes.

        auto makeVar = [flp, scopep, newClsp,
                        compFuncp](const string& name, AstNodeDType* const dtypep, bool funcLocal,
                                   const VBspFlag flag) -> AstVarScope* const {
            AstVar* const varp = new AstVar{flp, VVarType::MEMBER, name, dtypep};
            AstVarScope* const vscp = new AstVarScope{flp, scopep, varp};
            scopep->addVarsp(vscp);
            if (funcLocal) {
                compFuncp->addStmtsp(varp);
                varp->funcLocal(true);
            } else {
                newClsp->addStmtsp(varp);
                varp->bspFlag(flag);
            }
            return vscp;
        };

        AstVarScope* const dpiCondVscp
            = makeVar(m_freshNames.get("hasDpi"), m_netlistp->findBitDType(), false,
                      {VBspFlag::MEMBER_HOSTANYREQ, VBspFlag::MEMBER_OUTPUT,
                       VBspFlag::MEMBER_HOSTREAD, VBspFlag::MEMBER_HOSTREQ});

        AstVarScope* const tmpVscp
            = makeVar(m_freshNames.get("tmp"), dpiCondVscp->dtypep(), true, {});

        /// generate the following:
        //      tmp = 0;
        //      for (p : dpiPoints) tmp |= (p[0:0]);
        //      hasDpi = tmp;
        compFuncp->addStmtsp(
            new AstAssign{flp, new AstVarRef{flp, tmpVscp, VAccess::WRITE},
                          new AstConst{flp, AstConst::WidthedValue{}, tmpVscp->width(), 0}});

        // create an exchange which should come before the compute in the execution
        auto mkModFunc = [this](const string& name) -> AstCFunc* {
            AstCFunc* modFuncp = new AstCFunc{m_netlistp->fileline(), name,
                                              m_netlistp->topScopep()->scopep(), "void"};
            modFuncp->isInline(false);
            modFuncp->dontCombine(true);
            modFuncp->isMethod(true);
            m_netlistp->topScopep()->scopep()->addBlocksp(modFuncp);
            return modFuncp;
        };
        auto mkMemSel = [](AstVarScope* const varVscp, AstVarScope* const memberVscp,
                           VAccess const access) -> AstMemberSel* {
            FileLine* fl = varVscp->fileline();
            AstMemberSel* const memselp
                = new AstMemberSel{fl, new AstVarRef{fl, varVscp, access}, VFlagChildDType{},
                                   memberVscp->varp()->name()};
            memselp->varp(memberVscp->varp());
            memselp->dtypeFrom(memberVscp->varp());
            return memselp;
        };

        AstCFunc* const exchangeFuncp = mkModFunc(
            "dpiExchange");  // incast all the vertex dpi vectors to the condeval vertex
        AstCFunc* const broadcastFuncp
            = mkModFunc("dpiBroadcast");  // broadcast the result back to "reEntry" variables
        for (const auto& pair : m_records.getClasses()) {
            AstClass* const classp = pair.first;
            UASSERT(classp, "classp should be non-null" << endl);
            AstVarScope* const dpiPointp = pair.second.dpiPointp;
            AstVarScope* const reEntryp = pair.second.reEntryp;
            UASSERT_OBJ(reEntryp, classp, "expected re-entry variable");
            AstVarScope* const sourceInstVscp = m_records.getInst(classp);
            UASSERT_OBJ(instVscp, classp, "not instance found for class: " << classp << endl);

            broadcastFuncp->addStmtsp(
                new AstAssign{flp, mkMemSel(sourceInstVscp, reEntryp, VAccess::WRITE),
                              mkMemSel(instVscp, dpiCondVscp, VAccess::READ)});

            if (!dpiPointp) continue;  // not participating
            AstVarScope* dpiPartVscp = makeVar(m_freshNames.get("vec"), dpiPointp->dtypep(), false,
                                               {VBspFlag::MEMBER_INPUT});
            AstSel* const bitSelp = new AstSel{flp, new AstVarRef{flp, dpiPartVscp, VAccess::READ},
                                               new AstConst{flp, 0}, new AstConst{flp, 1}};
            AstOr* const orp = new AstOr{flp, bitSelp, new AstVarRef{flp, tmpVscp, VAccess::READ}};
            AstAssign* const assignp
                = new AstAssign{flp, new AstVarRef{flp, tmpVscp, VAccess::WRITE}, orp};
            compFuncp->addStmtsp(assignp);

            exchangeFuncp->addStmtsp(
                new AstAssign{flp, mkMemSel(instVscp, dpiPartVscp, VAccess::WRITE),
                              mkMemSel(sourceInstVscp, dpiPointp, VAccess::READ)});

            AstMemberSel* const sourceSelp
                = new AstMemberSel{flp, new AstVarRef{flp, sourceInstVscp, VAccess::READ},
                                   VFlagChildDType{}, dpiPointp->varp()->name()};
            sourceSelp->varp(dpiPointp->varp());
            sourceSelp->dtypeFrom(dpiPointp->varp());

            AstMemberSel* const targetSelp
                = new AstMemberSel{flp, new AstVarRef{flp, instVscp, VAccess::WRITE},
                                   VFlagChildDType{}, dpiPartVscp->varp()->name()};
            targetSelp->varp(dpiPartVscp->varp());
            targetSelp->dtypeFrom(dpiPartVscp->varp());

        }
        compFuncp->addStmtsp(new AstAssign{flp, new AstVarRef{flp, dpiCondVscp, VAccess::WRITE},
                                           new AstVarRef{flp, tmpVscp, VAccess::WRITE}});

        scopep->addBlocksp(compFuncp);
        newClsp->addStmtsp(scopep);

        m_netlistp->addModulesp(newClsp);
        m_netlistp->addModulesp(newClsp->classOrPackagep());
    }

    void visit(AstNode*) override {}

    explicit BspDpiCondVisitor(AstNetlist* netlistp, DpiRecord& records, V3UniqueNames& freshNames)
        : m_netlistp{netlistp}
        , m_records{records}
        , m_freshNames{freshNames} {
        go();
    }
    friend class V3BspDpi;
};
void V3BspDpi::delegateAll(AstNetlist* nodep) {

    V3UniqueNames m_newNames = VL_UNIQUENAMES("closure");

    UINFO(3, "Analyzing DPI calls" << endl);
    auto records = BspDpiAnalysisVisitor::analyze(nodep);

    UINFO(3, "Making DPI closures" << endl);
    { BspDpiClosureVisitor{nodep, records, m_newNames}; }
    V3Global::dumpCheckGlobalTree("bspDpiClosure", 0, dumpTree() >= 1);

    UINFO(3, "Delegating DPI calls" << endl);
    { BspDpiDelegateVisitor{nodep, m_newNames, records}; }
    V3Global::dumpCheckGlobalTree("bspDpiDelegate", 0, dumpTree() >= 1);

    UINFO(3, "Creating dpi condition" << endl);
    { BspDpiCondVisitor{nodep, records, m_newNames}; }
    V3Global::dumpCheckGlobalTree("bspDpiCond", 0, dumpTree() >= 1);
}