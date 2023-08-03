// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Bulk-synchronous parallel  scheduling
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
// V3BspSched::schedule is the top level entry-point for a parallel scheduling
// of simulation. This scheduling mode is currently quite limited:
//  - Only modules with a single clock at the top (without combinational inputs) are supported
//  - Combinational loops are not supported, the scheduler simply fails if they exist.
//  - The code does not attempt to optimize/elide __dly variables.
//  - The top clock is automatically toggled, hence there is no need for a testbench
//  - This pass is intended for generating code for message-passing machines, hence
//    some actions taken during partitioning may not make a whole lot of sense
//    (in terms of performance) on a shared-memory general purpose machine.
// In general the idea is to provide better parallel performance with minimum
// synchronization compared to the Verilator's original parallel partitioning
// algorithm. In BSP we minimize the synchonization cost creating a separate
// communication and computation phase. Basically the schedule follows this form:
//  1. Comb logic
//  2. AssignPre logic
//  3. Sequential logic
//  4. Barrier synchronization
//  5. AssignPost logic
//  6. Barrier synchronization
//  7. Jump to 1
//  All steps are parallel, and it involves replicating combinational logic if needed.
//  To do this, we build a fine-grained data dependence graph. This graph is different
//  from what V3Order builds since in V3Order comb logic is scheduled after AssignPost.
//  Not that executing comb logic first is fine as long as there are no inputs to the
//  design since in this case the whole design is a cyclic graph and we arbitrarily break
//  it after every AssignPost that may produce a value for comb or AssignPre logic.
//  Note that in this graph, the AssignPost logic nodes constitute the sink nodes.
//  From these sink nodes, we create "many" parallel processes by collecting all the logic
//  required to compute them. In essense, there will be up to N processes where N is
//  is the number of AssignPost nodes in the AST.
//  There are some other constraints that apply and limit the total number of processes.
//  Namely, r/w unpacked arrays constraint the number of processes. Suppose M is one such
//  array, then it should be the case only a single process references M.
//  To increase parallelism, it make sense to break unpacked arrays with constat ArraySel
//  operations into a list of packed arrays, e.g.,:
//      logic [B - 1 : 0] M [0: S - 1];
//  Where all references to M have constant ArraySel indices should become:
//      logic [B - 1 : 0] M0, M1, M2, ..., MS_1;
//  We don't perfrom this optimization here, as in general this should help with
//  even a non BSP schedule (opens up optimization opportunities, e.g., reveals dead code).
//
//  To respect these constraints we need to create disjoint sets of "non-sharable" resources.
//  A resources is the LHS of AssignPost or a read-write unpacked array.
//  Each disjoint set is then used to traverse the dependence graph bottom up and
//  collect nodes required for its computation.
//  Two disjoint sets may need the same combinational logic nodes and we unconditionally
//  duplicate them all, e.g.:
//      wire [..] mywire = combExpr(...);
//      always @(posedge clock) x <= xExpr(mywire);
//      always @(posedge clock) y <= yExpr(mywire):
//  will result in:
//      processX() { mywireCopyx = combExpr(..); x = xExpr(mywireCopyx); }
//      processY() { mywireCopyy = combExpr(..); x = yExpr(mywireCopyy); }
//
//
//*************************************************************************
#include "config_build.h"
#include "verilatedos.h"

// reuse some code from V3Sched
#include "V3Ast.h"
#include "V3BspGraph.h"
#include "V3BspMerger.h"
#include "V3BspModules.h"
#include "V3BspResync.h"
#include "V3BspRetiming.h"
#include "V3EmitCBase.h"
#include "V3EmitV.h"
#include "V3Order.h"
#include "V3Sched.h"
#include "V3SenExprBuilder.h"
#include "V3Stats.h"

VL_DEFINE_DEBUG_FUNCTIONS;
namespace V3BspSched {
namespace details {
V3Sched::LogicClasses gatherLogicClasses(AstNetlist* netlistp) {
    V3Sched::LogicClasses result;

    netlistp->foreach([&](AstScope* scopep) {
        std::vector<AstActive*> empty;

        scopep->foreach([&](AstActive* activep) {
            AstSenTree* const senTreep = activep->sensesp();
            if (!activep->stmtsp()) {
                // Some AstActives might be empty due to previous optimizations
                empty.push_back(activep);
            } else if (senTreep->hasStatic()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "static initializer with additional sensitivities");
                result.m_static.emplace_back(scopep, activep);
            } else if (senTreep->hasInitial()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "'initial' logic with additional sensitivities");
                result.m_initial.emplace_back(scopep, activep);
            } else if (senTreep->hasFinal()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "'final' logic with additional sensitivities");
                result.m_final.emplace_back(scopep, activep);
            } else if (senTreep->hasCombo()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "combinational logic with additional sensitivities");
                if (VN_IS(activep->stmtsp(), AlwaysPostponed)) {
                    result.m_postponed.emplace_back(scopep, activep);
                } else {
                    result.m_comb.emplace_back(scopep, activep);
                }
            } else {
                UASSERT_OBJ(senTreep->hasClocked(), activep, "What else could it be?");

                if (VN_IS(activep->stmtsp(), AlwaysObserved)) {
                    activep->v3warn(E_UNSUPPORTED, "Can not handle observed in BSP");
                } else if (VN_IS(activep->stmtsp(), AlwaysReactive)) {
                    activep->v3warn(E_UNSUPPORTED, "Can not handle Reactive in BSP");
                } else {
                    result.m_clocked.emplace_back(scopep, activep);
                }
            }
        });

