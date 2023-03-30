// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Bulk-synchronous parallel module creation
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
        UASSERT(!m_producer, "multiple producers!");
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

class ModuleBuilderImpl final {
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
    AstClassPackage* m_classPacakgep = nullptr;
    AstScope* m_packageScopep = nullptr;
    AstCell* m_packageCellp = nullptr;
    AstScope* m_topScopep = nullptr;
    std::string m_scopePrefix;

    // compute the references to each variable
    void computeReferences() {
        AstNode::user1ClearTree();
        for (const auto& graphp : m_partitionsp) {
            UINFO(100, "Inspecting graph " << graphp.get() << endl);
            for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                if (ConstrDefVertex* const defp = dynamic_cast<ConstrDefVertex*>(vtxp)) {
                    UINFO(100, "consumed: " << defp->vscp()->name() << endl);
                    m_vscpRefs(defp->vscp()).consumer(graphp);
                } else if (CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp)) {
                    if (AstAssignPost* const postp = VN_CAST(compp->nodep(), AssignPost)) {
                        postp->foreach([this, &graphp](const AstVarRef* vrefp) {
                            if (vrefp->access().isWriteOrRW()) {
                                UINFO(100, "produced: " << vrefp->varScopep()->name() << endl);
                                m_vscpRefs(vrefp->varScopep()).producer(graphp);
                            }
                        });
                    }
                }
            }
        }
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
        AstClass* classp = new AstClass{fl, m_modNames.get("vtxCls")};
        classp->classOrPackagep(new AstClassPackage{fl, m_modNames.get("vtxClsInitPkg")});
        classp->classOrPackagep()->classp(classp);

        AstClassRefDType* classTypep = new AstClassRefDType{fl, classp, nullptr};
        classTypep->classOrPackagep(m_packagep);
        m_netlistp->typeTablep()->addTypesp(classTypep);
        AstVar* classInstp = new AstVar{fl, VVarType::VAR, m_modNames.get("vtxInst"), classTypep};
        classInstp->lifetime(VLifetime::STATIC);
        // add the instance to the scope of the top module
        AstVarScope* instVscp = new AstVarScope{classInstp->fileline(), m_topScopep, classInstp};
        m_topScopep->addVarsp(instVscp);
        m_topModp->addStmtsp(classInstp);
        // this class will represent the code that runs on one core
        classp->level(4);  // lives under the BspPkg
        // create a scope for the class
        AstScope* scopep = new AstScope{fl, classp, m_scopePrefix + classp->name(),
                                        m_packageScopep, m_packageCellp};
        AstCFunc* cfuncp = new AstCFunc{fl, "compute", scopep, "void"};
        cfuncp->dontCombine(true);
        cfuncp->isMethod(true);

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
                    scopep->addVarsp(newVscp);
                    vscp->user3p(newVscp);
                    if (VN_IS(vscp->dtypep(), BasicDType)
                        || VN_IS(vscp->dtypep()->skipRefp(), BasicDType)) {
                        // not memory type, so create variable that is local to
                        // the compute function
                        if (refInfo.isOwned(graphp) && refInfo.isLocal()) {
                            // the variable is produced here and does not need
                            // to be sent out, however we should create a
                            // persitent class member for it to keep it alive
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
                        } else if (refInfo.isClocked()) {
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
                    } else if (VN_IS(vscp->dtypep(), UnpackArrayDType)) {
                        UASSERT_OBJ(refInfo.isLocal(), vscp, "memory should be local!");
                        classp->addStmtsp(varp);
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
                                || VN_IS(nodep, AssignPost) || VN_IS(nodep, AssignPre),
                            nodep, "unexpected node type " << nodep->prettyName() << endl);
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
            nodep->foreach([](AstVarRef* vrefp) {
                // replace with the new variables
                UASSERT_OBJ(vrefp->varScopep()->user3p(), vrefp->varScopep(), "Expected user3p");
                AstVarScope* substp = VN_AS(vrefp->varScopep()->user3p(), VarScope);
                // replace the reference
                AstVarRef* newp = new AstVarRef{vrefp->fileline(), substp, vrefp->access()};
                vrefp->replaceWith(newp);
                VL_DO_DANGLING(vrefp->deleteTree(), vrefp);
            });
            cfuncp->addStmtsp(nodep);
        }
        scopep->addBlocksp(cfuncp);
        classp->addStmtsp(scopep);
        return classp;
    }

    // make a class for each graph
    std::vector<AstClass*> makeClasses() {

        // first create a new top module that will replace the existing one later
        FileLine* fl = m_netlistp->topModulep()->fileline();
        UASSERT(!m_topModp, "new top already exsists!");
        // create a new top module
        m_topModp = new AstModule{fl, m_netlistp->topModulep()->name(), true};
        m_topModp->level(1);  // top module
        m_topScopep
            = new AstScope{fl, m_topModp, m_netlistp->topScopep()->name(), nullptr, nullptr};
        // m_topModp->addStmtsp(new AstTopScope{fl, topScopep});

        // all the classes will be in a package, let's build this package and
        // add an instance of it to the new top module
        // create a package for all the bsp classes
        m_packagep = new AstPackage{m_netlistp->fileline(), "__VBspPkg"};
        m_packagep->level(3);  // lives under a cell (2) under top (1)
        UASSERT(m_topScopep, "No top scope!");
        // a cell instance of the pakage that is added to the top module
        m_packageCellp
            = new AstCell{fl, fl, "__VBspPkgInst", m_packagep->name(), nullptr, nullptr, nullptr};
        m_packageCellp->modp(m_packagep);

        m_topModp->addStmtsp(m_packageCellp);
        // scope of the package that is under the top
        AstScope* packageScopep = new AstScope{m_packagep->fileline(), m_packagep,
                                               m_topScopep->name() + "." + m_packagep->name(),
                                               m_topScopep, m_packageCellp};
        m_packagep->addStmtsp(packageScopep);
        m_scopePrefix = packageScopep->name() + ".";
        m_packageScopep = packageScopep;
        // now gor though all partitions and create a class and an instance of it
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
    void makeCopyOperations() {
        // AstVarScope::user2 -> true if variable already processed
        AstNode::user2ClearTree();

        // function to run after computation
        AstCFunc* const copyFuncp
            = new AstCFunc{m_netlistp->topModulep()->fileline(), "exchange", m_topScopep, "void"};
        // function to run before everything
        AstCFunc* const initFuncp = new AstCFunc{m_netlistp->topModulep()->fileline(),
                                                 "initialize", m_topScopep, "void"};

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
        AstClass* classp = new AstClass{fl, m_modNames.get("vtxClsInit")};
        // need a class package as well
        classp->classOrPackagep(new AstClassPackage{fl, m_modNames.get("vtxClsInitPkg")});
        classp->classOrPackagep()->classp(classp);
        AstClassRefDType* classTypep = new AstClassRefDType{fl, classp, nullptr};
        classTypep->classOrPackagep(m_packagep);
        m_netlistp->typeTablep()->addTypesp(classTypep);
        AstVar* classInstp
            = new AstVar{fl, VVarType::VAR, m_modNames.get("vtxInstInit"), classTypep};
        classInstp->lifetime(VLifetime::STATIC);
        // add the instance to the scope of the top module
        AstVarScope* instVscp = new AstVarScope{classInstp->fileline(), m_topScopep, classInstp};
        m_topScopep->addVarsp(instVscp);
        m_topModp->addStmtsp(classInstp);
        // this class will represent the code that runs on one core
        classp->level(4);  // lives under the BspPkg
        // create a scope for the class
        AstScope* scopep = new AstScope{fl, classp, m_scopePrefix + classp->name(),
                                        m_packageScopep, m_packageCellp};
        AstCFunc* cfuncp = new AstCFunc{fl, "compute", scopep, "void"};
        cfuncp->dontCombine(true);
        cfuncp->isMethod(true);

        // STATE
        //      AstVarScope::user3p  -> new var scope local to the class

        AstNode::user3ClearTree();
        auto replaceOldVarRef = [this, &scopep, &classp, &cfuncp, &classInstp](AstVarRef* vrefp) {
            AstVarScope* oldVscp = vrefp->varScopep();
            AstVarScope* substp = VN_CAST(oldVscp->user3p(), VarScope);
            if (!substp) {
                AstVar* varp = new AstVar{vrefp->varScopep()->varp()->fileline(), VVarType::MEMBER,
                                          oldVscp->varp()->name(), oldVscp->varp()->dtypep()};
                substp = new AstVarScope{oldVscp->fileline(), scopep, varp};
                scopep->addVarsp(substp);
                oldVscp->user3p(substp);
                // if the variable is consumed by any of the graph nodes, then
                // we need to add it as a class level member, otherwise, it should
                // be kept local to the function
                if (m_vscpRefs(oldVscp).hasConsumer()) {
                    classp->addStmtsp(varp);
                    m_vscpRefs(oldVscp).initp({substp, classInstp});
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
        std::vector<AstClass*> submodp = makeClasses();
        AstClass* initClassp = makeInitial();
        // 3. Create copy operations
        UINFO(3, "Creating copy program" << endl);
        makeCopyOperations();
        // 4. add the classes
        m_netlistp->addModulesp(m_packagep);
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
};  // namespace V3BspSched