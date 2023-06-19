// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Create a $plusarg cache
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

#include "V3BspPlusArgs.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

VL_DEFINE_DEBUG_FUNCTIONS;

class PlusArgsCacheVisitor final : public VNVisitor {
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

    void visit(AstNode* nodep) { iterateChildren(nodep); }

public:
    explicit PlusArgsCacheVisitor(AstNetlist* netlistp)
        : m_hostFuncs(VL_UNIQUENAMES("fn"))
        , m_varNames(VL_UNIQUENAMES("vr")) {
        m_netlistp = netlistp;
        m_substs.clear();

        m_netlistp->topModulep()->foreach([this](AstVarScope* vscp) {
            AstClassRefDType* clsRefp = VN_CAST(vscp->varp()->dtypep(), ClassRefDType);

            UINFO(15, "Visiting " << vscp->prettyName() << endl);
            if (clsRefp && clsRefp->classp()->flag().isBsp()) {

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

void V3BspPlusArgs::makeCache(AstNetlist* netlistp) {

    { PlusArgsCacheVisitor{netlistp}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspPlusArg", 0, dumpTree() >= 1);
}