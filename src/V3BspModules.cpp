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

public:
    inline bool isClocked() const { return m_producer != nullptr; }
    inline bool isOwned(const std::unique_ptr<DepGraph>& graphp) const {
        return graphp.get() == m_producer;
    }

    inline bool isLocal() const {
        return !m_producer || (m_consumer.size() == 1 && m_consumer.front() == m_producer);
    }

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

    void sourcep(std::pair<AstVarScope*, AstVar*> nodep) {
        UASSERT(!m_sourcep.first && !m_sourcep.second, "source end point already set!");
        UASSERT(nodep.first && nodep.second, "expected non-null");
        m_sourcep = nodep;
    }
    std::pair<AstVarScope*, AstVar*> sourcep() const { return m_sourcep; }

    void addTargetp(std::pair<AstVarScope*, AstVar*> nodep) { m_targets.push_back(nodep); }
    std::vector<std::pair<AstVarScope*, AstVar*>>& targetsp() { return m_targets; }

    DepGraph* producer() const { return m_producer; }
    void consumer(const std::unique_ptr<DepGraph>& graphp) { m_consumer.push_back(graphp.get()); }
    const std::vector<DepGraph*>& consumer() const { return m_consumer; }
    VarScopeReferences() { m_sourcep = std::make_pair(nullptr, nullptr); }
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
    V3UniqueNames m_ctorNames;
    AstNetlist* m_netlistp;  // original netlist
    const std::vector<std::unique_ptr<DepGraph>>& m_partitionsp;  // partitions


    AstModule* m_topModp = nullptr;
    AstPackage* m_packagep = nullptr;
    AstScope* m_packageScopep = nullptr;
    AstCell* m_packageCellp = nullptr;
    AstScope* m_topScopep = nullptr;
    std::string m_scopePrefix;

    struct ModuleBuilder {
        const std::unique_ptr<DepGraph>& m_graphp;
        std::vector<AstVarScope*> m_inputsp, m_outputsp, m_localsp;
        AstModule* m_modp;
        ModuleBuilder(const std::unique_ptr<DepGraph>& graphp, AstModule* modp)
            : m_graphp(graphp)
            , m_modp(modp) {}
    };
    // compute the references to each variable
    void computeReferences() {
        AstNode::user1ClearTree();
        for (const auto& graphp : m_partitionsp) {
            for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                if (ConstrDefVertex* const defp = dynamic_cast<ConstrDefVertex*>(vtxp)) {
                    m_vscpRefs(defp->vscp()).consumer(graphp);
                } else if (CompVertex* const compp = dynamic_cast<CompVertex*>(vtxp)) {
                    if (AstAssignPost* const postp = VN_CAST(compp->nodep(), AssignPost)) {
                        postp->foreach([this, &graphp](const AstVarRef* vrefp) {
                            if (vrefp->access().isWriteOrRW()) {
                                m_vscpRefs(vrefp->varScopep()).producer(graphp);
                            }
                        });
                    }
                }
            }
        }
    }

    void makeClassPackage() {}

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
                AstVarScope* const vscp = constrp->vscp();
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
                    if (VN_IS(vscp->dtypep(), BasicDType)) {
                        // not memory type, so create variable that is local to
                        // the compute function
                        if (refInfo.isOwned(graphp) && refInfo.isLocal()) {
                            // the variable is produced here and does not need
                            // to be sent out, however we should create a
                            // persitent class member for it to keep it alive
                            // after the function goes out of scope
                            classp->addStmtsp(varp);
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
                    }
                    vscp->user2(true);  // mark visited
                }
            }
        }

        // add the computation
        UINFO(5, "Ordering computation" << endl);
        graphp->order();  // order the computation
        for (V3GraphVertex* itp = graphp->verticesBeginp(); itp; itp = itp->verticesNextp()) {
            if (CompVertex* vtxp = dynamic_cast<CompVertex*>(itp)) {
                // vtxp->nodep
                AstNode* nodep = vtxp->nodep();
                UASSERT(nodep, "nullptr vertex");
                UASSERT_OBJ(VN_IS(nodep, Always) || VN_IS(nodep, AlwaysPost)
                                || VN_IS(nodep, AssignPost) || VN_IS(nodep, AssignPre),
                            nodep, "unexpected node type " << nodep->prettyName() << endl);
                // UASSERT_OBJ(!nodep->nextp(), nodep, "Did not expect nextp");
                UINFO(10, "Cloning " << nodep << endl);
                AstNode* nodeCopyp = nodep->cloneTree(false);
                nodeCopyp->foreach([](AstVarRef* vrefp) {
                    // replace with the new variables
                    UASSERT_OBJ(vrefp->varScopep()->user3p(), vrefp->varScopep(),
                                "Expected user3p");
                    AstVarScope* substp = VN_AS(vrefp->varScopep()->user3p(), VarScope);
                    // replace the reference
                    AstVarRef* newp = new AstVarRef{vrefp->fileline(), substp, vrefp->access()};
                    vrefp->replaceWith(newp);
                });
                cfuncp->addStmtsp(nodeCopyp);
            }
        }
        scopep->addBlocksp(cfuncp);
        classp->addStmtsp(scopep);
        return classp;
    }

    std::vector<AstClass*> makeClasses() {

        // first create a new top module that will replace the existing one later
        FileLine* fl = m_netlistp->topModulep()->fileline();
        UASSERT(!m_topModp, "new top already exsists!");
        // create a new top module
        m_topModp = new AstModule{fl, m_modNames.get("bspTop"), true};
        m_topModp->level(1);  // top module
        m_topScopep = new AstScope{fl, m_topModp, "bspTop", nullptr, nullptr};
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

        return vtxClassesp;
    }
    void makeCopyOperations() {
        // AstVarScope::user2 -> true if variable already processed
        AstNode::user2ClearTree();

        AstCFunc* const copyFuncp
            = new AstCFunc{m_netlistp->topModulep()->fileline(), "exchange", m_topScopep, "void"};

        m_netlistp->topModulep()->foreach([this, &copyFuncp](AstVarScope* vscp) {
            if (vscp->user2()) { /*already processed*/
                return;
            }
            vscp->user2(true);

            auto& refInfo = m_vscpRefs(vscp);
            for (const auto& pair : refInfo.targetsp()) {
                AstVarScope* targetInstp = pair.first;
                AstVar* targetVarp = pair.second;
                AstVarScope* sourceInstp = refInfo.sourcep().first;
                AstVar* sourceVarp = refInfo.sourcep().second;
                UASSERT(targetInstp && targetVarp && sourceInstp && sourceVarp,
                        "should not be null");
                // create an assignment from target = source
                FileLine* fl = targetInstp->fileline();
                AstMemberSel* const targetp
                    = new AstMemberSel{fl, new AstVarRef{fl, targetInstp, VAccess::WRITE},
                                       VFlagChildDType{}, targetVarp->name()

                    };
                // resolve the dtype manually
                targetp->varp(targetVarp);
                targetp->dtypep(targetVarp->dtypep());
                AstMemberSel* const sourcep
                    = new AstMemberSel{fl, new AstVarRef{fl, sourceInstp, VAccess::READ},
                                       VFlagChildDType{}, sourceVarp->name()};
                sourcep->varp(sourceVarp);
                sourcep->dtypep(sourceVarp->dtypep());
                AstAssign* const assignp = new AstAssign{fl, targetp, sourcep};
                copyFuncp->addStmtsp(assignp);
            }
        });
        m_topScopep->addBlocksp(copyFuncp);
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


public:
    explicit ModuleBuilderImpl(AstNetlist* netlistp,
                               const std::vector<std::unique_ptr<DepGraph>>& partitionsp)
        : m_modNames{"__VBspCls"}
        , m_ctorNames{"__VBspCtor"}
        , m_netlistp{netlistp}
        , m_partitionsp{partitionsp} {}
    void go() {

        // 1. Determine producer and consumers
        UINFO(3, "Resolving references" << endl);
        computeReferences();
        // 2. Create modules that contain a class implementing the parallel computation
        // with a "compute" method
        UINFO(3, "Creating submodules" << endl);
        std::vector<AstClass*> submodp = makeClasses();
        // 3. Create copy operations
        UINFO(3, "Creating copy program" << endl);
        makeCopyOperations();
        // 4. add the classes
        m_netlistp->addModulesp(m_packagep);
        for (AstClass* clsp : submodp) {
            m_netlistp->addModulesp(clsp);
        }
    }

};  // namespace
}  // namespace

void V3BspModules::makeModules(AstNetlist* netlistp,
                               const std::vector<std::unique_ptr<DepGraph>>& partitionsp) {

    {
        ModuleBuilderImpl builder{netlistp, partitionsp};
        builder.go();
    }
    V3Global::dumpCheckGlobalTree("bspmodules", 0, dumpTree() >= 1);
}
};  // namespace V3BspSched