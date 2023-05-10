// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Lower the program tree create a poplar program
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

#include "V3BspPoplarProgram.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3BspModules.h"
#include "V3EmitCBase.h"
#include "V3Global.h"
#include "V3UniqueNames.h"

#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

class PoplarSetTileAndWorkerVisitor final {
private:
    uint32_t m_numAvailTiles;
    uint32_t m_numAvailWorkers;
    AstNetlist* m_netlistp;
    void doLocate(const std::vector<AstClass*> unlocated) {

        if (unlocated.size() > m_numAvailTiles * m_numAvailWorkers) {
            m_netlistp->v3warn(UNOPT, "Not enough tiles, exceeding worker limit: There are  "
                                          << unlocated.size() << " parallel process but have only "
                                          << m_numAvailTiles << "*" << m_numAvailWorkers
                                          << " tiles*workers" << endl);
        }
        // simple tile assignment
        uint32_t tid = 0;
        uint32_t wid = 0;
        for (AstClass* classp : unlocated) {
            auto newFlag = classp->flag().withTileId(tid++).withWorkerId(wid);
            classp->flag(newFlag);
            if (tid == m_numAvailTiles) {
                tid = 0;
                wid++;
            }
        }
    }

public:
    explicit PoplarSetTileAndWorkerVisitor(AstNetlist* netlistp) {
        // collect all the bsp classes that do not have tile or worker id
        // const auto numAvailTiles = V3Global.opt
        m_netlistp = netlistp;
        m_numAvailTiles = static_cast<uint32_t>(v3Global.opt.tiles());
        m_numAvailWorkers = static_cast<uint32_t>(v3Global.opt.workers());
        std::vector<AstClass*> unlocatedCompute;
        std::vector<AstClass*> unlocatedInit;
        netlistp->topModulep()->foreach([&](AstVar* varp) {
            AstClassRefDType* clsTypep = VN_CAST(varp->dtypep(), ClassRefDType);
            if (clsTypep && clsTypep->classp()->flag().isBsp()
                && clsTypep->classp()->flag().tileId() >= m_numAvailTiles) {
                if (clsTypep->classp()->flag().isBspInit())
                    unlocatedInit.push_back(clsTypep->classp());
                else
                    unlocatedCompute.push_back(clsTypep->classp());
            }
        });
        doLocate(unlocatedCompute);
        doLocate(unlocatedInit);
    }
};

class PoplarPlusArgsVisitor final : public VNVisitor {
private:
    V3UniqueNames m_hostFuncs;
    V3UniqueNames m_varNames;

    AstVarScope* m_instp = nullptr;
    AstClass* m_classp = nullptr;
    AstScope* m_scopep = nullptr;
    AstNetlist* m_netlistp = nullptr;

    struct ReadMemSubst {
        AstReadMem* origp = nullptr;
        AstVar* hostMemp = nullptr;
        AstVarScope* classInstp = nullptr;
        ReadMemSubst(AstReadMem* origp, AstVar* hostMemp, AstVarScope* classInstp)
            : origp{origp}
            , hostMemp{hostMemp}
            , classInstp{classInstp} {}
    };

    std::vector<ReadMemSubst> m_rmems;
    struct PlusArgSubst {
        AstNodeExpr* origp = nullptr;
        AstVar* firep = nullptr;
        AstVarScope* classInstp = nullptr;
        AstVar* valp = nullptr;
        PlusArgSubst(AstNodeExpr* origp, AstVar* firep, AstVarScope* classInstp, AstVar* valp)
            : origp(origp)
            , firep(firep)
            , classInstp(classInstp)
            , valp(valp) {}
    };
    std::vector<PlusArgSubst> m_substs;

    void visit(AstClass* nodep) {
        UASSERT(nodep->flag().isBsp(), "expected BSP class");
        UINFO(10, "visiting " << nodep->name() << endl);
        UASSERT_OBJ(m_instp, nodep, "class is not instantiated");
        m_classp = nodep;
        iterateChildren(nodep);
    }
    void visit(AstScope* nodep) {
        m_scopep = nodep;
        iterateChildren(nodep);
    }

    void visit(AstTestPlusArgs* nodep) {
        UINFO(3, "replacing " << nodep << endl);
        // replace $test$plusargs("something") a simple class member
        // on the class and and then push it to the m_substs to later deal with
        AstVar* varp = new AstVar{nodep->fileline(), VVarType::MEMBER, m_varNames.get("test"),
                                  nodep->dtypep()};
        varp->bspFlag(
            VBspFlag{}.append(VBspFlag::MEMBER_INPUT).append(VBspFlag::MEMBER_HOSTWRITE));
        AstVarScope* vscp = new AstVarScope{varp->fileline(), m_scopep, varp};
        m_classp->stmtsp()->addHereThisAsNext(varp);
        m_scopep->addVarsp(vscp);
        m_substs.emplace_back(nodep, varp, m_instp, nullptr /*no value*/);
        AstVarRef* vrefp = new AstVarRef{nodep->fileline(), vscp, VAccess::READ};
        nodep->replaceWith(vrefp);
    }

    void visit(AstValuePlusArgs* nodep) {
        UINFO(3, "replacing " << nodep << endl);
        auto addVarToClass = [this, nodep](const std::string& name, AstNodeDType* dtypep) {
            AstVar* varp = new AstVar{nodep->fileline(), VVarType::MEMBER, name, dtypep};
            varp->bspFlag(
                VBspFlag{}.append(VBspFlag::MEMBER_INPUT).append(VBspFlag::MEMBER_HOSTWRITE));
            AstVarScope* vscp = new AstVarScope{varp->fileline(), m_scopep, varp};
            m_classp->stmtsp()->addHereThisAsNext(varp);
            m_scopep->addVarsp(vscp);
            return std::make_pair(varp, vscp);
        };

        auto firep = addVarToClass(m_varNames.get("valuetest"), nodep->dtypep());
        m_substs.emplace_back(nodep, firep.first, m_instp, nullptr);
        UASSERT_OBJ(nodep->outp(), nodep, "expected argument");
        auto valuep
            = addVarToClass(m_varNames.get("valuevalue"), VN_AS(nodep->outp(), VarRef)->dtypep());
        m_substs.back().valp = valuep.first;

        AstValuePlusArgsProxy* proxyp = new AstValuePlusArgsProxy{
            nodep->fileline(), new AstVarRef{nodep->fileline(), firep.second, VAccess::READ},
            new AstVarRef{nodep->fileline(), valuep.second, VAccess::READ},
            VN_AS(nodep->outp(), VarRef)->cloneTree(false)};
        proxyp->dtypep(nodep->dtypep());

        nodep->replaceWith(proxyp);
    }

