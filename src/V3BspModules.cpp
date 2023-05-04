// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Bulk-synchronous parallel class creation
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

#include "V3BspModules.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3BspGraph.h"
#include "V3UniqueNames.h"

#include <algorithm>
VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3BspSched {
namespace {

class VarScopeReferences {
private:
    DepGraph* m_producer = nullptr;
    std::vector<DepGraph*> m_consumer;
    std::pair<AstVarScope*, AstVar*> m_sourcep;
    std::vector<std::pair<AstVarScope*, AstVar*>> m_targets;
    std::pair<AstVarScope*, AstVar*> m_initp;

public:
    inline bool isClocked() const { return m_producer != nullptr; }
    inline bool isOwned(const std::unique_ptr<DepGraph>& graphp) const {
        return graphp.get() == m_producer;
    }

    inline bool isLocal() const {
        return !m_producer || (m_consumer.size() == 1 && m_consumer.front() == m_producer);
    }
    inline bool hasConsumer() const { return !m_consumer.empty(); }
    inline bool isConsumed(const std::unique_ptr<DepGraph>& graphp) const {
        return std::find_if(m_consumer.begin(), m_consumer.end(),
                            [&graphp](const DepGraph* other) { return other == graphp.get(); })
               != m_consumer.end();
    }
    inline bool isRemote(const std::unique_ptr<DepGraph>& graphp) const {
        return !isLocal() && !isOwned(graphp);
    }
    inline void producer(const std::unique_ptr<DepGraph>& graphp) {
        UASSERT(!m_producer || m_producer == graphp.get(), "multiple producers!");
        m_producer = graphp.get();
    }

    void sourcep(const std::pair<AstVarScope*, AstVar*>& nodep) {
        UASSERT(!m_sourcep.first && !m_sourcep.second, "source end point already set!");
        UASSERT(nodep.first && nodep.second, "expected non-null");
        m_sourcep = nodep;
    }
    void initp(const std::pair<AstVarScope*, AstVar*>& nodep) {
        UASSERT(!m_initp.first && !m_initp.second, "already init!");
        UASSERT(nodep.first && nodep.second, "should not be null");
        m_initp = nodep;
    }
    std::pair<AstVarScope*, AstVar*> initp() const { return m_initp; }

    std::pair<AstVarScope*, AstVar*> sourcep() const { return m_sourcep; }

    void addTargetp(const std::pair<AstVarScope*, AstVar*>& nodep) { m_targets.push_back(nodep); }
    std::vector<std::pair<AstVarScope*, AstVar*>>& targetsp() { return m_targets; }

    DepGraph* producer() const { return m_producer; }
    void consumer(const std::unique_ptr<DepGraph>& graphp) { m_consumer.push_back(graphp.get()); }
    const std::vector<DepGraph*>& consumer() const { return m_consumer; }
    VarScopeReferences() {
        m_sourcep = std::make_pair(nullptr, nullptr);
        m_initp = std::make_pair(nullptr, nullptr);
    }
    ~VarScopeReferences() = default;
};

class ModuleBuilderImpl final : public VNDeleter {
private:
    // NODE STATE
    //      VarScope::user1     -> consumers and producer of the variable
    VNUser1InUse user1InUse;
    VNUser2InUse user2InUse;
    VNUser3InUse user3InUse;
    AstUser1Allocator<AstVarScope, VarScopeReferences> m_vscpRefs;
    V3UniqueNames m_modNames;
    AstNetlist* m_netlistp;  // original netlist
    const std::vector<std::unique_ptr<DepGraph>>& m_partitionsp;  // partitions
    const V3Sched::LogicByScope m_initials;
    const V3Sched::LogicByScope m_initialStatics;

    AstModule* m_topModp = nullptr;
    AstPackage* m_packagep = nullptr;

    AstClass* m_classWithComputep = nullptr;
    AstClassRefDType* m_classWithComputeDtypep = nullptr;
    AstClass* m_classWithInitp = nullptr;
    AstClassRefDType* m_classWithInitDTypep = nullptr;

    AstScope* m_packageScopep = nullptr;
    AstCell* m_packageCellp = nullptr;
    AstScope* m_topScopep = nullptr;
    std::string m_scopePrefix;

    // compute the references to each variable
    void computeReferences() {
        AstNode::user1ClearTree();
        //  first go through the active (clocked) partitions
        for (const auto& graphp : m_partitionsp) {
            UINFO(100, "Inspecting graph " << graphp.get() << endl);
            for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                if (ConstrDefVertex* const defp = dynamic_cast<ConstrDefVertex*>(vtxp)) {
                    UINFO(100, "consumed: " << defp->vscp()->name() << endl);
                    m_vscpRefs(defp->vscp()).consumer(graphp);
                } else if (CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp)) {
                    if (VN_IS(compp->nodep(), AssignPost) || VN_IS(compp->nodep(), AlwaysPost)) {
                        // this is a commit node whose variables appears in the LHS of some post
                        // assignment
                        compp->nodep()->foreach([this, compp, &graphp](const AstVarRef* vrefp) {
                            if (vrefp->access().isWriteOrRW()) {
                                UINFO(100, "produced: " << vrefp->varScopep()->name() << " from "
                                                        << cvtToHex(compp->nodep()) << endl);
                                m_vscpRefs(vrefp->varScopep()).producer(graphp);
                            }
                        });
                    }
                } else if (ConstrCommitVertex* const commitp
                           = dynamic_cast<ConstrCommitVertex*>(vtxp)) {
                    if (commitp->outEmpty()) {
                        UINFO(100,
                              "produced: " << commitp->vscp()->name() << " from commit" << endl);
                        m_vscpRefs(commitp->vscp()).producer(graphp);
                    } else {
                        // Leads to an LHS of a post assignment which is handled above.
                        // Note that a commit node with both and incoming edge and outgoing
                        // edge is considered duplicable and hence is not considered as "produced"
                        // since production should be unique to a single partition
                    }
                }
            }
        }
    }

    std::pair<AstClass*, AstClassRefDType*> newClass(FileLine* fl, const std::string& name,
                                                     const std::string& pkgName) {
        AstClass* newClsp = new AstClass{fl, name};
        newClsp->classOrPackagep(new AstClassPackage{fl, pkgName});
        newClsp->classOrPackagep()->classp(newClsp);
        AstClassRefDType* dtypep = new AstClassRefDType{fl, newClsp, nullptr};
        dtypep->classOrPackagep(m_packagep);
        m_netlistp->typeTablep()->addTypesp(dtypep);
        return {newClsp, dtypep};
    }

    void makeBaseClasses() {

        auto clsAndTpe
            = newClass(new FileLine(FileLine::builtInFilename()), V3BspModules::builtinBaseClass,
                       V3BspModules::builtinBaseClassPkg);
        m_classWithComputep = clsAndTpe.first;
        m_classWithComputep->isVirtual(true);

        m_classWithComputeDtypep = clsAndTpe.second;

        auto initClsAndTypep
            = newClass(new FileLine(FileLine::builtInFilename()),
                       V3BspModules::builtinBaseInitClass, V3BspModules::builtinBaseClassPkg);
        m_classWithInitp = initClsAndTypep.first;
        m_classWithInitDTypep = initClsAndTypep.second;
        m_classWithInitp->isVirtual(true);
    }

    // make a single class representing the parallel computation in each graph
    AstClass* makeClass(const std::unique_ptr<DepGraph>& graphp) {
        UASSERT(m_packagep, "need bsp package!");
        FileLine* fl = nullptr;
        // get a better fileline
        for (V3GraphVertex* itp = graphp->verticesBeginp(); itp; itp = itp->verticesNextp()) {
            if (CompVertex* vtxp = dynamic_cast<CompVertex*>(itp)) {
                if (VN_IS(vtxp->nodep(), AlwaysPost) || VN_IS(vtxp->nodep(), AssignPost)) {
                    fl = vtxp->nodep()->fileline();
                } else if (!fl) {
                    fl = vtxp->nodep()->fileline();
                }
            }
        }

        // create a class for the the graph partition
        auto classAndTypep = newClass(fl, m_modNames.get("vtxCls"), m_modNames.get("vtxClsPkg"));
        AstClass* classp = classAndTypep.first;
        AstClassRefDType* classTypep = classAndTypep.second;
        AstClassExtends* extendsp = new AstClassExtends{fl, nullptr, false};

        extendsp->dtypep(m_classWithComputeDtypep);
        classp->addExtendsp(extendsp);
        classp->isExtended(true);

        AstVar* classInstp = new AstVar{fl, VVarType::VAR, m_modNames.get("vtxInst"), classTypep};
        classInstp->lifetime(VLifetime::STATIC);
        // add the instance to the scope of the top module
        AstVarScope* instVscp = new AstVarScope{classInstp->fileline(), m_topScopep, classInstp};
        m_topScopep->addVarsp(instVscp);
        m_topModp->addStmtsp(classInstp);
        // this class will represent the code that runs on one core
        classp->level(4);  // lives under the BspPkg
        classp->flag(VClassFlag{}.append(VClassFlag::BSP_BUILTIN));
        // create a scope for the class
        AstScope* scopep = new AstScope{fl, classp, m_scopePrefix + classp->name(),
                                        m_packageScopep, m_packageCellp};
        AstCFunc* cfuncp = new AstCFunc{fl, "compute", scopep, "void"};
        cfuncp->dontCombine(true);
        cfuncp->isMethod(true);
        cfuncp->isInline(true);

        // create member variables for the class
        // STATE
        // VarScope::user2   -> true if already processed
        // VarScope::user3p  -> new var scope inside the class
        AstNode::user2ClearTree();
        AstNode::user3ClearTree();

        std::vector<AstVarScope*> inputsp;
        std::vector<AstVarScope*> outputsp;
        std::vector<AstVarScope*> localsp;

        for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            if (ConstrVertex* constrp = dynamic_cast<ConstrVertex*>(vtxp)) {
                AstVarScope* vscp = constrp->vscp();
                if (!vscp->user2()) {
                    // add any variable reference in the partition to the local scope
                    // for any local variable that is produced by another graph we need
                    // to create an input variable
                    auto& refInfo = m_vscpRefs(vscp);
                    AstVar* varp = new AstVar{vscp->varp()->fileline(), VVarType::MEMBER,
                                              vscp->varp()->name(), vscp->varp()->dtypep()};
                    varp->lifetime(VLifetime::AUTOMATIC);
                    AstVarScope* newVscp = new AstVarScope{vscp->fileline(), scopep, varp};
                    newVscp->trace(vscp->isTrace());
                    scopep->addVarsp(newVscp);
                    vscp->user3p(newVscp);
                    if (VN_IS(vscp->dtypep()->skipRefp(), BasicDType)
                        || VN_IS(vscp->dtypep()->skipRefp(), UnpackArrayDType)) {

                        if (refInfo.isOwned(graphp) && refInfo.isLocal()) {
                            // the variable is produced here and does not need
                            // to be sent out, however we should create a
                            // persistent class member for it to keep it alive
                            // after the function goes out of scope
                            classp->addStmtsp(varp);
                            refInfo.addTargetp(std::make_pair(instVscp, varp));
                        } else if (refInfo.isOwned(graphp) && !refInfo.isLocal()) {
                            // variable is owed/produced here but also referenced
                            // by others
                            classp->addStmtsp(varp);
                            // need to send it
                            UASSERT(!refInfo.sourcep().first, "multiple producers!");
                            refInfo.sourcep(std::make_pair(instVscp, varp));
                        } else if (refInfo.isClocked() || refInfo.initp().first) {
                            UASSERT_OBJ(refInfo.isConsumed(graphp), vscp, "Unexpected reference!");
                            // not produced here but consumed
                            classp->addStmtsp(varp);
                            // need to recieve it
                            refInfo.addTargetp(std::make_pair(instVscp, varp));
                        } else {
                            // temprorary variables, lifetime limited to the scope
                            // of the enclosing function
                            cfuncp->addStmtsp(varp);
                            varp->funcLocal(true);
                        }
                    } else {
                        vscp->v3fatalSrc("Unknown data type" << vscp->dtypep());
                    }
                    vscp->user2(true);  // mark visited
                }
            }
        }

        // add the computation
        UINFO(5, "Ordering computation" << endl);
        graphp->order();  // order the computation
        std::vector<AstNode*> stmtps;
        for (V3GraphVertex* itp = graphp->verticesBeginp(); itp; itp = itp->verticesNextp()) {
            if (CompVertex* vtxp = dynamic_cast<CompVertex*>(itp)) {
                // vtxp->nodep
                AstNode* nodep = vtxp->nodep();
                UASSERT(nodep, "nullptr vertex");
                UASSERT_OBJ(VN_IS(nodep, Always) || VN_IS(nodep, AlwaysPost)
                                || VN_IS(nodep, AssignPost) || VN_IS(nodep, AssignPre)
                                || VN_IS(nodep, AssignW) || VN_IS(nodep, AssignAlias),
                            nodep, "unexpected node type " << nodep->prettyTypeName() << endl);
                if (AstNodeProcedure* alwaysp = VN_CAST(nodep, NodeProcedure)) {
                    for (AstNode* np = alwaysp->stmtsp(); np; np = np->nextp()) {
                        UINFO(10, "Cloning " << np << endl);
                        stmtps.push_back(np->cloneTree(false));
                    }
                } else {
                    UINFO(10, "Cloning " << nodep << endl);
                    stmtps.push_back(nodep->cloneTree(false));
                }
            }
        }
        for (AstNode* nodep : stmtps) {
            nodep->foreach([this](AstVarRef* vrefp) {
                // replace with the new variables
                UASSERT_OBJ(vrefp->varScopep()->user3p(), vrefp->varScopep(), "Expected user3p");
                AstVarScope* substp = VN_AS(vrefp->varScopep()->user3p(), VarScope);
                // replace the reference
                AstVarRef* newp = new AstVarRef{vrefp->fileline(), substp, vrefp->access()};
                vrefp->replaceWith(newp);
                VL_DO_DANGLING(pushDeletep(vrefp), vrefp);
            });
            cfuncp->addStmtsp(nodep);
        }
        scopep->addBlocksp(cfuncp);
        classp->addStmtsp(scopep);
        return classp;
    }

    void checkBuiltinNotUsed() const {
        m_netlistp->foreach([this](AstPackage* pkgp) {
            if (pkgp->name() == V3BspModules::builtinBspPkg) {
                pkgp->v3fatalSrc("name clash with builtin package " << V3BspModules::builtinBspPkg
                                                                    << endl);
            }
        });
        m_netlistp->foreach([this](AstClass* classp) {
            if (classp->name() == V3BspModules::builtinBaseClass) {
                classp->v3fatalSrc("name clash with builtin base class "
                                   << V3BspModules::builtinBaseClass << endl);
            }
            if (classp->name() == V3BspModules::builtinBaseInitClass) {
                classp->v3fatalSrc("name clash with builtin base class "
                                   << V3BspModules::builtinBaseInitClass << endl);
            }
        });
        m_netlistp->foreach([this](AstClassPackage* classp) {
            if (classp->name() == V3BspModules::builtinBaseClassPkg) {
                classp->v3fatalSrc("name classh with builtin base classpacakge "
                                   << V3BspModules::builtinBaseClassPkg << endl);
            }
        });
    }
    void prepareClassGeneration() {
        checkBuiltinNotUsed();
        // first create a new top module that will replace the existing one later
        FileLine* fl = m_netlistp->topModulep()->fileline();
        UASSERT(!m_topModp, "new top already exsists!");
        // create a new top module
        m_topModp = new AstModule{fl, m_netlistp->topModulep()->name(), true};
        m_topModp->level(1);  // top module
        m_topScopep = new AstScope{fl, m_topModp, m_netlistp->topScopep()->scopep()->name(),
                                   nullptr, nullptr};
        // m_topModp->addStmtsp(new AstTopScope{fl, topScopep});

        // all the classes will be in a package, let's build this package and
        // add an instance of it to the new top module
        // create a package for all the bsp classes
        m_packagep = new AstPackage{m_netlistp->fileline(), V3BspModules::builtinBspPkg};
        m_packagep->level(3);  // lives under a cell (2) under top (1)
        UASSERT(m_topScopep, "No top scope!");
        // a cell instance of the pakage that is added to the top module
        m_packageCellp = new AstCell{fl,
                                     fl,
                                     m_modNames.get(V3BspModules::builtinBspPkg + "Inst"),
                                     m_packagep->name(),
                                     nullptr,
                                     nullptr,
                                     nullptr};
        m_packageCellp->modp(m_packagep);

        m_topModp->addStmtsp(m_packageCellp);
        // scope of the package that is under the top
        AstScope* packageScopep = new AstScope{m_packagep->fileline(), m_packagep,
                                               m_topScopep->name() + "." + m_packagep->name(),
                                               m_topScopep, m_packageCellp};
        m_packagep->addStmtsp(packageScopep);
        m_scopePrefix = packageScopep->name() + ".";
        m_packageScopep = packageScopep;
        makeBaseClasses();
    }
    // make a class for each graph
    std::vector<AstClass*> makeClasses() {
        // make base classes for compute and top level program
        // now go though all partitions and create a class and an instance of it
        std::vector<AstClass*> vtxClassesp;
        // AstTopScope* topScopep =
        for (const auto& graphp : m_partitionsp) {
            AstClass* modp = makeClass(graphp);
            vtxClassesp.push_back(modp);
        }

        // make class for initialization
        return vtxClassesp;
    }

    // make a top level module with a single "exchange" function that emulates "AssignPost"
    void makeCopyOperations(const std::vector<AstClass*>& computeClassesp) {
        // AstVarScope::user2 -> true if variable already processed
        AstNode::user2ClearTree();

        // function to run after computation
        AstCFunc* const copyFuncp
            = new AstCFunc{m_netlistp->topModulep()->fileline(), "exchange", m_topScopep, "void"};
        copyFuncp->dontCombine(true);
        // function to run before everything
        AstCFunc* const initFuncp = new AstCFunc{m_netlistp->topModulep()->fileline(),
                                                 "initialize", m_topScopep, "void"};

        initFuncp->slow(true);
        // go through all of the old variables and find their new producer and consumers
        // then create a assignments for updating them safely in an "exchange" function.
        // initialization (AstInitial and AstInitialStatic) also get a similar treatment
        // since there is an individual class that performs the initial computation
        // and that needs to be copied as well.
        m_netlistp->topModulep()->foreach([this, &copyFuncp, &initFuncp](AstVarScope* vscp) {
            auto makeCopyOp = [](const std::pair<AstVarScope*, AstVar*>& sourcep,
                                 const std::pair<AstVarScope*, AstVar*>& targetp) {
                AstVarScope* targetInstp = targetp.first;
                AstVar* targetVarp = targetp.second;
                AstVarScope* sourceInstp = sourcep.first;
                AstVar* sourceVarp = sourcep.second;
                UASSERT(targetInstp && targetVarp && sourceInstp && sourceVarp,
                        "should not be null");
                // create an assignment from target = source
                FileLine* fl = targetInstp->fileline();
                AstMemberSel* const targetSelp
                    = new AstMemberSel{fl, new AstVarRef{fl, targetInstp, VAccess::WRITE},
                                       VFlagChildDType{}, targetVarp->name()

                    };
                // resolve the dtype manually
                targetSelp->varp(targetVarp);
                targetSelp->dtypep(targetVarp->dtypep());
                AstMemberSel* const sourceSelp
                    = new AstMemberSel{fl, new AstVarRef{fl, sourceInstp, VAccess::READ},
                                       VFlagChildDType{}, sourceVarp->name()};
                sourceSelp->varp(sourceVarp);
                sourceSelp->dtypep(sourceVarp->dtypep());
                AstAssign* const assignp = new AstAssign{fl, targetSelp, sourceSelp};
                return assignp;
            };
            if (vscp->user2()) { /*already processed*/
                return;
            }
            vscp->user2(true);

            auto& refInfo = m_vscpRefs(vscp);
            for (const auto& pair : refInfo.targetsp()) {
                if (refInfo.sourcep().first) {
                    UASSERT(refInfo.sourcep() != pair, "Self message not allowed!");
                    copyFuncp->addStmtsp(makeCopyOp(refInfo.sourcep(), pair));
                }
                if (refInfo.initp().first) {
                    initFuncp->addStmtsp(makeCopyOp(refInfo.initp(), pair));
                }
            }
        });
        m_topScopep->addBlocksp(copyFuncp);
        m_topScopep->addBlocksp(initFuncp);

        // snatch the AstTopScope from the existing topModle
        AstTopScope* singletonTopScopep = m_netlistp->topScopep()->unlinkFrBack();
        AstSenTree* senTreep = singletonTopScopep->senTreesp()->unlinkFrBackWithNext();
        AstScope* oldScopep = singletonTopScopep->scopep();
        oldScopep->replaceWith(m_topScopep);
        VL_DO_DANGLING(oldScopep->deleteTree(), oldScopep);
        VL_DO_DANGLING(senTreep->deleteTree(), senTreep);

        // finally put the top scope in the new top module
        m_topModp->addStmtsp(singletonTopScopep);
        // delete any existing modules in the netlist
        AstNodeModule* oldModsp = m_netlistp->modulesp()->unlinkFrBackWithNext();
        VL_DO_DANGLING(oldModsp->deleteTree(), oldModsp);
        // add the new topmodule (should be first, see AstNetlist::topModulesp())
        m_netlistp->addModulesp(m_topModp);

        AstCFunc* computeSetp = new AstCFunc{m_netlistp->topModulep()->fileline(), "computeSet",
                                             m_topScopep, "void"};
        for (AstClass* classp : computeClassesp) {
            AstVarScope* vscp = nullptr;
            for (vscp = m_topScopep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
                AstClassRefDType* classRefp = VN_CAST(vscp->dtypep(), ClassRefDType);
                if (classRefp && classRefp->classp() == classp) break;
            }
            UASSERT(vscp, "did not find class instance!");
            FileLine* fl = vscp->fileline();
            AstCFunc* methodp = nullptr;
            classp->foreach([&methodp](AstCFunc* np) {
                if (np->name() == "compute") { methodp = np; }
            });
            UASSERT_OBJ(methodp, classp, "Expected method named compute");
            AstCMethodCall* callp = new AstCMethodCall{
                fl, new AstVarRef{fl, vscp, VAccess::READ}, methodp, nullptr /*no args*/
            };
            callp->dtypeSetVoid();
            computeSetp->addStmtsp(new AstStmtExpr{fl, callp});
        }
        m_topScopep->addBlocksp(computeSetp);
    }

    AstClass* makeInitial() {
        FileLine* fl = nullptr;
        if (!m_initials.empty()) {
            fl = m_initials.front().second->fileline();
        } else if (!m_initialStatics.empty()) {
            fl = m_initialStatics.front().second->fileline();
        } else {
            fl = m_netlistp->topModulep()->fileline();
        }
        // create a class for the initialization
        auto classAndTypep
            = newClass(fl, m_modNames.get("vtxClsInit"), m_modNames.get("vtxClsInitPkg"));
        AstClass* classp = classAndTypep.first;
        AstClassRefDType* classTypep = classAndTypep.second;
        AstClassExtends* extendsp = new AstClassExtends{fl, nullptr, false};
        extendsp->dtypep(m_classWithInitDTypep);
        classp->addExtendsp(extendsp);
        classp->isExtended(true);

        AstVar* classInstp
            = new AstVar{fl, VVarType::VAR, m_modNames.get("vtxInstInit"), classTypep};
        classInstp->lifetime(VLifetime::STATIC);
        // add the instance to the scope of the top module
        AstVarScope* instVscp = new AstVarScope{classInstp->fileline(), m_topScopep, classInstp};
        m_topScopep->addVarsp(instVscp);
        m_topModp->addStmtsp(classInstp);
        // this class will represent the code that runs on one core
        classp->level(4);  // lives under the BspPkg
        classp->flag(
            VClassFlag{}.append(VClassFlag::BSP_BUILTIN).append(VClassFlag::BSP_INIT_BUILTIN));
        // create a scope for the class
        AstScope* scopep = new AstScope{fl, classp, m_scopePrefix + classp->name(),
                                        m_packageScopep, m_packageCellp};
        AstCFunc* cfuncp = new AstCFunc{fl, "compute", scopep, "void"};
        cfuncp->dontCombine(true);
        cfuncp->isMethod(true);
        cfuncp->isInline(true);

        // STATE
        //      AstVarScope::user3p  -> new var scope local to the class

        AstNode::user3ClearTree();
        auto replaceOldVarRef = [this, &scopep, &classp, &cfuncp, &instVscp](AstVarRef* vrefp) {
            AstVarScope* oldVscp = vrefp->varScopep();
            AstVarScope* substp = VN_CAST(oldVscp->user3p(), VarScope);
            if (!substp) {
                AstVar* varp = new AstVar{vrefp->varScopep()->varp()->fileline(), VVarType::MEMBER,
                                          oldVscp->varp()->name(), oldVscp->varp()->dtypep()};
                substp = new AstVarScope{oldVscp->fileline(), scopep, varp};
                substp->trace(oldVscp->isTrace());
                scopep->addVarsp(substp);
                oldVscp->user3p(substp);
                // if the variable is consumed by any of the graph nodes, then
                // we need to add it as a class level member, otherwise, it should
                // be kept local to the function
                if (vrefp->access().isWriteOrRW() && m_vscpRefs(oldVscp).hasConsumer()) {
                    classp->addStmtsp(varp);
                    m_vscpRefs(oldVscp).initp({instVscp, varp});
                } else {
                    cfuncp->addStmtsp(varp);
                    varp->funcLocal(true);
                }
            }
            // replace the reference
            AstVarRef* newRefp = new AstVarRef{vrefp->fileline(), substp, vrefp->access()};
            vrefp->replaceWith(newRefp);
            VL_DO_DANGLING(vrefp->deleteTree(), vrefp);
        };
        auto appendLogicAndVars = [this, &replaceOldVarRef, &cfuncp](AstNode* nodep) {
            if (AstNodeProcedure* procp = VN_CAST(nodep, NodeProcedure)) {
                for (AstNode* oldp = procp->stmtsp(); oldp; oldp = oldp->nextp()) {
                    AstNode* newp = oldp->cloneTree(false);
                    newp->foreach(replaceOldVarRef);
                    cfuncp->addStmtsp(newp);
                }
            } else {
                AstNode* newp = nodep->cloneTree(false);
                newp->foreach(replaceOldVarRef);
                cfuncp->addStmtsp(newp);
            }
        };

        m_initialStatics.foreachLogic(appendLogicAndVars);
        m_initials.foreachLogic(appendLogicAndVars);

        classp->addStmtsp(scopep);
        scopep->addBlocksp(cfuncp);

        return classp;
    }

