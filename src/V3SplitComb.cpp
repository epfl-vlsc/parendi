// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Split always_combs
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
// Author: Mahyar Emami
//*************************************************************************
// step1:
// for each always_comb find the set of LVs, if the always comb has DPI,
// return an empty set
// step2:
// if set.size() > 1:
// for each lv in set:
//     for each var in set that is not lv, create a blocktemp clone variable
//     clone always block and replace all the reference (LV and RV) of var
//     with blocktemp.
//     finaly prepend blocktemp = var to the always block.
// once all is done, remove dead code
//     blktmps = [clone var for var in set - lv]
//     clone always block, prepend [blk = var for blk, var in blk]
//
// for each always_comb block find the set of LVs
//      for in lv in LVs create a new always_comb block that
#include "config_build.h"
#include "verilatedos.h"

#include "V3SplitComb.h"

#include "V3Ast.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <set>
#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

// find all the LVs in an always block, return none if there is PLI
class CollectLVsVisitor : public VNVisitor {
private:
    bool m_unopt = false;
    std::set<AstVarScope*> m_lvsp;

    void visit(AstVarRef* vrefp) override {
        if (m_unopt) return;
        if (vrefp->access().isWriteOrRW()) { m_lvsp.insert(vrefp->varScopep()); }
    }

    void visit(AstAlways* nodep) override { iterateChildren(nodep); }
    // void visit(AstJumpGo* nodep) override {
    //     // maybe splittable, but unsure since deadcode may not be able to clean
    //     // afterwards
    //     UINFO(9, "        Jump prevents split " << nodep << endl);
    //     m_unopt = true;
    // }
    void visit(AstNode* nodep) override {
        if (!nodep->isPure()) {  // PLI or something weird, better not split
            UINFO(7, "        Impure prevents split " << nodep << endl);
            m_unopt = true;
        }
        iterateChildren(nodep);
    }

public:
    explicit CollectLVsVisitor(AstAlways* alwaysp) { iterate(alwaysp); }
    std::set<AstVarScope*> lvsp() {
        if (m_unopt) {
            return std::set<AstVarScope*>{};
        } else {
            return m_lvsp;
        }
    }
};

using SubstMap = std::unordered_map<AstVarScope*, AstVarScope*>;