    void visit(AstReadMem* nodep) {
        UINFO(3, "replacing " << nodep << endl);
        if (nodep->lsbp() || nodep->msbp()) {
            nodep->v3warn(E_UNSUPPORTED, "can not have start and end address");
        }
        // $read/writememh/b(filename, memvar, start, end);
        // becomes
        // ## IPU : for (i = start to end) memvar[i] = hostMemVar[i - start];
        // ## HOST: $read/writememh/b(filename, hostMemVar, start, end);
        //          copyToDevice(hostMemVar);

        // note that this transformation may introduce false warnings in case
        // the file does not exist and is never also read. It also breaks if
        // the memory is too large, since we now need to keep two copies of the
        // same array on the same vertex
        FileLine* flp = nodep->fileline();
        AstVar* varp
            = new AstVar{flp, VVarType::MEMBER, m_varNames.get("rmrepl"), nodep->memp()->dtypep()};
        varp->bspFlag(
            VBspFlag{}.append(VBspFlag::MEMBER_INPUT).append(VBspFlag::MEMBER_HOSTWRITE));
        AstVarScope* vscp = new AstVarScope{flp, m_scopep, varp};
        m_classp->stmtsp()->addHereThisAsNext(varp);
        m_scopep->addVarsp(vscp);
        // replace the current node with
        AstReadMemProxy* proxyp
            = new AstReadMemProxy{nodep->fileline(), nodep->isHex(),
                                  new AstVarRef{flp, vscp, VAccess::READ} /*filenamep, abused?*/,
                                  nodep->memp()->cloneTree(false)};
        m_rmems.emplace_back(nodep, varp, m_instp);

        proxyp->dtypep(nodep->dtypep());
        nodep->replaceWith(proxyp);
    }
    void visit(AstNode* nodep) { iterateChildren(nodep); }

public:
    explicit PoplarPlusArgsVisitor(AstNetlist* netlistp)
        : m_hostFuncs("__VHostBspPlusArgFunc")
        , m_varNames("__VBspPlusArgVar") {
        m_netlistp = netlistp;
        m_substs.clear();

        m_netlistp->topModulep()->foreach([this](AstVarScope* vscp) {
            AstClassRefDType* clsRefp = VN_CAST(vscp->varp()->dtypep(), ClassRefDType);
            if (clsRefp && clsRefp->classp()->flag().isBsp()) {
                clsRefp->classp()->user1p(vscp);
                m_instp = vscp;
                visit(clsRefp->classp());
            }
        });

        // create function on the host that calls all the $plusarg functions
        // and caches their results
        AstCFunc* hostFuncSetp = new AstCFunc{netlistp->fileline(), "plusArgs",
                                              m_netlistp->topScopep()->scopep(), "void"};
        AstCFunc* hostFuncCopyp = new AstCFunc{netlistp->fileline(), "plusArgsCopy",
                                               m_netlistp->topScopep()->scopep(), "void"};
        hostFuncSetp->dontCombine(true);
        hostFuncCopyp->dontCombine(true);
        netlistp->topScopep()->scopep()->addBlocksp(hostFuncSetp);
        netlistp->topScopep()->scopep()->addBlocksp(hostFuncCopyp);
        for (const PlusArgSubst& subst : m_substs) {
            UASSERT(subst.firep, "need a firing condition!");
            FileLine* flp = subst.origp->fileline();
            auto appendCopy = [&subst, flp, &hostFuncCopyp](AstVar* lhsp, AstVarScope* rhsp) {
                AstMemberSel* memselp
                    = new AstMemberSel{flp, new AstVarRef{flp, subst.classInstp, VAccess::WRITE},
                                       VFlagChildDType{}, lhsp->name()};
                memselp->varp(lhsp);
                memselp->dtypep(lhsp->dtypep());
                AstAssign* assignp
                    = new AstAssign{flp, memselp, new AstVarRef{flp, rhsp, VAccess::READ}};
                hostFuncCopyp->addStmtsp(assignp);
            };

            // create a variable in the top module for the firing condition
            AstVarScope* fireHostp = m_netlistp->topScopep()->scopep()->createTemp(
                m_hostFuncs.get("testhost"), subst.firep->dtypep());
            appendCopy(subst.firep, fireHostp);

            if (subst.valp) {
                AstVarScope* hostValuep = m_netlistp->topScopep()->scopep()->createTemp(
                    m_hostFuncs.get("valuehost"), subst.valp->dtypep());
                appendCopy(subst.valp, hostValuep);

                AstNode* oldOutp = VN_AS(subst.origp, ValuePlusArgs)->outp();
                oldOutp->replaceWith(new AstVarRef{flp, hostValuep, VAccess::WRITE});
                oldOutp->deleteTree();
            }
            AstAssign* setCondp
                = new AstAssign{flp, new AstVarRef{flp, fireHostp, VAccess::WRITE}, subst.origp};
            hostFuncSetp->addStmtsp(setCondp);
        }

        AstCFunc* hostReadMemp = new AstCFunc{netlistp->fileline(), "readMem",
                                              m_netlistp->topScopep()->scopep(), "void"};
        AstCFunc* hostReadMemCopyp = new AstCFunc{netlistp->fileline(), "readMemCopy",
                                                  m_netlistp->topScopep()->scopep(), "void"};
        hostReadMemp->dontCombine(true);
        hostReadMemCopyp->dontCombine(true);
        m_netlistp->topScopep()->scopep()->addBlocksp(hostReadMemp);
        m_netlistp->topScopep()->scopep()->addBlocksp(hostReadMemCopyp);
        for (const ReadMemSubst& subst : m_rmems) {
            FileLine* flp = subst.origp->fileline();
            AstMemberSel* memselp
                = new AstMemberSel{flp, new AstVarRef{flp, subst.classInstp, VAccess::WRITE},
                                   VFlagChildDType{}, subst.hostMemp->name()};
            memselp->varp(subst.hostMemp);
            memselp->dtypep(subst.hostMemp->dtypep());
            AstVarScope* hostValuep = m_netlistp->topScopep()->scopep()->createTemp(
                m_hostFuncs.get("valuehost"), subst.hostMemp->dtypep());

            AstReadMem* newp = new AstReadMem{flp,
                                              subst.origp->isHex(),
                                              subst.origp->filenamep()->cloneTree(false),
                                              new AstVarRef{flp, hostValuep, VAccess::WRITE},
                                              nullptr,
                                              nullptr};
            hostReadMemp->addStmtsp(newp);
            subst.origp->deleteTree();
            hostReadMemCopyp->addStmtsp(
                new AstAssign{flp, memselp, new AstVarRef{flp, hostValuep, VAccess::READ}});
        }
    }
};
/// @brief create class member interfaces for host interactions.
/// This visitor looks for Display, Finish, and Stop nodes in the bsp classes
/// and replaces them with writes to class members. Then a hostHandle function
/// is created in the top module that handles the the system call in the host
/// Note that general DPI calls with return values is not supported.
class PoplarHostInteractionVisitor final : public VNVisitor {
private:
    AstNetlist* m_netlistp = nullptr;