public:
    explicit ModuleBuilderImpl(AstNetlist* netlistp,
                               const std::vector<std::unique_ptr<DepGraph>>& partitionsp,
                               const V3Sched::LogicByScope& initials,
                               const V3Sched::LogicByScope& statics)
        : m_modNames{"__VBspCls"}
        , m_netlistp{netlistp}
        , m_partitionsp{partitionsp}
        , m_initials{initials}
        , m_initialStatics{statics} {}
    void go() {
        // do not reorder
        // 1. Determine producer and consumers
        UINFO(3, "Resolving references" << endl);
        computeReferences();
        // 2. Create modules that contain a class implementing the parallel computation
        // with a "compute" method
        UINFO(3, "Creating submodules" << endl);
        prepareClassGeneration();

        AstClass* initClassp = makeInitial();  // should be before making classes
        // since it sets the initp used in makeClasses
        std::vector<AstClass*> submodp = makeClasses();
        // 3. Create copy operations
        UINFO(3, "Creating copy program" << endl);
        makeCopyOperations(submodp);
        // 4. add the classes
        m_netlistp->addModulesp(m_packagep);

        m_netlistp->addModulesp(m_classWithComputep);
        m_netlistp->addModulesp(m_classWithComputep->classOrPackagep());

        m_netlistp->addModulesp(m_classWithInitp);
        m_netlistp->addModulesp(m_classWithInitp->classOrPackagep());

        for (AstClass* clsp : submodp) {
            m_netlistp->addModulesp(clsp);
            m_netlistp->addModulesp(clsp->classOrPackagep());
        }
        m_netlistp->addModulesp(initClassp);
        m_netlistp->addModulesp(initClassp->classOrPackagep());
        // 5. create a class that handles the initialization
    }

};  // namespace
}  // namespace