        for (AstActive* const activep : empty) activep->unlinkFrBack()->deleteTree();
    });

    return result;
}

};  // namespace details

std::tuple<V3Sched::LogicClasses, V3Sched::LogicRegions, std::vector<std::unique_ptr<DepGraph>>>
buildDepGraphs(AstNetlist* netlistp) {

    // Step 1. classify logic classes, may error out on unsupported logic classes
    V3Sched::LogicClasses logicClasses = details::gatherLogicClasses(netlistp);

    auto unsupportedWhy = [](const auto& region, const string& reason) {
        if (!region.empty()) { region.front().second->v3warn(E_UNSUPPORTED, "    " << reason); }
    };

    // Step 3. check for comb cycles and error
    logicClasses.m_hybrid = V3Sched::breakCycles(netlistp, logicClasses.m_comb);

    unsupportedWhy(logicClasses.m_hybrid, "Hybrid logic means there is some combinational loop");
    unsupportedWhy(logicClasses.m_final, "Final logic not implemented, too lazy");
    unsupportedWhy(logicClasses.m_observed, "Observed region is out of the scope of this work");
    unsupportedWhy(logicClasses.m_postponed, "Postponed not supported");
    unsupportedWhy(logicClasses.m_reactive, "Reactive not supported");
    // Step 4. not really needed to settle the logic since we expect no inputs

    // Step 5. partition the logic into pre-active, active, and NBA regions.
    // In this mode, only a non-empty NBA region is valid. Any other non-empty
    // region indicates the existence of external inputs to the module which we
    // do not yet support
    V3Sched::LogicRegions logicRegions
        = V3Sched::partition(logicClasses.m_clocked, logicClasses.m_comb, logicClasses.m_hybrid);

    unsupportedWhy(logicRegions.m_pre,
                   "Pre-active not supprted because as can only handle a single clock");

    unsupportedWhy(logicRegions.m_act, "active region computation is not fully supported");
    V3Sched::LogicByScope& nbaLogic = logicRegions.m_nba;
    // Step 6. make a fine-grained dependence graph. This graph is different from
    // the V3Order graph in many ways but the most notably difference is wrt ordering
    // of combinational logic. This graph pushes combinational logic before clocked
    // logic, in parallel with AssignPre logic.
    V3Stats::statsStage("partitionLogic");
    std::unique_ptr<DepGraph> graphp = DepGraphBuilder::build(nbaLogic);
    if (dumpGraph() >= 3) { graphp->dumpDotFilePrefixed("nba_orig"); }
    V3Stats::statsStage("dagGeneration");
    // Step 7. Break the dependence graph into a maximal set of indepdent parallel
    // graphs
    std::vector<std::unique_ptr<DepGraph>> splitGraphsp;
    if (v3Global.opt.tiles() == 1 && v3Global.opt.workers() == 1 && graphp->verticesBeginp()) {
        splitGraphsp.emplace_back(std::move(graphp));
    } else if (graphp->verticesBeginp()) {
        splitGraphsp = DepGraphBuilder::splitIndependent(graphp);
    }
    V3Stats::statsStage("dagSplit");
    return {std::move(logicClasses), std::move(logicRegions), std::move(splitGraphsp)};
}

void schedule(AstNetlist* netlistp) {

    if (dumpTree() >= 3) {
        UINFO(0, "Emitting verilog\n");
        V3EmitV::debugEmitV(v3Global.debugFilename("pre-bsp") + ".v");
    }
    if (v3Global.opt.fIpuRetime()) {
        Retiming::retimeAll(netlistp);
        V3Stats::statsStage("bspRetime");
    } else if (v3Global.opt.fIpuResync()) {
        Resync::resyncAll(netlistp);
        V3Stats::statsStage("bspResync");
    }

    auto deps = buildDepGraphs(netlistp);
    auto& splitGraphsp = std::get<2>(deps);
    auto& logicClasses = std::get<0>(deps);
    auto& logicRegions = std::get<1>(deps);
    V3Stats::statsStage("bspGraph");
    // merge small partitions into larger ones
    if (v3Global.opt.fIpuMerge()) {
        V3BspMerger::merge(splitGraphsp);
        V3Stats::statsStage("bspMerge");
    }
    // Create a module for each DepGraph. To do this we also need to determine
    // whether a varialbe is solely referenced locally or by multiple cores.
    V3BspModules::makeModules(netlistp, splitGraphsp, logicClasses.m_initial,
                              logicClasses.m_static, logicRegions.m_act);

    // std::exit(0);
}

};  // namespace V3BspSched