    V3UniqueNames m_funcNames;
    V3UniqueNames m_memberName;
    struct ClassInfo {
        AstClass* m_classp = nullptr;
        AstVarScope* m_instp = nullptr;
        AstVarScope* m_classInteractionp = nullptr;
        AstVarScope* m_classCallId = nullptr;
        AstScope* m_scopep = nullptr;
        AstCFunc* m_hostHandlep = nullptr;
        int m_callId = 0;
        int m_numCalls = 0;
        void clear() {
            m_classp = nullptr;
            m_instp = nullptr;
            m_classInteractionp = nullptr;
            m_classCallId = nullptr;
            m_scopep = nullptr;
            m_numCalls = 0;
            m_callId = 0;
            m_hostHandlep = nullptr;
        }
    };
    std::vector<ClassInfo> m_info;
    ClassInfo m_ctx;

    AstVarScope* getInteractionTrigger() {
        UASSERT(m_ctx.m_classp, "not in class");
        UASSERT(m_ctx.m_scopep, "no scope!");
        if (!m_ctx.m_classInteractionp) {
            // create a new member in the class to which basically is
            // set whenever a host interaction is required
            AstVar* varp
                = new AstVar{m_ctx.m_classp->fileline(), VVarType::MEMBER,
                             m_memberName.get("needInteraction"), m_netlistp->findUInt32DType()};
            varp->bspFlag(VBspFlag{}
                              .append(VBspFlag::MEMBER_HOSTREAD)
                              .append(VBspFlag::MEMBER_OUTPUT)
                              .append(VBspFlag::MEMBER_HOSTREQ));
            AstVarScope* vscp = new AstVarScope{varp->fileline(), m_ctx.m_scopep, varp};
            UASSERT(m_ctx.m_classp->stmtsp(), "class with no vars!");
            m_ctx.m_classp->stmtsp()->addHereThisAsNext(varp);
            m_ctx.m_scopep->addVarsp(vscp);
            m_ctx.m_classInteractionp = vscp;
        }
        return m_ctx.m_classInteractionp;
    }
    AstVarScope* getInteractionId() {
        UASSERT(m_ctx.m_classp, "not in class!");
        UASSERT(m_ctx.m_scopep, "no scope!");
        if (!m_ctx.m_classCallId) {
            UASSERT(m_ctx.m_numCalls > 0, "should not create host interface!");
            AstVar* varp = new AstVar{
                m_ctx.m_classp->fileline(), VVarType::MEMBER, m_memberName.get("callId"),
                m_netlistp->findBitDType(m_ctx.m_numCalls,
                                         std::max(m_ctx.m_numCalls, VL_EDATASIZE),
                                         VSigning::UNSIGNED)};
            varp->bspFlag(
                VBspFlag{}.append(VBspFlag::MEMBER_HOSTREAD).append(VBspFlag::MEMBER_OUTPUT));
            AstVarScope* vscp = new AstVarScope{varp->fileline(), m_ctx.m_scopep, varp};
            UASSERT(m_ctx.m_classp->stmtsp(), "Class with no vars!");
            m_ctx.m_classp->stmtsp()->addHereThisAsNext(varp);
            m_ctx.m_scopep->addVarsp(vscp);
            m_ctx.m_classCallId = vscp;
        }
        return m_ctx.m_classCallId;
    }
    void makeHostHandle(AstNode* nodep) {
        UINFO(3, "Replacing " << nodep->shortName() << " with triggers" << endl);
        AstVarScope* triggerp = getInteractionTrigger();
        AstVarScope* callIdp = getInteractionId();

        FileLine* fl = nodep->fileline();
        AstAssign* setTriggerp = new AstAssign{fl, new AstVarRef{fl, triggerp, VAccess::WRITE},
                                               new AstConst{fl, AstConst::Unsized32{}, 1u}};
        AstAssign* setIdp = new AstAssign{
            fl, new AstSel{fl, new AstVarRef{fl, callIdp, VAccess::WRITE}, m_ctx.m_callId, 1},
            new AstConst{fl, AstConst::WidthedValue{}, 1, 1}};
        setTriggerp->addNext(setIdp);
        AstNode* oldp = nodep;
        nodep->replaceWith(setTriggerp);
        // create a function directly under the top module for handling this
        if (!m_ctx.m_hostHandlep) {
            m_ctx.m_hostHandlep
                = new AstCFunc{nodep->fileline(), m_funcNames.get(nodep->prettyName()),
                               m_netlistp->topScopep()->scopep(), "void"};
        }
        // create IF(classInst.memberCallId == (1 << callid)) DISPLAY under the host handle
        AstMemberSel* callSelp = new AstMemberSel{
            oldp->fileline(), new AstVarRef{oldp->fileline(), m_ctx.m_instp, VAccess::READ},
            VFlagChildDType{}, m_ctx.m_classCallId->name()};
        callSelp->varp(m_ctx.m_classCallId->varp());
        callSelp->dtypep(m_ctx.m_classCallId->dtypep());
        AstSel* selp = new AstSel{oldp->fileline(), callSelp, m_ctx.m_callId, 1};
        AstIf* hostIfp
            = new AstIf{oldp->fileline(),
                        new AstEq{oldp->fileline(), selp,
                                  new AstConst{oldp->fileline(), AstConst::WidthedValue{}, 1, 1}},
                        oldp, nullptr};
        m_ctx.m_hostHandlep->addStmtsp(hostIfp);
        m_ctx.m_callId++;
    }
    // VISITORS
    void visit(AstDisplay* nodep) override {
        // For every argument, create a class member. Alternatively, we could create
        // a single member for all the host functions within this class but we leave that
        // as an optimization, since it requires low-level indexing and casting.
        AstSFormatF* fmtp = nodep->fmtp();
        UASSERT(!fmtp->scopeNamep() || fmtp->scopeNamep()->forall([fmtp](const AstNode* nodep) {
            return VN_IS(nodep, Text) || fmtp->scopeNamep() == nodep;
        }),
                "did not expect op2 on AstFormatF " << fmtp << endl);
        std::vector<AstNodeExpr*> exprsp;
        for (AstNodeExpr* exprp = fmtp->exprsp(); exprp; exprp = VN_AS(exprp->nextp(), NodeExpr)) {
            exprsp.push_back(exprp);
        }
        std::vector<AstVar*> membersp;
        std::vector<AstAssign*> assignsp;
        for (AstNodeExpr* exprp : exprsp) {
            // create a class member
            AstVar* memberp = new AstVar{exprp->fileline(), VVarType::MEMBER,
                                         m_memberName.get("fmtArg"), exprp->dtypep()};
            membersp.push_back(memberp);
            // mark the variable as something that can be read by the host
            memberp->bspFlag(
                VBspFlag{}.append(VBspFlag::MEMBER_HOSTREAD).append(VBspFlag::MEMBER_OUTPUT));
            UASSERT(m_ctx.m_classp->stmtsp(), "Expected at least one stmt!");
            m_ctx.m_classp->stmtsp()->addHereThisAsNext(memberp);
            // need a variable scope as well
            AstVarScope* memVscp = new AstVarScope{exprp->fileline(), m_ctx.m_scopep, memberp};
            m_ctx.m_scopep->addVarsp(memVscp);
            // create an assignment member = exprp
            AstNodeExpr* unlinkp = exprp->unlinkFrBack();
            AstAssign* assignp = new AstAssign{
                exprp->fileline(), new AstVarRef{exprp->fileline(), memVscp, VAccess::WRITE},
                unlinkp};
            assignsp.push_back(assignp);
        }
        // add assigns right before the current node
        for (AstAssign* const assignp : vlstd::reverse_view(assignsp)) {
            nodep->addHereThisAsNext(assignp);
        }

        // replace the fmtp with a new one that directly references the members
        // of the current class responsible for the display
        for (AstVar* memberp : membersp) {
            AstMemberSel* exprp = new AstMemberSel{
                memberp->fileline(),
                new AstVarRef{memberp->fileline(), m_ctx.m_instp, VAccess::READ},
                VFlagChildDType{}, memberp->name()};
            exprp->dtypep(memberp->dtypep());
            exprp->varp(memberp);
            fmtp->addExprsp(exprp);
        }
        // now create a host handle
        makeHostHandle(nodep);
    }
    void visit(AstFinish* nodep) { makeHostHandle(nodep); }
    void visit(AstStop* nodep) { makeHostHandle(nodep); }