std::string V3BspModules::builtinBspPkg = "__VbuiltinBspPkg";
std::string V3BspModules::builtinBaseClass = "__VbuiltinBspCompute";
std::string V3BspModules::builtinBaseInitClass = "__VbuiltinBspInit";
std::string V3BspModules::builtinBaseClassPkg = "__VbuiltinBspComputePkg";

void V3BspModules::makeModules(AstNetlist* netlistp,
                               const std::vector<std::unique_ptr<DepGraph>>& partitionsp,
                               const V3Sched::LogicByScope& initials,
                               const V3Sched::LogicByScope& statics) {

    {
        ModuleBuilderImpl builder{netlistp, partitionsp, initials, statics};
        builder.go();
    }
    V3Global::dumpCheckGlobalTree("bspmodules", 0, dumpTree() >= 1);
}

namespace {
AstClass* doFind(AstNetlist* nodep, const std::string& which) {
    AstClass* foundp = nullptr;
    nodep->foreach([&foundp, &which](AstClass* classp) {
        if (classp->name() == which) foundp = classp;
    });
    UASSERT(foundp, "did not find " << which << endl);
    return foundp;
}
}  // namespace
AstClass* V3BspModules::findBspBaseClass(AstNetlist* nodep) {
    return doFind(nodep, builtinBaseClass);
}

AstClass* V3BspModules::findBspBaseInitClass(AstNetlist* nodep) {
    return doFind(nodep, builtinBaseInitClass);
}

};  // namespace V3BspSched