class VarRefSubstitionVisitor : public VNVisitor {
private:
    const SubstMap& m_substp;
    void visit(AstNodeVarRef* vrefp) override {
        // just in case
        UASSERT_OBJ(VN_IS(vrefp, VarRef), vrefp, "unknown NodeVarRef type " << vrefp << endl);
        auto it = m_substp.find(vrefp->varScopep());
        if (it != m_substp.end()) {
            AstVarRef* newVrefp = new AstVarRef{vrefp->fileline(), it->second, vrefp->access()};
            vrefp->replaceWith(newVrefp);
            VL_DO_DANGLING(pushDeletep(vrefp), vrefp);
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit VarRefSubstitionVisitor(AstAlways* alwaysp, const SubstMap& substp)
        : m_substp(substp) {
        iterate(alwaysp);
    }
};

using LogicSet = std::unordered_set<AstNode*>;
using LogicMap = std::map<AstVarScope*, LogicSet>;
class LValueLogicVisitor : public VNVisitor {
private:
    // A map from varscopes to the logic that assigns as LV
    LogicMap m_producer;
    AstNode* m_logicp = nullptr;

    void iterateLogic(AstNode* nodep) {
        UASSERT_OBJ(!m_logicp, nodep, "should not nest logic!");
        m_logicp = nodep;
        iterateChildren(nodep);
        m_logicp = nullptr;
    }

    // Map LVs to their producer logic(s)
    void visit(AstNodeVarRef* vrefp) override {
        if (!m_logicp) return;
        if (vrefp->access().isWriteOrRW()) {
            auto it = m_producer.find(vrefp->varScopep());
            UINFO(15, "    " << vrefp->varScopep()->prettyName() << " produced by "
                             << m_logicp->prettyTypeName() << " " << cvtToHex(m_logicp) << endl);
            if (it == m_producer.end()) {
                m_producer.emplace(vrefp->varScopep(), std::unordered_set<AstNode*>{m_logicp});
            } else {
                it->second.emplace(m_logicp);
            }
        }
    }

    // always like
    void visit(AstInitial* nodep) override { iterateLogic(nodep); }
    void visit(AstInitialStatic* nodep) override { iterateLogic(nodep); }
    void visit(AstInitialAutomatic* nodep) override { iterateLogic(nodep); }
    void visit(AstAlways* nodep) override { iterateLogic(nodep); }
    void visit(AstAlwaysPost* nodep) override { iterateLogic(nodep); }
    void visit(AstAlwaysObserved* nodep) override { iterateLogic(nodep); }
    void visit(AstAlwaysReactive* nodep) override { iterateLogic(nodep); }
    void visit(AstFinal* nodep) override { /*does not produce*/
    }
    // assignmenst outside always blocks
    void visit(AstAssignW* nodep) override { iterateLogic(nodep); }
    void visit(AstAssignAlias* nodep) override { iterateLogic(nodep); }
    void visit(AstAssignPre* nodep) override { iterateLogic(nodep); /* not really needed right?*/ }
    void visit(AstAssignPost* nodep) override {
        iterateLogic(nodep); /* not really needed right?*/
    }

    // internal mutations
    void visit(AstAlwaysPublic* nodep) override {  //
        iterateLogic(nodep);
    }
    void visit(AstCoverToggle* nodep) override {  //
        iterateLogic(nodep);
    }

    // VISITOR
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit LValueLogicVisitor(AstNetlist* netlistp) { iterate(netlistp); }

    inline LogicMap map() const { return m_producer; }
};

class SplitMarkKeepVisitor : public VNVisitor {
private:
    std::unordered_set<AstNode*> m_keepp;
    enum Step : int { CHECK_NONE = 0, CHECK_LV = 1, KEEP_RV = 2, POST_ALIVE = 4 };
    int m_step = CHECK_NONE;

    void visit(AstNodeVarRef* vrefp) override {
        if ((m_step | CHECK_LV) && vrefp->access().isWriteOrRW()
            && m_keepp.count(vrefp->varScopep())) {
            m_step = m_step | POST_ALIVE;

        } else if ((m_step == KEEP_RV) && vrefp->access().isReadOrRW()) {
            m_keepp.insert(vrefp->varScopep());
            UINFO(11, "        variable " << vrefp->varScopep() << " is alive " << endl);
        }
    }
    void visit(AstNode* nodep) override {
        m_step = CHECK_LV;  // assume nodep is dead
        // iterate childrend backwards and look at any LV reference,
        // if the LV reference is a live one, come back here and mark any RV
        // reference below here as also live
        UINFO(11, "        iterating node " << nodep->prettyTypeName() << "  " << cvtToHex(nodep)
                                            << endl);
        iterateChildrenBackwards(nodep);
        if (m_step & POST_ALIVE) {
            UINFO(10, "        Keeping node " << nodep->prettyTypeName() << " " << cvtToHex(nodep)
                                              << endl);
            m_step = KEEP_RV;
            iterateChildrenBackwards(nodep);
        }
    }

public:
    explicit SplitMarkKeepVisitor(AstAlways* alwaysp, AstVarScope* alivep)
        : m_keepp{alivep} {
        UINFO(10, "    Marking " << alivep->prettyNameQ() << " as alive" << endl);
        iterateChildrenBackwards(alwaysp);
    }
    static std::unordered_set<AstNode*> keepers(AstAlways* alwaysp, AstVarScope* alivep) {
        return SplitMarkKeepVisitor{alwaysp, alivep}.m_keepp;
    }
};

class SplitRemoveDeadStmtsVisitor : public VNVisitor {
private:
    const std::unordered_set<AstNode*>& m_keepp;

    void visit(AstNodeAssign* assignp) override {
        // bool keep =
        const bool keep = assignp->exists([this](AstNodeVarRef* vrefp) {
            return vrefp->access().isWriteOrRW() && m_keepp.count(vrefp->varScopep());
        });
        if (!keep) {
            if (assignp->backp()) { assignp->unlinkFrBack(); }
            VL_DO_DANGLING(assignp->deleteTree(), assignp);
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit SplitRemoveDeadStmtsVisitor(AstAlways* alwaysp,
                                         const std::unordered_set<AstNode*>& keepersp)
        : m_keepp(keepersp) {
        iterateChildren(alwaysp);
    }
};

class SplitCombVisitor : public VNVisitor {
private:
    V3UniqueNames m_duplNames;
    VDouble0 m_statsSplits;

    AstScope* m_scopep = nullptr;
    AstVarScope* m_resultVscp = nullptr;
    using AlwaysVec = std::vector<AstAlways*>;

    std::unordered_map<AstAlways*, AlwaysVec> m_splitsp;
    // map varscopes to their correspondig production logic
    const LogicMap m_logics;

    void makeTempVars(AstAlways* alwaysp, std::function<bool(AstVarScope* const)>&& cond) {}
    void visit(AstScope* scopep) override {
        UASSERT(!m_scopep, "nested scopes not allowed");
        m_scopep = scopep;
        iterateChildren(scopep);
        m_scopep = nullptr;
    }

    bool localProduction(AstAlways* const alwaysp, AstVarScope* const vscp) const {
        auto it = m_logics.find(vscp);
        // What is going on? Maybe borken LValueMapVisitor?
        UASSERT_OBJ(it != m_logics.end(), vscp, "broken LVMap");
        UASSERT_OBJ(it->second.count(alwaysp), vscp, "broken LVMap set");
        return it->second.size() == 1;
    }
    void visit(AstAlways* alwaysp) override {
        std::set<AstVarScope*> lvsp = CollectLVsVisitor{alwaysp}.lvsp();
        if (lvsp.size() <= 1) {
            return;  // nothing to do
        }

        m_splitsp.emplace(alwaysp, AlwaysVec{});
        AlwaysVec& newCombsp = m_splitsp[alwaysp];
        for (const auto& targetp : lvsp) {
            // clone the always block

            AstAlways* newAlwaysp = alwaysp->cloneTree(false);
            // find all the node within the clone that we wish to keep
            const std::unordered_set<AstNode*> keepNodep
                = SplitMarkKeepVisitor::keepers(newAlwaysp, targetp);
            // clean anything not needed
            { SplitRemoveDeadStmtsVisitor{newAlwaysp, keepNodep}; }

            if (!newAlwaysp->stmtsp()) {
                continue;  // nothing lef after dead code removal
            }
            // for any varialbe other than targetp, create block temp substitutions
            std::unordered_map<AstVarScope*, AstVarScope*> substp;
            for (const auto& lvp : lvsp) {
                if (lvp != targetp && keepNodep.count(lvp)) {
                    AstVar* const newVarp
                        = new AstVar{lvp->varp()->fileline(), VVarType::BLOCKTEMP,
                                     m_duplNames.get(lvp->varp()->name()), lvp->varp()->dtypep()};
                    newVarp->lifetime(VLifetime::AUTOMATIC);
                    m_scopep->modp()->addStmtsp(newVarp);
                    AstVarScope* const newVscp
                        = new AstVarScope{lvp->fileline(), m_scopep, newVarp};
                    m_scopep->addVarsp(newVscp);
                    substp.emplace(lvp, newVscp);
                }
            }

            // and go through each statement and apply substitutions
            { VarRefSubstitionVisitor{newAlwaysp, substp}; }
            AstNode* const stmtps = newAlwaysp->stmtsp()->unlinkFrBackWithNext();
            for (const auto& oldp : lvsp) {
                if (oldp == targetp || localProduction(alwaysp, oldp)
                    || keepNodep.count(oldp) == 0)
                    continue;
                // if this variable is not uniquely produced here, then we
                // need to pre-assign it.
                UASSERT_OBJ(substp.count(oldp), oldp, "no subst?");
                AstVarScope* const newVscp = substp.find(oldp)->second;
                // the temp variable is pre-assigned to the original variable.
                // This could be potentially wasteful, and expenseive for wide
                // values that are only partially modified inside the block.
                // I wonder if there is a simple trick to make it more efficient?
                AstAssign* const assignp = new AstAssign{
                    oldp->fileline(), new AstVarRef{oldp->fileline(), newVscp, VAccess::WRITE},
                    new AstVarRef{oldp->fileline(), oldp, VAccess::READ}};
                newAlwaysp->addStmtsp(assignp);
            }
            newAlwaysp->addStmtsp(stmtps);
            newCombsp.push_back(newAlwaysp);
        }
    }
    void visit(AstActive* nodep) override {
        // ensure we are running the in the right place, i.e., after V3ActiveTop
        // UASSERT_OBJ(!nodep->sensesStorep(), nodep,
        //             "AstSenTrees should have been made global in V3ActiveTop");

        if (nodep->sensesp()->hasCombo()) {
            // only visit comb blocks
            UASSERT_OBJ(
                nodep->sensesp()->forall([](const AstSenItem* itemp) { return itemp->isCombo(); }),
                nodep, "expected all senses to be combinational");
            m_splitsp.clear();
            iterateChildren(nodep);
            for (const auto& splitpairp : m_splitsp) {
                AstAlways* oldp = splitpairp.first;
                for (AstAlways* const newp : splitpairp.second) {
                    m_statsSplits++;
                    nodep->addStmtsp(newp);
                }
                oldp->unlinkFrBack()->deleteTree();
            }
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit SplitCombVisitor(AstNetlist* netlistp)
        : m_duplNames("__Vspltcomb")
        , m_logics{LValueLogicVisitor{netlistp}.map()} {

        iterate(netlistp);
    }
    ~SplitCombVisitor() override {
        V3Stats::addStat("Optimizations, Split always_comb", m_statsSplits);
    }
};

void V3SplitComb::splitAlwaysComb(AstNetlist* netlistp) {
    UINFO(3, "V3SplitComb:");
    { SplitCombVisitor{netlistp}; }
    v3Global.dumpCheckGlobalTree("splitcomb", 0, dumpTree() >= 3);
}