    void visit(AstScope* nodep) override {
        if (m_ctx.m_classp) {
            UASSERT(!m_ctx.m_scopep, "should not nest scopes!");
            m_ctx.m_scopep = nodep;
            iterateChildren(nodep);
        }
    }

    void visit(AstClass* classp) override {}
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

    void interactionAggregate() {
        FileLine* flp = m_netlistp->fileline();
        AstClass* newClsp = new AstClass{flp, m_memberName.get("condeval")};
        newClsp->classOrPackagep(
            new AstClassPackage{flp, V3BspSched::V3BspModules::builtinBaseClassPkg});
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
        AstClassRefDType* baseDTypep = nullptr;
        m_netlistp->foreach([&baseDTypep](AstClassRefDType* clsDTypep) {
            if (clsDTypep->classp()->name() == V3BspSched::V3BspModules::builtinBaseClass) {
                baseDTypep = clsDTypep;
            }
        });
        AstClassExtends* extendsp = new AstClassExtends{flp, nullptr, false};
        extendsp->dtypep(baseDTypep);
        newClsp->addExtendsp(extendsp);
        newClsp->isExtended(true);
        newClsp->level(4);
        newClsp->flag(VClassFlag{}
                          .append(VClassFlag::BSP_BUILTIN)
                          .append(VClassFlag::BSP_COND_BUILTIN)
                          .withTileId(0)
                          .withWorkerId(0));
        AstVar* classInstp
            = new AstVar{flp, VVarType::VAR, m_memberName.get("condevalinst"), dtypep};
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
        AstCFunc* exchp = nullptr;
        m_netlistp->topModulep()->foreach([&exchp](AstCFunc* funcp) {
            if (funcp->name() == "exchange") { exchp = funcp; }
        });

        compFuncp->dontCombine(true);
        compFuncp->isMethod(true);
        compFuncp->isInline(false);
        // for each host request, create new member variable + one extra member
        // variable that is the disjunction of the rest

        AstVar* disjunctVarp = new AstVar{flp, VVarType::MEMBER, m_memberName.get("disjucntion"),
                                          m_netlistp->findUInt32DType()};
        AstVarScope* disjunctVscp = new AstVarScope{flp, scopep, disjunctVarp};
        scopep->addVarsp(disjunctVscp);
        newClsp->addStmtsp(disjunctVarp);

        AstVar* tmpVarp = new AstVar{flp, VVarType::MEMBER, m_memberName.get("stacktemp"),
                                     m_netlistp->findUInt32DType()};
        AstVarScope* tmpVscp = new AstVarScope{flp, scopep, tmpVarp};
        scopep->addVarsp(tmpVscp);
        compFuncp->addStmtsp(tmpVarp);
        tmpVarp->funcLocal(true);
        compFuncp->addStmtsp(new AstAssign{flp, new AstVarRef{flp, tmpVscp, VAccess::WRITE},
                                           new AstConst{flp, AstConst::Unsized32{}, 0}});

        for (const auto info : m_info) {
            if (info.m_classp->flag().isBspInit()) continue;  // should not consider them here
            AstVar* condVarp = new AstVar{flp, VVarType::MEMBER, m_memberName.get("cond"),
                                          m_netlistp->findUInt32DType()};
            condVarp->bspFlag(VBspFlag{}.append(VBspFlag::MEMBER_INPUT));
            AstVarScope* condVscp = new AstVarScope{flp, scopep, condVarp};
            scopep->addVarsp(condVscp);
            newClsp->addStmtsp(condVarp);
            // clang-format off
            // OR condition
            compFuncp->addStmtsp(
                new AstAssign{
                    flp,
                    new AstVarRef{flp, tmpVscp, VAccess::WRITE},
                    new AstOr{flp,
                        new AstVarRef{flp, tmpVscp, VAccess::READ},
                        new AstVarRef{flp, condVscp, VAccess::READ}
                    }
                }
            );
            // clear on read
            compFuncp->addStmtsp(
                new AstAssign{
                    flp,
                    new AstVarRef{flp, condVscp, VAccess::WRITE},
                    new AstConst{flp, AstConst::Unsized32{}, 0}
                }
            );
            // clang-format on
            // add the exchange
            AstMemberSel* sourceSelp
                = new AstMemberSel{flp, new AstVarRef{flp, info.m_instp, VAccess::READ},
                                   VFlagChildDType{}, info.m_classInteractionp->name()};
            sourceSelp->varp(info.m_classInteractionp->varp());
            sourceSelp->dtypep(info.m_classInteractionp->varp()->dtypep());
            AstMemberSel* targetSelp
                = new AstMemberSel{flp, new AstVarRef{flp, instVscp, VAccess::WRITE},
                                   VFlagChildDType{}, condVarp->name()};
            targetSelp->varp(condVarp);
            targetSelp->dtypep(condVarp->dtypep());
            exchp->addStmtsp(new AstAssign{flp, targetSelp, sourceSelp});
        }
        // clang-format off
        compFuncp->addStmtsp(
            new AstAssign{
                flp,
                new AstVarRef{flp, disjunctVscp, VAccess::WRITE},
                new AstVarRef{flp, tmpVscp, VAccess::READ}
            }
        );
        // clang-format on
        disjunctVarp->bspFlag(VBspFlag{}
                                  .append(VBspFlag::MEMBER_HOSTREAD)
                                  .append(VBspFlag::MEMBER_OUTPUT)
                                  .append(VBspFlag::MEMBER_HOSTREQ)
                                  .append(VBspFlag::MEMBER_HOSTANYREQ));

        scopep->addBlocksp(compFuncp);
        newClsp->addStmtsp(scopep);
        m_netlistp->addModulesp(newClsp);
    }

public:
    explicit PoplarHostInteractionVisitor(AstNetlist* nodep)
        : m_funcNames{"__VbspHost"}
        , m_memberName{"__VbspHostInterface"} {
        UASSERT(v3Global.assertScoped(), "expected scopes, run before descoping");
        m_netlistp = nodep;

        // m_netlistp->topScopep()->scopep(), "void"

        AstCFunc* hostHandlep = new AstCFunc{m_netlistp->fileline(), "hostHandle",
                                             m_netlistp->topScopep()->scopep(), "void"};
        hostHandlep->dontCombine(true);
        // non-standard traversal, walk through the top level vars, if var is
        // of base class type, jump to class definition
        nodep->topModulep()->foreach([this, &hostHandlep](AstVarScope* vscp) {
            AstClassRefDType* clsRefp = VN_CAST(vscp->varp()->dtypep(), ClassRefDType);
            if (clsRefp && clsRefp->classp()->flag().isBsp()) {
                // jump to the class
                m_ctx.clear();
                m_memberName.reset();
                m_ctx.m_classp = clsRefp->classp();
                m_ctx.m_instp = vscp;
                m_ctx.m_classp->foreach([this](AstNodeStmt* stmtp) {
                    // count the number of calls to $stop, $finish, and $display
                    if (VN_IS(stmtp, Display) || VN_IS(stmtp, Finish) || VN_IS(stmtp, Stop)) {
                        m_ctx.m_numCalls += 1;
                    }
                });
                if (m_ctx.m_numCalls > 0) {
                    iterateChildren(m_ctx.m_classp);
                    m_info.push_back(m_ctx);
                    // reset trigger and callId
                    AstAssign* trigResetp
                        = new AstAssign{m_ctx.m_classInteractionp->fileline(),
                                        new AstVarRef{m_ctx.m_classInteractionp->fileline(),
                                                      m_ctx.m_classInteractionp, VAccess::WRITE},
                                        new AstConst{m_ctx.m_classInteractionp->fileline(),
                                                     AstConst::Unsized32{}, 0}};
                    AstAssign* callIdResetp = new AstAssign{
                        m_ctx.m_classCallId->fileline(),
                        new AstVarRef{m_ctx.m_classCallId->fileline(), m_ctx.m_classCallId,
                                      VAccess::WRITE},
                        new AstConst{m_ctx.m_classCallId->fileline(), AstConst::WidthedValue{},
                                     m_ctx.m_classCallId->width(), 0}};
                    AstCFunc* computep = nullptr;
                    m_ctx.m_classp->foreach([&computep](AstCFunc* funcp) {
                        if (funcp->name() == "compute") { computep = funcp; }
                    });
                    UASSERT(computep, "Could not fin compute method!");
                    // reset the host interfaces before starting computation
                    // we could potentially guard them as well with an unlikely
                    // condition on the trigger, but whatever...
                    computep->stmtsp()->addHereThisAsNext(callIdResetp);
                    computep->stmtsp()->addHereThisAsNext(trigResetp);
                    m_netlistp->topScopep()->scopep()->addBlocksp(m_ctx.m_hostHandlep);
                    AstMemberSel* selTrigp = new AstMemberSel{
                        m_ctx.m_classp->fileline(),
                        new AstVarRef{m_ctx.m_instp->fileline(), m_ctx.m_instp, VAccess::READ},
                        VFlagChildDType{}, m_ctx.m_classInteractionp->name()};
                    selTrigp->dtypep(m_ctx.m_classInteractionp->dtypep());
                    selTrigp->varp(m_ctx.m_classInteractionp->varp());
                    AstCCall* callHandlep
                        = new AstCCall{selTrigp->fileline(), m_ctx.m_hostHandlep, nullptr};
                    callHandlep->dtypeSetVoid();
                    AstIf* thisHandlep = new AstIf{
                        m_ctx.m_classp->fileline(),
                        new AstEq{m_ctx.m_classp->fileline(), selTrigp,
                                  new AstConst{selTrigp->fileline(), AstConst::Unsized32{}, 1}},
                        new AstStmtExpr{callHandlep->fileline(), callHandlep}};
                    hostHandlep->addStmtsp(thisHandlep);
                }
            }
        });
        interactionAggregate();
        // create a function that handles are host interactions
        m_netlistp->topScopep()->scopep()->addBlocksp(hostHandlep);
    }
};
/// @brief ensure no field name has leading underscores. This is required by the
/// poplar graph compiler and sadly all internally generated verilator names
/// have leading underscores. This makes every name quite long though
class PoplarLegalizeFieldNamesVisitor final {
public:
    explicit PoplarLegalizeFieldNamesVisitor(AstNetlist* netlistp) {
        netlistp->foreach([](AstClass* classp) {
            if (classp->flag().isBsp()) {
                classp->foreach([](AstVarScope* vscp) {
                    // AstNode::dedotName()
                    // const std::string newName
                    //     = vscp->scopep()->nameDotless() + "__ARROW__" + vscp->varp()->name();
                    vscp->varp()->name("BSP__" + vscp->varp()->name());
                });
            }
        });
    }
};
/// @brief  replaces AstVarRefs of members of the classes derived from the base
/// V3BspSched::V3BspModules::builtinBaseClass with AstVarRefView so that the code generation
/// can simply emit either a reinterpret_cast or a placement new. We do this because the poplar
/// classes could only have poplar::Vector<> as their members and these Vector are basically
/// opaque pointers for use. So we need to cast them to appropriate types.
class PoplarViewsVisitor final : public VNVisitor {
private:
    AstNetlist* m_netlistp = nullptr;
    AstClass* m_classp = nullptr;  // the class we are under

