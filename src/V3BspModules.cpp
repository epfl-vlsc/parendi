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
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <algorithm>
VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3BspSched {
namespace {
class ReplaceOldVarRefsVisitor final : public VNVisitor {
private:
    void visit(AstVarRef* vrefp) override {
        // replace with the new variables
        if (vrefp->dtypep()->basicp() && vrefp->dtypep()->basicp()->isTriggerVec())
            return;  // need need to replace
        UASSERT_OBJ(vrefp->varScopep()->user3p(), vrefp->varScopep(),
                    "Expected user3p, perhaps you have created a combinational partition?");
        AstVarScope* substp = VN_AS(vrefp->varScopep()->user3p(), VarScope);
        // replace the reference with placement-new
        vrefp->name(substp->varp()->name());
        vrefp->varp(substp->varp());
        vrefp->varScopep(substp);
        // new AstVarRef{vrefp->fileline(), substp, vrefp->access()};
        // allocation new will not work if nodep == vrefp and there is no back
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit ReplaceOldVarRefsVisitor(AstNode* nodep) { iterate(nodep); }
};

class VarScopeReferences {
private:
    DepGraph* m_producer = nullptr;
    std::vector<DepGraph*> m_consumer;
    std::pair<AstVarScope*, AstVar*> m_sourcep;
    std::vector<std::pair<AstVarScope*, AstVar*>> m_targets;
    std::pair<AstVarScope*, AstVar*> m_initp;
    bool m_actRegion = false;

public:
    inline bool isActive() const { return m_actRegion; }
    inline void active(bool v) { m_actRegion = v; }
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
    AstUser1Allocator<AstVarScope, VarScopeReferences> m_vscpRefs;
    V3UniqueNames m_modNames;
    V3UniqueNames m_memberNames;
    AstNetlist* m_netlistp;  // original netlist
    const std::vector<std::unique_ptr<DepGraph>>& m_partitionsp;  // partitions
    const V3Sched::LogicByScope m_initials;
    const V3Sched::LogicByScope m_initialStatics;
    const V3Sched::LogicByScope m_actives;

    AstModule* m_topModp = nullptr;
    AstPackage* m_packagep = nullptr;

    AstClass* m_classWithComputep = nullptr;
    AstClassRefDType* m_classWithComputeDtypep = nullptr;
    AstClass* m_classWithInitp = nullptr;
    AstClassRefDType* m_classWithInitDTypep = nullptr;

    AstScope* m_packageScopep = nullptr;
    AstCell* m_packageCellp = nullptr;
    AstScope* m_topScopep = nullptr;

    struct TriggerInfo {
        std::vector<AstSenTree*> clockersp;
        AstBasicDType* trigDTypep = nullptr;
        AstVarScope* autoTriggerp = nullptr;
        AstSenTree* autoTriggerSenTreep = nullptr;
        TriggerInfo() {
            clockersp.clear();
            trigDTypep = nullptr;
            autoTriggerp = nullptr;
        }
    } m_triggering;

    std::string m_scopePrefix;
    string freshName(AstVarScope* oldVscp) {
        return m_memberNames.get(oldVscp->scopep()->nameDotless() + "__DOT__"
                                 + oldVscp->varp()->name());
    }
    string freshName(const string& n) { return m_memberNames.get(n); }
    string freshName(const AstNode* nodep) { return m_memberNames.get(nodep); }

    // check whether data type is supported
    inline bool supportedDType(const AstNodeDType* const dtypep) const {
        return isAnyTypeFromList<AstBasicDType, AstUnpackArrayDType, AstNodeUOrStructDType>(
            dtypep);
    }
    template <typename... Args>
    inline bool isAnyTypeFromList(const AstNodeDType* const dtypep) const {
        for (bool x : {(dynamic_cast<Args*>(dtypep->skipRefp()) != nullptr)...}) {
            if (x) { return true; }
        }
        return false;
    }

    // compute the references to each variable
    void computeReferences() {
        AstNode::user1ClearTree();
        //  first go through the nba (clocked) partitions

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
                    if (compp->domainp()) {
                        compp->domainp()->foreach([this, &graphp](const AstVarRef* vrefp) {
                            if (vrefp->access().isReadOrRW()) {
                                m_vscpRefs(vrefp->varScopep()).consumer(graphp);
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
        // now go through the active region and mark any variable produced there
        // to ensure they are considered class members, not stack variables.
        // This can be optimized but should not matter so much
        m_actives.foreachLogic([this](AstNode* actp) {
            actp->foreach([this](const AstVarRef* vrefp) {
                if (vrefp->access().isWriteOrRW()) { m_vscpRefs(vrefp->varScopep()).active(true); }
            });
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

        // collect clockers
        m_triggering.clockersp.clear();
        for (AstSenTree* stp = m_netlistp->topScopep()->senTreesp(); stp;
             stp = VN_AS(stp->nextp(), SenTree)) {
            if (stp->hasHybrid()) {
                stp->v3warn(E_UNSUPPORTED, "Hybrid logic not supported");
            } else if (stp->hasClocked() && !stp->hasCombo() && !stp->hasHybrid()) {
                stp->foreach([this, stp](AstVarRef* vrefp) {
                    if (vrefp->varp()->isUsedClock() && vrefp->varp()->isReadOnly()
                        && !stp->sensesp()->nextp() /* should be a single item*/) {
                        UASSERT_OBJ(m_triggering.autoTriggerp == vrefp->varScopep()
                                        || !m_triggering.autoTriggerp,
                                    vrefp->varScopep(), "Could not determine global clock");
                        // select the global clocker, there should be just one global clock for now
                        // and this global clock should be the sole primary IO of the top module
                        m_triggering.autoTriggerp = vrefp->varScopep();
                        m_triggering.autoTriggerSenTreep = stp;
                    }
                });
                m_triggering.clockersp.push_back(stp);
            }
        }
        if (!m_triggering.autoTriggerp && m_partitionsp.size() > 0) {
            m_netlistp->v3error("Failed to detect the clock, this is might be an internal error");
        }
        m_triggering.trigDTypep
            = new AstBasicDType{fl, VBasicDTypeKwd::TRIGGERVEC, VSigning::UNSIGNED,
                                static_cast<int>(m_triggering.clockersp.size()),
                                static_cast<int>(m_triggering.clockersp.size())};
        m_netlistp->typeTablep()->addTypesp(m_triggering.trigDTypep);
    }

    // make a class for each graph
    std::vector<AstClass*> makeClasses() {
        // make base classes for compute and top level program
        // now go though all partitions and create a class and an instance of it
        std::vector<AstClass*> vtxClassesp;
        // AstTopScope* topScopep =
        int index = 0;
        for (const auto& graphp : m_partitionsp) {
            UASSERT(graphp->verticesBeginp(), "Expected non-empty graph");
            AstClass* modp = makeClass(graphp);
            if (dumpGraph() > 0) { graphp->dumpDotFilePrefixed("ordered_" + cvtToStr(index++)); }

            vtxClassesp.push_back(modp);
        }

        // make class for initialization
        return vtxClassesp;
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
        m_classWithComputep->internal(true);  // prevent deletion
        m_classWithComputep->inLibrary(true);
        m_classWithComputeDtypep = clsAndTpe.second;

        auto initClsAndTypep
            = newClass(new FileLine(FileLine::builtInFilename()),
                       V3BspModules::builtinBaseInitClass, V3BspModules::builtinBaseClassPkg);
        m_classWithInitp = initClsAndTypep.first;
        m_classWithInitDTypep = initClsAndTypep.second;
        m_classWithInitp->isVirtual(true);
        m_classWithInitp->inLibrary(true);
        m_classWithInitp->internal(true);  // prevent deletion
    }

    AstVarScope* cloneTriggerVar(AstVarScope* oldVscp, AstScope* scopep, AstClass* classp,
                                 const std::unique_ptr<DepGraph>& consumer,
                                 AstVarScope* instVscp) {
        if (oldVscp->user2()) {
            UASSERT_OBJ(oldVscp->user3p(), oldVscp, "expected user3p");
            return VN_AS(oldVscp->user3p(), VarScope);  // already cloned
        }

        AstVar* newVarp
            = new AstVar{oldVscp->varp()->fileline(), VVarType::MEMBER,
                         freshName(oldVscp->varp()->name()), oldVscp->varp()->dtypep()};
        newVarp->lifetime(VLifetime::AUTOMATIC);
        newVarp->bspFlag(VBspFlag{}.append(VBspFlag::MEMBER_LOCAL));
        classp->addStmtsp(newVarp);

        AstVarScope* const newVscp = new AstVarScope{oldVscp->fileline(), scopep, newVarp};
        scopep->addVarsp(newVscp);
        oldVscp->user3p(newVscp);
        oldVscp->user2(true);

        if (consumer) {
            UASSERT(instVscp, "expected none-null");
            m_vscpRefs(oldVscp).consumer(consumer);
            m_vscpRefs(oldVscp).addTargetp({instVscp, newVarp});
        }

        return newVscp;
    }
    void cloneTriggerVars(AstSenTree* senTreep, AstScope* scopep, AstClass* classp,
                          const std::unique_ptr<DepGraph>& consumer, AstVarScope* instVscp) {
        senTreep->foreach([this, scopep, classp, &consumer, instVscp](AstVarRef* vrefp) {
            cloneTriggerVar(vrefp->varScopep(), scopep, classp, consumer, instVscp);
        });
    }
    /// @brief make trigger pair for the given SenItem
    /// @param itemp the activation item, should not empty and should be a clocked type
    /// @param scopep the scope to add the trigger variable scopes
    /// @param classp the class to add the trigger variable
    /// @return the old value of the trigger and an assignment to update it
    auto makeTriggerPair(AstSenItem* itemp, AstScope* scopep, AstClass* classp) {
        VEdgeType edge = itemp->edgeType();
        UASSERT_OBJ(itemp->sensp(), itemp, "null expression");
        // TODO: use temp variable to avoid recomputing expression

        // create a member variable that holds the previous value of the expression
        AstVar* prevVarp = new AstVar{itemp->fileline(), VVarType::MEMBER, freshName(itemp),
                                      m_netlistp->findBitDType(1, 1, VSigning::UNSIGNED)};
        prevVarp->lifetime(VLifetime::AUTOMATIC);
        classp->addStmtsp(prevVarp);
        AstVarScope* prevVscp = new AstVarScope{prevVarp->fileline(), scopep, prevVarp};
        scopep->addVarsp(prevVscp);

        AstNodeExpr* exprClonep = itemp->sensp()->cloneTree(false);

        { ReplaceOldVarRefsVisitor{exprClonep}; }

        AstNodeExpr* condExprp = nullptr;
        AstNodeExpr* prevExprp = new AstVarRef{itemp->fileline(), prevVscp, VAccess::READ};

        if (edge == VEdgeType::ET_CHANGED) {
            condExprp = new AstNeq{itemp->fileline(), exprClonep, prevExprp};
        } else if (edge == VEdgeType::ET_POSEDGE || edge == VEdgeType::ET_NEGEDGE) {
            auto mkNot = [](AstNodeExpr* exprp, bool pos) -> AstNodeExpr* {
                if (pos) {
                    return exprp;
                } else {
                    return new AstNot{exprp->fileline(), exprp};
                }
            };
            bool posEdge = (edge == VEdgeType::ET_POSEDGE);
            condExprp = new AstAnd{itemp->fileline(), mkNot(exprClonep, posEdge),
                                   mkNot(prevExprp, !posEdge)};

        } else if (edge == VEdgeType::ET_BOTHEDGE) {
            condExprp = new AstXor{itemp->fileline(), exprClonep, prevExprp};
        } else {
            itemp->v3warn(E_UNSUPPORTED, "Unsupported edge type");
        }
        AstAssign* updatep = new AstAssign{
            itemp->fileline(), new AstVarRef{itemp->fileline(), prevVscp, VAccess::WRITE},
            exprClonep->cloneTree(false)};

        return std::pair<AstNodeExpr*, AstAssign*>{condExprp, updatep};
    }

    // make the triggering function
    using TrigAtGen = std::function<AstNodeExpr*(AstVarScope*)>;
    std::pair<std::map<AstSenTree*, TrigAtGen>, AstCFunc*>
    makeTriggerEvalFunc(const std::unique_ptr<DepGraph>& graphp, AstClass* classp,
                        AstScope* scopep, AstVarScope* instVscp) {

        AstCFunc* const trigEvalFuncp = new AstCFunc{classp->fileline(), "triggerEval", scopep};
        // creates this function
        // void triggerEval() {
        //     trigger.clear();
        //     while (trigger.empty()) {
        //          trigger.set(...)
        //          trigger.set(...)
        //          if (!trigger.any()) {
        //              autoTrig = !autoTrig
        //              time += 1;
        //          }
        //     }
        // }
        trigEvalFuncp->isMethod(true);
        trigEvalFuncp->isInline(true);
        trigEvalFuncp->dontCombine(true);
        scopep->addBlocksp(trigEvalFuncp);
        // create the a function that sets the triggers, for this we first need
        // to gather all the different SenTrees that could cause an activation
        // within this partition.

        AstVar* const thisTrigp = new AstVar{classp->fileline(), VVarType::MEMBER,
                                             freshName("actTrig"), m_triggering.trigDTypep};
        trigEvalFuncp->rtnType(thisTrigp->dtypep()->cType("", false, false));
        thisTrigp->funcLocal(true);
        thisTrigp->funcReturn(true);
        thisTrigp->lifetime(VLifetime::AUTOMATIC);
        trigEvalFuncp->addStmtsp(thisTrigp);
        AstVarScope* const thisTrigVscp = new AstVarScope{classp->fileline(), scopep, thisTrigp};
        scopep->addVarsp(thisTrigVscp);

        AstCMethodHard* const trigClearp = new AstCMethodHard{
            classp->fileline(), new AstVarRef{classp->fileline(), thisTrigVscp, VAccess::WRITE},
            "clear", nullptr};
        trigClearp->dtypeSetVoid();
        trigEvalFuncp->addStmtsp(
            new AstStmtExpr{classp->fileline(), trigClearp});  // trigger.clear()
        AstCMethodHard* const trigEmptyp = new AstCMethodHard{
            classp->fileline(), new AstVarRef{classp->fileline(), thisTrigVscp, VAccess::READ},
            "empty", nullptr};
        trigEmptyp->dtypeSetBit();
        AstWhile* const trigLoopp = new AstWhile{classp->fileline(), trigEmptyp, nullptr, nullptr};
        // while(!trigger.any()) {
        trigEvalFuncp->addStmtsp(trigLoopp);
        // AstWhile* triggerLoopp = new AstWhile{classp->fileline(), }
        uint32_t triggerId = 0;
        AstNodeStmt* trigSetStmtp = nullptr;
        AstNodeStmt* trigUpdateStmtp = nullptr;
        std::map<AstSenTree*, TrigAtGen> atFuncs;

        auto processSenTree = [&](AstSenTree* const senTreep,
                                  const std::unique_ptr<DepGraph>& consumer,
                                  AstVarScope* consumerInstp) {
            if (senTreep->user2()) { return; /* already done */ }
            senTreep->user2(true);  // mark visited

            // clone the trigger variables, used by the ReplaceOldVarRefsVisitor
            cloneTriggerVars(senTreep, scopep, classp, consumer, consumerInstp);

            UASSERT(senTreep->sensesp() && senTreep->sensesp()->sensp(), "empty SenTree");

            // create an OR of all the items
            auto trigInfo = makeTriggerPair(senTreep->sensesp(), scopep, classp);
            AstNodeExpr* trigOrExprp = trigInfo.first;
            AstNodeAssign* const trigUpdatep = trigInfo.second;
            // AstNodeExpr* trigExprp = mkTrigExpr(senTreep->sensesp(), prevVscp);
            for (AstSenItem* itemp = VN_AS(senTreep->sensesp()->nextp(), SenItem); itemp;
                 itemp = VN_AS(itemp->nextp(), SenItem)) {
                UASSERT(itemp->isClocked(), "expected clocked");
                auto newTrig = makeTriggerPair(itemp, scopep, classp);
                trigOrExprp = new AstOr{senTreep->fileline(), trigOrExprp, newTrig.first};
                trigUpdatep->addNext(newTrig.second);
            }
            AstCMethodHard* const setTrigp = new AstCMethodHard{
                senTreep->fileline(),
                new AstVarRef{senTreep->fileline(), thisTrigVscp, VAccess::WRITE}, "set",
                new AstConst{senTreep->fileline(), triggerId}};
            setTrigp->pure(false);
            atFuncs.emplace(senTreep, [classp, triggerId](AstVarScope* trigVscp) -> AstNodeExpr* {
                AstCMethodHard* const atp = new AstCMethodHard{
                    classp->fileline(), new AstVarRef{classp->fileline(), trigVscp, VAccess::READ},
                    "at", new AstConst{classp->fileline(), triggerId}};
                atp->dtypeSetBit();
                atp->pure(true);
                return atp;
            });

            triggerId++;
            setTrigp->addPinsp(trigOrExprp);
            setTrigp->dtypeSetVoid();
            if (!trigSetStmtp) {
                trigSetStmtp = new AstStmtExpr{senTreep->fileline(), setTrigp};
            } else {
                trigSetStmtp->addNext(new AstStmtExpr{senTreep->fileline(), setTrigp});
            }
            if (!trigUpdateStmtp) {
                trigUpdateStmtp = trigUpdatep;
            } else {
                trigUpdateStmtp->addNext(trigUpdatep);
            }
        };

        // make sure the generated sentree (i.e., the top clock) exists
        processSenTree(m_triggering.autoTriggerSenTreep, nullptr, nullptr /* is not a consumer*/);
        // we process all sentrees, irrespective whether they have been used or not
        //
        m_netlistp->topScopep()->foreach(
            [&processSenTree, &graphp, instVscp](AstSenTree* senTreep) {
                if (senTreep->hasClocked() && !senTreep->hasHybrid() && !senTreep->hasCombo())
                    processSenTree(senTreep, graphp, instVscp /* is a consumer*/);
            });

        AstNode* const actp = new AstComment{classp->fileline(), "active region computation"};
        for (const std::pair<AstScope*, AstActive*>& activePair : m_actives) {

            // "act" region should be execute before setting the triggers
            // but we only execute the actives that matter. These are the ones
            // that set the value of one of the trigger variables which
            // is cloned just above

            activePair.second->foreach(
                [this, scopep, classp, instVscp, &graphp](AstVarRef* vrefp) {
                    if (vrefp->varScopep()->user2()) return; /*already cloned*/
                    AstVarScope* const vscp = vrefp->varScopep();
                    auto& refInfo = m_vscpRefs(vscp);
                    vscp->user2(true);  // visited
                    AstVar* const varp = new AstVar{vscp->varp()->fileline(), VVarType::MEMBER,
                                                    freshName(vscp), vscp->varp()->dtypep()};
                    varp->origName(vscp->name());
                    varp->lifetime(VLifetime::AUTOMATIC);
                    classp->addStmtsp(varp);
                    AstVarScope* const newVscp = new AstVarScope{vscp->fileline(), scopep, varp};
                    newVscp->trace(vscp->isTrace());
                    scopep->addVarsp(newVscp);
                    vscp->user3p(newVscp);
                    // the variable could be produced by another partition, the init
                    // class or the current active. In the latter case, we can keep
                    // it on the stack as an optimizatin, but we don't do it yet.
                    if (supportedDType(vscp->dtypep())) {
                        UASSERT_OBJ(!refInfo.isOwned(graphp), vscp,
                                    "Expected to be produced by another");
                        if (refInfo.isClocked() || refInfo.initp().first) {
                            // not produced here but consumed
                            varp->bspFlag(VBspFlag{}.append(VBspFlag::MEMBER_INPUT));
                            // need to recieve it
                            refInfo.addTargetp(std::make_pair(instVscp, varp));
                            V3Stats::addStatSum("BspModules, input variable", 1);
                        }
                    } else {
                        vscp->v3error("Unknown data type " << vscp->dtypep()->skipRefp() << endl);
                    }
                });
            AstNode* const clonep = activePair.second->stmtsp()->cloneTree(true);
            for (AstNode* cp = clonep; cp; cp = cp->nextp()) { ReplaceOldVarRefsVisitor{cp}; }
            actp->addNext(clonep);
        }

        trigLoopp->addStmtsp(actp);

        trigLoopp->addStmtsp(trigSetStmtp);
        trigLoopp->addStmtsp(trigUpdateStmtp);

        // create the auto trigger, basically toggling the clock
        AstAssign* const clockTogglep = new AstAssign{
            classp->fileline(),
            new AstVarRef{classp->fileline(), VN_AS(m_triggering.autoTriggerp->user3p(), VarScope),
                          VAccess::WRITE},
            new AstNot{classp->fileline(),
                       new AstVarRef{classp->fileline(),
                                     VN_AS(m_triggering.autoTriggerp->user3p(), VarScope),
                                     VAccess::READ}}};
        AstIf* const doTogglep
            = new AstIf{classp->fileline(), trigEmptyp->cloneTree(false), clockTogglep, nullptr};
        trigLoopp->addStmtsp(doTogglep);
        trigEvalFuncp->addStmtsp(new AstCReturn{
            classp->fileline(), new AstVarRef{classp->fileline(), thisTrigVscp, VAccess::READ}});
        return {atFuncs, trigEvalFuncp};
    }

    void makeClassMemberVarOrConst(AstVarScope* const vscp,
                                   const std::unique_ptr<DepGraph>& graphp, AstScope* const scopep,
                                   AstClass* const classp, AstVarScope* const instVscp,
                                   AstCFunc* const nbaTopp, FileLine* const fl) {
        if (vscp->user2()) {
            // already processed
            return;
        }
        vscp->user2(true);  // mark visited
        // check if the variable is part of the const pool
        if (m_netlistp->constPoolp()
            && m_netlistp->constPoolp()->modp() == vscp->scopep()->modp()) {
            // need not to clone it, rather keep a a reference to self
            vscp->user3p(vscp);
            return;
        }
        // add any variable reference in the partition to the local scope
        // for any local variable that is produced by another graph we need
        // to create an input variable
        auto& refInfo = m_vscpRefs(vscp);
        AstVar* const varp = new AstVar{vscp->varp()->fileline(), VVarType::MEMBER,
                                        freshName(vscp), vscp->varp()->dtypep()};
        varp->origName(vscp->name());
        varp->lifetime(VLifetime::AUTOMATIC);
        AstVarScope* const newVscp = new AstVarScope{vscp->fileline(), scopep, varp};
        newVscp->trace(vscp->isTrace());
        scopep->addVarsp(newVscp);
        vscp->user3p(newVscp);
        if (supportedDType(vscp->dtypep())) {

            if (refInfo.isOwned(graphp) && refInfo.isLocal()) {
                // the variable is produced here and does not need
                // to be sent out, however we should create a
                // persistent class member for it to keep it alive
                // after the function goes out of scope
                classp->addStmtsp(varp);
                refInfo.addTargetp(std::make_pair(instVscp, varp));
                varp->bspFlag(
                    VBspFlag{}.append(VBspFlag::MEMBER_OUTPUT).append(VBspFlag::MEMBER_LOCAL));
                V3Stats::addStatSum("BspModules, local variable", 1);
            } else if (refInfo.isOwned(graphp) && !refInfo.isLocal()) {
                // variable is owed/produced here but also referenced
                // by others
                classp->addStmtsp(varp);
                // need to send it
                UASSERT(!refInfo.sourcep().first, "multiple producers!");
                refInfo.sourcep(std::make_pair(instVscp, varp));
                refInfo.addTargetp({instVscp, varp});
                varp->bspFlag(VBspFlag{}.append(VBspFlag::MEMBER_OUTPUT));
                V3Stats::addStatSum("BspModules, output variable", 1);
            } else if (refInfo.isClocked() || refInfo.initp().first) {
                UASSERT_OBJ(refInfo.isConsumed(graphp), vscp, "Unexpected reference!");
                // not produced here but consumed
                classp->addStmtsp(varp);
                varp->bspFlag(VBspFlag{}.append(VBspFlag::MEMBER_INPUT));
                // need to recieve it
                refInfo.addTargetp(std::make_pair(instVscp, varp));
                V3Stats::addStatSum("BspModules, input variable", 1);
            } else if (refInfo.isActive()) {
                // variable produced by the trigger function, but local otherwise
                classp->addStmtsp(varp);
                varp->bspFlag({VBspFlag::MEMBER_LOCAL});
                V3Stats::addStatSum("BspModules, active variable", 1);
            } else {
                // temprorary variables, lifetime limited to the scope
                // of the enclosing function
                V3Stats::addStatSum("BspModules, stack variable", 1);
                nbaTopp->addStmtsp(varp);
                varp->funcLocal(true);
            }
        } else {
            vscp->v3fatalSrc("Unknown data type " << vscp->dtypep() << endl);
        }
    }
    void makeClassMemberVars(const std::unique_ptr<DepGraph>& graphp, AstScope* const scopep,
                             AstClass* const classp, AstVarScope* const instVscp,
                             AstCFunc* const nbaTopp, FileLine* const fl) {
        for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            if (ConstrVertex* constrp = dynamic_cast<ConstrVertex*>(vtxp)) {
                AstVarScope* vscp = constrp->vscp();
                makeClassMemberVarOrConst(vscp, graphp, scopep, classp, instVscp, nbaTopp, fl);
            }
        }
    }

    /// @brief create a class for the given partiton
    /// @param graphp
    /// @return
    AstClass* makeClass(const std::unique_ptr<DepGraph>& graphp) {

        m_memberNames.reset();
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
        // create member variables for the class
        // STATE
        // VarScope::user2   -> true if already processed
        // AstSenTree::user2 -> true if processesed
        // VarScope::user3p  -> new var scope inside the class

        VNUser2InUse user2InUse;
        VNUser3InUse user3InUse;
        AstNode::user2ClearTree();
        AstNode::user3ClearTree();

        AstCFunc* nbaTopp = new AstCFunc{fl, "nbaTop", scopep, "void"};
        nbaTopp->isMethod(true);
        nbaTopp->isInline(true);
        nbaTopp->dontCombine(true);
        // add the function arg

        AstVar* const trigArgp
            = new AstVar{fl, VVarType::MEMBER, freshName("trigArg"), m_triggering.trigDTypep};
        trigArgp->funcLocal(true);
        trigArgp->direction(VDirection::CONSTREF);
        nbaTopp->addArgsp(trigArgp);
        AstVarScope* const thisTrigVscp = new AstVarScope{classp->fileline(), scopep, trigArgp};
        scopep->addVarsp(thisTrigVscp);

        scopep->addBlocksp(nbaTopp);

        // create class member or function local variable for every variable neeeded
        // by the graphp computations
        makeClassMemberVars(graphp, scopep, classp, instVscp, nbaTopp, fl);

        // create the trigger evaluation function
        auto triggerFunctions = makeTriggerEvalFunc(graphp, classp, scopep, instVscp);
        auto& triggerCheckGen = triggerFunctions.first;
        // triggerCheckGen can be used to create trigger.at(i) expression for each AstSenTree

        // add the computation
        UINFO(5, "Ordering computation" << endl);
        graphp->order();  // order the computation

        // Go through the compute vertices in order and append them to nbaTopp.
        // Each compute vertex has a domainp (nullptr if combinatiol) that
        // determines whether the statement should fire or not. We keep track
        // of the active domains to avoid emitting unnecessary AstIf statements.
        // Alternatively, another pass could try to coalesce the AtsIfs.

        struct {
            AstNode* firstp = nullptr;
            AstIf* lastp = nullptr;
            AstSenTree* domainp = nullptr;
        } currentActive;
        // just add a comment to as firstp to make sure its not null
        currentActive.firstp = new AstComment{fl, "begin nba computation"};

        for (V3GraphVertex* itp = graphp->verticesBeginp(); itp; itp = itp->verticesNextp()) {
            auto vtxp = dynamic_cast<CompVertex* const>(itp);
            if (!vtxp) continue;
            auto vtxDomp = vtxp->domainp();
            auto nodep = vtxp->nodep();
            UASSERT(nodep, "nullptr vertex");
            UASSERT_OBJ(VN_IS(nodep, Always) || VN_IS(nodep, AlwaysPost)
                            || VN_IS(nodep, AssignPost) || VN_IS(nodep, AssignPre)
                            || VN_IS(nodep, AssignW) || VN_IS(nodep, AssignAlias),
                        nodep, "unexpected node type " << nodep->prettyTypeName() << endl);
            auto flatClone = [](AstNode* nodep) {
                if (AstNodeProcedure* procp = VN_CAST(nodep, NodeProcedure)) {
                    return procp->stmtsp()->cloneTree(true);  // clone next
                } else if (AstNodeBlock* blockp = VN_CAST(nodep, NodeBlock)) {
                    return procp->stmtsp()->cloneTree(true);  // clone next
                } else {
                    // do not clone next, PRE and POST are in the same active
                    // but need to be ordered separately.
                    return nodep->cloneTree(false);
                }
            };
            AstNode* const clonep = flatClone(nodep);
            if (currentActive.domainp && vtxDomp) {
                if (vtxDomp != currentActive.domainp) {
                    // changing domain
                    AstIf* const newBlockp
                        = new AstIf{vtxDomp->fileline(), triggerCheckGen[vtxDomp](thisTrigVscp),
                                    clonep, nullptr};
                    currentActive.lastp = newBlockp;
                    currentActive.firstp->addNext(newBlockp);
                } else {
                    // same domain
                    UASSERT(currentActive.lastp, "expected AstIf");
                    currentActive.lastp->addThensp(clonep);
                }
            } else if (!currentActive.domainp && vtxDomp) {
                // entering a new domain from comb
                AstIf* const newBlockp = new AstIf{
                    vtxDomp->fileline(), triggerCheckGen[vtxDomp](thisTrigVscp), clonep, nullptr};
                UASSERT(!currentActive.lastp, "did not expect AstIf");
                currentActive.lastp = newBlockp;
                currentActive.firstp->addNext(newBlockp);
            } else if (currentActive.domainp && !vtxDomp) {
                // leaving seq to comb
                UASSERT(currentActive.lastp, "expected AstIf");
                currentActive.lastp = nullptr;
                currentActive.firstp->addNext(clonep);
            } else if (!currentActive.domainp && !vtxDomp) {
                // comb to comb transition
                UASSERT(!currentActive.lastp, "did not expect AstIf");
                currentActive.firstp->addNext(clonep);
            }
            currentActive.domainp = vtxDomp;
        }

        nbaTopp->addStmtsp(currentActive.firstp);

        { ReplaceOldVarRefsVisitor{nbaTopp}; }

        AstCFunc* cfuncp = new AstCFunc{fl, "compute", scopep, "void"};
        cfuncp->dontCombine(true);
        cfuncp->isMethod(true);
        cfuncp->isInline(true);
        scopep->addBlocksp(cfuncp);

        // AstCMethodCall* const callp = new AstCMethodCall{}
        AstCCall* const callTrigp = new AstCCall{fl, triggerFunctions.second};
        callTrigp->dtypeFrom(thisTrigVscp);
        AstCCall* const callNbap = new AstCCall{fl, nbaTopp, callTrigp};
        callNbap->dtypeSetVoid();
        cfuncp->addStmtsp(callNbap->makeStmt());
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

    // make a top level module with a single "exchange" function that emulates "AssignPost"
    void makeCopyOperations() {
        // AstVarScope::user2 -> true if variable already processed
        VNUser2InUse user2InUse;
        AstNode::user2ClearTree();

        // function to run after computation
        AstCFunc* const copyFuncp
            = new AstCFunc{m_netlistp->topModulep()->fileline(), "exchange", m_topScopep, "void"};
        copyFuncp->dontCombine(true);
        // function to run before everything
        AstCFunc* const initFuncp = new AstCFunc{m_netlistp->topModulep()->fileline(),
                                                 "initialize", m_topScopep, "void"};

        initFuncp->slow(true);
        initFuncp->dontCombine(true);
        // go through all of the old variables and find their new producer and consumers
        // then create a assignments for updating them safely in an "exchange" function.
        // initialization (AstInitial and AstInitialStatic) also get a similar treatment
        // since there is an individual class that performs the initial computation
        // and that needs to be copied as well.
        m_netlistp->foreach([this, &copyFuncp, &initFuncp](AstVarScope* vscp) {
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
            UINFO(400, "Insepcting " << vscp->name() << endl);
            auto& refInfo = m_vscpRefs(vscp);
            // UASSERT_OBJ(!refInfo.hasConsumer()
            //                 || refInfo.producer() /* consumed implies produced*/,
            //             vscp, "consumed but not produced!");
            for (const auto& pair : refInfo.targetsp()) {
                if (refInfo.sourcep().first
                    && refInfo.sourcep() != pair /*no need to send to self*/) {
                    // UASSERT(refInfo.sourcep() != pair, "Self message not allowed!");
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

        // snatch the dpi function prototypes from the old top module
        for (AstNode* np = oldScopep->blocksp(); np;) {
            AstCFunc* funcp = VN_CAST(np, CFunc);
            np = np->nextp();
            if (!funcp) continue;
            UASSERT_OBJ(funcp->dpiImportPrototype(), funcp, "expected function to be inlined");
            // keep the function
            funcp->scopep(m_topScopep);
            UASSERT_OBJ(!funcp->stmtsp(), funcp, "DPI function should not have a body");
            m_topScopep->addBlocksp(funcp->unlinkFrBack());
        }
        oldScopep->replaceWith(m_topScopep);
        VL_DO_DANGLING(oldScopep->deleteTree(), oldScopep);
        VL_DO_DANGLING(senTreep->deleteTree(), senTreep);

        // finally put the top scope in the new top module
        m_topModp->addStmtsp(singletonTopScopep);
        // delete any existing top module in the netlist, but keep the package
        AstNodeModule* oldModsp = m_netlistp->topModulep()->unlinkFrBack();
        for (AstNode* oldNodep = oldModsp->stmtsp(); oldNodep;) {
            AstNode* oldNextp = oldNodep->nextp();
            if (AstTypedef* tdefp = VN_CAST(oldNodep, Typedef)) {
                // keep any typedefs
                m_topModp->addStmtsp(tdefp->unlinkFrBack());
            } else if (AstCell* cellp = VN_CAST(oldNodep, Cell)) {
                if (cellp->modp()->inLibrary()) {
                    // a library package or something, keep it under the new top module,
                    // this requires replacing the scope below
                    cellp->modp()->foreach([this, cellp](AstScope* scopep) {
                        // scopep->modp()
                        scopep->modp(m_topModp);
                        scopep->aboveScopep(m_topScopep);
                    });
                    m_topModp->addStmtsp(cellp->unlinkFrBack());
                }
            }
            oldNodep = oldNextp;
        }

        VL_DO_DANGLING(oldModsp->deleteTree(), oldModsp);
        // add the new topmodule (should be first, see AstNetlist::topModulesp())
        if (m_netlistp->modulesp()) {
            m_netlistp->modulesp()->addHereThisAsNext(m_topModp);
        } else {
            m_netlistp->addModulesp(m_topModp);
        }
    }

    void makeComputeSet(const std::vector<AstClass*> computeClassesp,
                        const std::string& funcName) {

        AstCFunc* computeSetp
            = new AstCFunc{m_netlistp->topModulep()->fileline(), funcName, m_topScopep, "void"};
        computeSetp->dontCombine(true);
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
        //      AstVarScope::user2   -> true if ever written
        VNUser3InUse user3InUse;
        VNUser2InUse user2InUse;
        AstNode::user3ClearTree();
        AstNode::user2ClearTree();
        auto setWritten = [this](AstNode* nodep) {
            nodep->foreach([](AstVarRef* vrefp) {
                if (vrefp->access().isWriteOrRW()) { vrefp->varScopep()->user2(true); }
            });
        };
        auto replaceOldVarRef = [this, &scopep, &classp, &cfuncp, &instVscp](AstVarRef* vrefp) {
            AstVarScope* oldVscp = vrefp->varScopep();
            AstVarScope* substp = VN_CAST(oldVscp->user3p(), VarScope);
            if (!substp) {
                AstVar* varp = new AstVar{vrefp->varScopep()->varp()->fileline(), VVarType::MEMBER,
                                          freshName(oldVscp), oldVscp->varp()->dtypep()};
                substp = new AstVarScope{oldVscp->fileline(), scopep, varp};
                substp->trace(oldVscp->isTrace());
                scopep->addVarsp(substp);
                oldVscp->user3p(substp);
                auto& refInfo = m_vscpRefs(oldVscp);
                // if the variable is consumed by any of the graph nodes, then
                // we need to add it as a class level member, otherwise, it should
                // be kept local to the function

                if (oldVscp->user2() /*written by the initial*/
                    && (refInfo.hasConsumer() || refInfo.producer()
                        /* even if the producer does not consumed the variable, we need to
                        propagate the initialized value. Subword assignment is wrongly
                        considered as only production, but is in fact a read-modify-write
                        operations*/
                        )) {
                    // note that checking user2 is only done to prevent promoting
                    // a variable that is consumed by the nba regions and only read
                    // here to the variable that is produced by the initial block and
                    // (hence sent out after initializaiton). If we don't do this
                    // check functionality should remain the same since we are basically
                    // sending out and undefined variable.
                    UINFO(300, "Adding init member " << oldVscp->name() << endl);
                    classp->addStmtsp(varp);
                    m_vscpRefs(oldVscp).initp({instVscp, varp});
                } else {
                    UINFO(300, "Adding init local " << oldVscp->name() << endl);
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
        m_initialStatics.foreachLogic(setWritten);
        m_initials.foreachLogic(setWritten);

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
                               const V3Sched::LogicByScope& statics,
                               const V3Sched::LogicByScope& actives)
        : m_modNames{"__VBspCls"}
        , m_memberNames{"__VBspMember"}  // reset on partition
        , m_netlistp{netlistp}
        , m_partitionsp{partitionsp}
        , m_initials{initials}
        , m_initialStatics{statics}
        , m_actives{actives} {}
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
        makeCopyOperations();
        makeComputeSet({initClassp}, "initComputeSet");
        makeComputeSet(submodp, "computeSet");
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
                               const V3Sched::LogicByScope& statics,
                               const V3Sched::LogicByScope& actives) {

    {
        ModuleBuilderImpl builder{netlistp, partitionsp, initials, statics, actives};
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