    // STATE
    //     AstVar::user1() -> true if top level class member
    //     AstVarRef::user1() -> true if processes
    VNUser1InUse m_user1Inuse;  // clear on AstClass
    uint32_t calcSize(AstNodeDType* dtp) {
        if (VN_IS(dtp, RefDType)) {
            return calcSize(dtp->skipRefp());
        } else if (VN_IS(dtp, BasicDType) || VN_IS(dtp, EnumDType)) {
            // hit bottom
            UASSERT_STATIC(VL_IDATASIZE == 32, "not sure if we can do none uint32_t data types");
            return dtp->widthWords();
        } else if (AstNodeArrayDType* arrayp = VN_CAST(dtp, NodeArrayDType)) {
            // auto dims = arrayp->dimensions(false /*do not include the basic types*/);
            // UINFO(10, "dimensions " << dtp << " " << dims.first << ", " << dims.second << " " <<
            // arrayp->elementsConst() << endl); UASSERT(dims.first == 0, "Not sure if I can do
            // unpack arrays yet! " << dtp << endl);
            return arrayp->elementsConst() * calcSize(arrayp->subDTypep());
        } else {
            UASSERT_OBJ(false, dtp, "Can not handle data type " << dtp << endl);
            return 0;
        }
    }
    // VISITORS
    AstVectorDType* vectorTypep(AstNodeDType* fromp) {

        uint32_t size = calcSize(fromp);
        UINFO(8, "compute size of " << fromp << " is " << size << endl);
        AstVectorDType* dtp
            = new AstVectorDType(fromp->fileline(), size,
                                 VN_AS(m_netlistp->typeTablep()->findUInt32DType(), BasicDType));
        m_netlistp->typeTablep()->addTypesp(dtp);
        return dtp;
    }
    void visit(AstVarRef* vrefp) override {

        if (!m_classp) { return; }
        if (!vrefp->varp()->user1()) { return; }
        if (vrefp->user1()) { return; /*processed*/ }
        vrefp->user1(true);
        // wrap the reference in an AstVarView, this is essential for poplar since
        // the member variables are Vectors of contiguous data and we wish to
        // view them as Verilator data types, like VlWide and VlUnpackArray
        VNRelinker relinkHandle;
        vrefp->unlinkFrBack(&relinkHandle);
        UINFO(100, "Wrapping " << vrefp->name() << " in AstVarRefView" << endl);
        AstVarRefView* newp = new AstVarRefView{vrefp->fileline(), vrefp};
        relinkHandle.relink(newp);
    }

    void visit(AstClass* nodep) override {
        VL_RESTORER(m_classp);
        if (nodep->flag().isBsp()) {
            m_classp = nodep;
            AstNode::user1ClearTree();
            std::vector<AstVar*> members;
            for (AstNode* stmtp = nodep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                if (AstVar* varp = VN_CAST(stmtp, Var)) {
                    varp->user1(true);
                    members.push_back(varp);
                    varp->dtypep(vectorTypep(varp->dtypep()));
                }
            }
            // change all member dtypes to POPLAR_VECTOR_UINT32

            iterateChildren(nodep);
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit PoplarViewsVisitor(AstNetlist* nodep) {
        m_netlistp = nodep;
        iterate(nodep);
    }
};

class PoplarComputeGraphBuilder final : public VNDeleter {
private:
    AstNetlist* m_netlistp = nullptr;
    V3UniqueNames m_newNames;
    AstBasicDType* m_ctxTypep = nullptr;
    AstBasicDType* m_tensorTypep = nullptr;
    AstBasicDType* m_vtxRefTypep = nullptr;
    AstVar* m_ctxVarp = nullptr;
    AstVarScope* m_ctxVscp = nullptr;

    struct TensorHandle {
        std::string tensor;
        std::string hostRead;
        std::string hostWrite;
        bool isReq;
        TensorHandle() {
            tensor.erase();
            hostRead.erase();
            hostWrite.erase();
            isReq = false;
        }
    };
    VNUser1InUse m_user1InUse;
    AstUser1Allocator<AstVar, TensorHandle> m_handles;

    void initBuiltinTypes() {
        auto newType = [this](VBasicDTypeKwd kwd) {
            AstBasicDType* const typep
                = new AstBasicDType{m_netlistp->fileline(), kwd, VSigning::UNSIGNED};
            m_netlistp->typeTablep()->addTypesp(typep);
            return typep;
        };
        m_ctxTypep = newType(VBasicDTypeKwd::POPLAR_CONTEXT);
        m_tensorTypep = newType(VBasicDTypeKwd::POPLAR_TENSOR);
        m_vtxRefTypep = newType(VBasicDTypeKwd::POPLAR_VERTEXREF);
    }

    AstVarScope* createFuncVar(FileLine* fl, AstCFunc* funcp, const std::string& name,
                               AstBasicDType* dtp) {

        AstVar* varp = new AstVar{fl, VVarType::VAR, name, dtp};
        varp->funcLocal(true);
        AstVarScope* vscp = new AstVarScope{fl, funcp->scopep(), varp};
        funcp->scopep()->addVarsp(vscp);
        funcp->addStmtsp(varp);
        return vscp;
    }

    AstCFunc* getFunc(AstNodeModule* topModp, const std::string& name) {
        AstCFunc* foundp = nullptr;
        topModp->foreach([&foundp, &name](AstCFunc* funcp) {
            if (funcp->name() == name) { foundp = funcp; }
        });
        UASSERT(foundp, "Could not find function name " << name << endl);
        return foundp;
    }

    AstCMethodHard* mkCall(FileLine* fl, const std::string& name,
                           const std::vector<AstNodeExpr*>& argsp, AstNodeDType* dtp = nullptr) {
        AstCMethodHard* callp
            = new AstCMethodHard{fl, new AstVarRef{fl, m_ctxVscp, VAccess::READWRITE}, name};
        for (const auto ap : argsp) { callp->addPinsp(ap); }
        if (dtp) {
            callp->dtypep(dtp);
        } else {
            callp->dtypeSetVoid();
        }
        return callp;
    };

    AstCFunc* createVertexCons(AstClass* classp, uint32_t tileId) {
        FileLine* fl = classp->fileline();
        AstCFunc* ctorp = new AstCFunc{classp->fileline(), "ctor_" + classp->name(),
                                       m_netlistp->topScopep()->scopep(), "void"};
        ctorp->isInline(false);
        AstVarScope* vtxVscp = createFuncVar(fl, ctorp, m_newNames.get("instance"), m_vtxRefTypep);
        auto className = EmitCBaseVisitor::prefixNameProtect(classp);
        AstAssign* mkVtx = new AstAssign{
            fl, new AstVarRef{fl, vtxVscp, VAccess::WRITE},
            mkCall(fl, "getOrAddVertex",
                   {new AstConst{fl, AstConst::String{}, className},
                    new AstConst{fl, AstConst::String{},
                                 classp->flag().isBspInit()
                                     ? "init"
                                     : classp->flag().isBspCond() ? "condeval" : "compute"}},
                   m_vtxRefTypep)};
        ctorp->addStmtsp(mkVtx);
        auto setTileMapping = [this, &fl, &ctorp](AstVarScope* vscp, uint32_t tid) {
            AstStmtExpr* tileMapp
                = new AstStmtExpr{fl, mkCall(fl, "setTileMapping",
                                             {new AstVarRef{fl, vscp, VAccess::READWRITE},
                                              new AstConst{fl, AstConst::Unsized32{}, tid}})};
            ctorp->addStmtsp(tileMapp);
        };

        setTileMapping(vtxVscp, tileId);

        AstStmtExpr* perfEstp
            = new AstStmtExpr{fl, mkCall(fl, "setPerfEstimate",
                                         {new AstVarRef{fl, vtxVscp, VAccess::READWRITE},
                                          new AstConst{fl, AstConst::Unsized32{}, 0}})};
        ctorp->addStmtsp(perfEstp);
        // iterate through the class members and create tensors
        for (AstNode* nodep = classp->stmtsp(); nodep; nodep = nodep->nextp()) {
            AstVar* varp = VN_CAST(nodep, Var);
            if (!varp) continue;
            AstVectorDType* dtp = VN_CAST(varp->dtypep(), VectorDType);
            UASSERT_OBJ(dtp, varp, "expected VectorDType, need to create AstVarRefViews first!");
            UASSERT_OBJ(dtp->basicp()->keyword() == VBasicDTypeKwd::UINT32, dtp, "expeced UINT32");
            UASSERT_OBJ(varp->isClassMember(), varp, "Expected class member");
            // create a tensor for this variable
            AstVarScope* tensorVscp
                = createFuncVar(fl, ctorp, m_newNames.get("tensor"), m_tensorTypep);
            std::string tensorDeviceHandle = className + "." + varp->nameProtect();
            m_handles(varp).tensor
                = tensorDeviceHandle;  // need this to be able to later look up the tensor
            AstAssign* mkTensorp = new AstAssign{
                fl, new AstVarRef{fl, tensorVscp, VAccess::WRITE},
                mkCall(fl, "addTensor",
                       {new AstConst{fl, AstConst::Unsized32{}, dtp->size()},
                        new AstConst{fl, AstConst::String{}, tensorDeviceHandle}})};
            ctorp->addStmtsp(mkTensorp);
            setTileMapping(tensorVscp, tileId);
            // connect the tensor to the vertex
            ctorp->addStmtsp(new AstStmtExpr{
                fl, mkCall(fl, "connect",
                           {new AstVarRef{fl, vtxVscp, VAccess::READWRITE},
                            new AstConst{fl, AstConst::String{}, varp->nameProtect()},
                            new AstVarRef{fl, tensorVscp, VAccess::READWRITE}})});
            // check whether we need to create host read/write handles
            if (varp->bspFlag().hasHostRead()) {
                const std::string hrHandle = "hr." + tensorDeviceHandle;
                m_handles(varp).hostRead = hrHandle;
                ctorp->addStmtsp(new AstStmtExpr{
                    fl, mkCall(fl, "createHostRead",
                               {new AstConst{fl, AstConst::String{}, hrHandle},
                                new AstVarRef{fl, tensorVscp, VAccess::READWRITE},
                                new AstConst{fl, AstConst::Unsized32{}, dtp->size()}},
                               nullptr)});

                if (varp->bspFlag().hasHostReq()) {
                    ctorp->addStmtsp(
                        new AstStmtExpr{fl, mkCall(fl, "isHostRequest",
                                                   {new AstVarRef{fl, tensorVscp, VAccess::READ},
                                                    new AstConst{fl, AstConst::BitTrue{},
                                                                 varp->bspFlag().hasAnyHostReq()}},
                                                   nullptr)});
                }
            }
            if (varp->bspFlag().hasHostWrite()) {
                const std::string hwHandle = "hw." + tensorDeviceHandle;
                m_handles(varp).hostWrite = hwHandle;
                ctorp->addStmtsp(new AstStmtExpr{
                    fl, mkCall(fl, "createHostWrite",
                               {new AstConst{fl, AstConst::String{}, hwHandle},
                                new AstVarRef{fl, tensorVscp, VAccess::READWRITE},
                                new AstConst{fl, AstConst::Unsized32{}, dtp->size()}},
                               nullptr)});
            }
        }

        return ctorp;
    }

    void addCopies(AstCFunc* cfuncp, const bool isInit) {

        for (AstNode* nodep = cfuncp->stmtsp(); nodep;) {
            UASSERT(VN_IS(nodep, Assign), "expected AstAssign");
            AstAssign* const assignp = VN_AS(nodep, Assign);
            AstVar* const top = VN_AS(assignp->lhsp(), MemberSel)->varp();
            AstVar* const fromp = VN_AS(assignp->rhsp(), MemberSel)->varp();
            // get the handles from the user1
            auto toHandle = m_handles(top).tensor;
            UASSERT(!toHandle.empty(), "handle not set!");
            auto fromHandle = m_handles(fromp).tensor;
            UASSERT(!fromHandle.empty(), "handle not set!");
            AstNode* newp = new AstStmtExpr{
                nodep->fileline(),
                mkCall(assignp->fileline(), "addCopy",
                       {new AstConst{nodep->fileline(), AstConst::String{}, fromHandle} /*source*/,
                        new AstConst{nodep->fileline(), AstConst::String{}, toHandle} /*target*/,
                        new AstConst{nodep->fileline(), AstConst::Unsized32{},
                                     static_cast<uint32_t>(top->widthWords())} /*number of words*/,
                        new AstConst{nodep->fileline(), AstConst::BitTrue{},
                                     isInit} /*is it part of init*/})};
            nodep->replaceWith(newp);
            VL_DO_DANGLING(nodep->deleteTree(), nodep);
            nodep = newp->nextp();
        }
    }

    void addInitConstCopies(AstCFunc* cfuncp) {
        for (AstNode* nodep = cfuncp->stmtsp(); nodep;) {
            UASSERT(VN_IS(nodep, Assign), "expected assign");
            AstAssign* const assignp = VN_AS(nodep, Assign);
            AstVar* const top = VN_AS(assignp->lhsp(), MemberSel)->varp();
            AstVarRef* const fromp = VN_AS(assignp->rhsp(), VarRef);
            auto toHandle = m_handles(top).hostWrite;
            AstStmtExpr* newp = new AstStmtExpr{
                nodep->fileline(),
                mkCall(assignp->fileline(), "setHostData",
                       {
                           new AstConst{nodep->fileline(), AstConst::String{}, toHandle},
                           fromp->cloneTree(false),
                       })};
            nodep->replaceWith(newp);
            VL_DO_DANGLING(nodep->deleteTree(), nodep);
            nodep = newp->nextp();
        }
    }
    void patchHostHandle() {
        // find the host handle top function
        AstCFunc* hostp = getFunc(m_netlistp->topModulep(), "hostHandle");
        // find any other reachable function from hostp;
        std::set<AstCFunc*> reachablep;
        std::queue<AstCFunc*> toVisitp;
        toVisitp.push(hostp);
        int depth = 0;
        while (!toVisitp.empty()) {
            UASSERT(depth++ < 100000, "something is up");
            AstCFunc* toCheckp = toVisitp.front();
            toVisitp.pop();
            if (reachablep.find(toCheckp) != reachablep.end()) continue;
            toCheckp->foreach([&toVisitp, &reachablep](AstCCall* callp) {
                auto foundIt = reachablep.find(callp->funcp());
                if (foundIt == reachablep.end()) { toVisitp.push(callp->funcp()); }
            });
            reachablep.insert(toCheckp);
        }

        for (AstCFunc* nodep : reachablep) {
            // replace all the AstMemberSel with calls to appropriate members
            // to PoplarContext
            nodep->foreach([this](AstMemberSel* memselp) {
                // MEMBERSEL cls.var becomes ctx.getHostData<dtype>(var, dtype{})
                // memselp->dtypep() still has the "old" host side datatype
                // not the vector type that PoplarViewsVisitor creates in BSP
                // classes. Consider it a bug (since type information is broken) or a feature
                // (since things become easier here!)
                auto handle = m_handles(memselp->varp()).hostRead;
                UASSERT(!handle.empty(), "empty handle!");
                AstCMethodHard* callp
                    = mkCall(memselp->fileline(),
                             "getHostData", /*getHostData is a templated function, we use a empty
                                               constant to help the C++ compiler infer the type*/
                             {new AstConst{memselp->fileline(), AstConst::String{}, handle},
                              new AstConst{memselp->fileline(), AstConst::DTyped{},
                                           memselp->dtypep()} /*used to resolve template types*/},
                             memselp->dtypep());
                memselp->replaceWith(callp);
                VL_DO_DANGLING(pushDeletep(memselp), memselp);
            });
        }
    }

    // create a vertex that ORs all the needInteraction signals. Ideally this
    // vertex should be a multivertex for better performance, but I'll leave that
    // for later.

public:
    explicit PoplarComputeGraphBuilder(AstNetlist* nodep)
        : m_newNames{"__VPoplar"} {
        m_netlistp = nodep;
        // AstClass*
        initBuiltinTypes();

        m_ctxVarp = new AstVar{m_netlistp->fileline(), VVarType::VAR, "ctx", m_ctxTypep};
        m_netlistp->topModulep()->stmtsp()->addHereThisAsNext(m_ctxVarp);
        m_ctxVscp = new AstVarScope{m_netlistp->fileline(), m_netlistp->topScopep()->scopep(),
                                    m_ctxVarp};
        m_netlistp->topScopep()->scopep()->addVarsp(m_ctxVscp);

        // Step 1.
        // go through each class and create constructors. All that happens here
        // depends on a hard coded PoplarContext that provides a few methods
        // for constructing graphs from codelets and connecting tensors to vertices
        AstNode::user1ClearTree();
        AstCFunc* constructAllp = new AstCFunc{m_netlistp->fileline(), "constructAll",
                                               m_netlistp->topScopep()->scopep(), "void"};
        constructAllp->isMethod(true);
        constructAllp->dontCombine(true);
        m_netlistp->topScopep()->scopep()->addBlocksp(constructAllp);

        m_netlistp->foreach([this, &constructAllp](AstClass* classp) {
            // go through each deriviation of the base bsp class and create
            // host constructors
            if (!classp->flag().isBsp()) { return; /*some other class*/ }
            AstCFunc* ctorp = createVertexCons(classp, classp->flag().tileId());
            m_netlistp->topScopep()->scopep()->addBlocksp(ctorp);

            AstCCall* callp = new AstCCall{ctorp->fileline(), ctorp};
            callp->dtypeSetVoid();
            constructAllp->addStmtsp(new AstStmtExpr{ctorp->fileline(), callp});
        });
        // add the plusArgs function right after construction, this will set a bunch
        // of host variables later needed to be copied to the user vertcies
        // copy the cache values of args to the tensors on startup
        AstCFunc* plusArgsCopyp = getFunc(m_netlistp->topModulep(), "plusArgsCopy");
        addInitConstCopies(plusArgsCopyp);
        // same goes for readmem
        AstCFunc* readMemCopyp = getFunc(m_netlistp->topModulep(), "readMemCopy");
        addInitConstCopies(readMemCopyp);
        // AstCCall* plusArgsCopyCallp = new AstCCall{plusArgsCopyp->fileline(), plusArgsCopyp};
        // plusArgsCopyCallp->dtypeSetVoid();
        // constructAllp->addStmtsp(
        //     new AstStmtExpr{plusArgsCopyCallp->fileline(), plusArgsCopyCallp});
        // Step 2.
        // create a poplar program with the following structure:
        // Add the copy operations
        m_netlistp->foreach([this](AstCFunc* cfuncp) {
            if (cfuncp->name() == "exchange" || cfuncp->name() == "initialize") {
                // create copy operations
                addCopies(cfuncp, cfuncp->name() == "initialize");
            }
        });
        patchHostHandle();
        // remove the computeSet funciton, not used
        getFunc(m_netlistp->topModulep(), "computeSet")->unlinkFrBack()->deleteTree();
    }
};
void V3BspPoplarProgram::createProgram(AstNetlist* nodep) {
    // reoder passes only if you know what you are doing
    UINFO(3, "Creating poplar program" << endl);
    { PoplarSetTileAndWorkerVisitor{nodep}; }
    { PoplarPlusArgsVisitor{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspPlusArg", 0, dumpTree() >= 1);
    { PoplarHostInteractionVisitor{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspPoplarHost", 0, dumpTree() >= 1);
    { PoplarLegalizeFieldNamesVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("bspLegal", 0, dumpTree() >= 1);
    { PoplarViewsVisitor{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspPoplarView", 0, dumpTree() >= 1);
    { PoplarComputeGraphBuilder{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bscPoplarProgram", 0, dumpTree() >= 1);
}
