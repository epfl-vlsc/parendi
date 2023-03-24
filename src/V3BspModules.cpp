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

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3BspSched {
namespace {

class VarScopeReferences {
private:
    DepGraph* m_producer = nullptr;
    std::vector<DepGraph*> m_consumer;

public:
    inline bool isOwned(const std::unique_ptr<DepGraph>& graphp) const {
        return graphp.get() == m_producer;
    }

    inline bool isLocal() const {
        return !m_producer || (m_consumer.size() == 1 && m_consumer.front() == m_producer);
    }


    inline bool isRemote(const std::unique_ptr<DepGraph>& graphp) const {
        return !isLocal() && !isOwned(graphp);
    }
    inline void producer(const std::unique_ptr<DepGraph>& graphp) {
        UASSERT(!m_producer, "multiple producers!");
        m_producer = graphp.get();
    }
    DepGraph* producer() const { return m_producer; }
    void consumer(const std::unique_ptr<DepGraph>& graphp) { m_consumer.push_back(graphp.get()); }
    const std::vector<DepGraph*>& consumer() const { return m_consumer; }
    VarScopeReferences() {}
    ~VarScopeReferences() = default;
};

class ModuleBuilderImpl final {
private:
    // NODE STATE
    //      VarScope::user1     -> consumers and producer of the variable
    //      VarScope::user2     -> true if added to partition
    VNUser1InUse user1InUse;
    VNUser1InUse user2InUse;
    AstUser1Allocator<AstVarScope, VarScopeReferences> m_vscpRefs;
    V3UniqueNames m_modNames;
    const AstNetlist* m_netlistp;  // original netlist
    const std::vector<std::unique_ptr<DepGraph>>& m_partitionsp;  // partitions
    std::vector<AstModule*> m_modulesp;
    AstModule* m_originalTopModp = nullptr;
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

    void mkModuleVars() {
        // AstCMethodCall
        // AstCFunc* loadLocal = nullptr;
        // AstCMethodHard* loadLocal = nullptr;
        for (const auto& graphp : m_partitionsp) {
            AstModule* modp
                = new AstModule{m_netlistp->fileline() /*maybe do better that netlist..*/,
                                m_modNames.get("process"), true};
            AstScope* scopep = new AstScope{modp->fileline(), modp, "STATE", nullptr, nullptr};
            V3UniqueNames inputNames{"__VbspInput"};
            V3UniqueNames outputNames{"__VbspOutput"};
            V3UniqueNames memoryNames{"__VbspMemory"};
            V3UniqueNames localNames{"__VbspLocal"};
            modp->addStmtsp(scopep);
            graphp->modp(modp);


            AstNode::user2ClearTree();
            for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp;
                 vtxp = vtxp->verticesNextp()) {
                if (ConstrVertex* constrp = dynamic_cast<ConstrVertex*>(vtxp)) {
                    AstVarScope* const vscp = constrp->vscp();
                    if (!vscp->user2()) {
                        // scenarios:
                        // 1. read only UnpackedArrayDType
                        //      create a local variable
                        // 2. read-write UnpackedArrayDType
                        //      create an InOut variable
                        // 3. BasicDType variable
                        // 3.1 produced here and consumed here only (call it var)
                        //     make MODULETEMP var
                        //     add var = LOADLOCAL(var, index);
                        //

                        // add any variable reference in the partition to the local scope
                        scopep->createTempLike(vscp->name(), vscp);
                        // for any local variable that is produced by another graph we need
                        // to create an input variable
                        const auto& refInfo = m_vscpRefs(vscp);

                        if (!refInfo.isOwned(graphp)) {
                            // variable not produced by graphp, but is referenced by it

                        } else {

                        }
                        if (refInfo.isOwned(graphp) && !refInfo.isLocal()) {
                            // variable is produced by graphp and consumed by others
                            // we need to send it out

                        }
                        vscp->user2(true);
                    }
                }
            }
        }

        for (const auto& graphp : m_partitionsp) {}
        m_originalTopModp->foreach([this](const AstVarScope* origVscp) {
            const auto& refs = m_vscpRefs(origVscp);
            if (refs.isLocal()) {
                // variable is local to just a single graph
                // modp
            }
        });
    }

public:
    explicit ModuleBuilderImpl(AstNetlist* netlistp,
                               const std::vector<std::unique_ptr<DepGraph>>& partitionsp)
        : m_modNames{"__VBspModule"}
        , m_netlistp{netlistp}
        , m_partitionsp{partitionsp} {}
    void go() {
        // find the top module
        m_netlistp->foreach([this](const AstTopScope* m_topScopep) {
            UASSERT_OBJ(!m_originalTopModp, m_topScopep->scopep()->modp(),
                        "Multiple top modules! Should flatten");
            m_originalTopModp = VN_AS(m_topScopep->scopep()->modp(), Module);
        });
        // 1. Determine producer and consumers
        computeReferences();
        // 2. Create modules and variables using
        mkModuleVars();
        for (const auto& graphp : m_partitionsp) {
            AstModule* modp
                = new AstModule{m_netlistp->fileline() /*maybe do better that netlist..*/,
                                m_modNames.get("process"), true};
            AstScope* scopep = new AstScope{modp->fileline(), modp, "STATE", nullptr, nullptr};
            modp->addStmtsp(scopep);

            // mkModuleVars(modp, graphp);
        }
    }
    std::vector<AstModule*> modules() const { return m_modulesp; }
};
}  // namespace

std::vector<AstModule*>
V3BspModules::makeModules(AstNetlist* netlistp,
                          const std::vector<std::unique_ptr<DepGraph>>& partitionsp) {

    // We create one module per graph. Basically each module will have the
    // following signature:
    // MODULE
    //   VAR in1, in2, in3,...
    //   VAR out1, out2, out3, ...
    //   VAR Local
    //   VAR Serial
    //   VAR Exception
    //   SCOPE original_varscopes
    //   CFUNC compute
    //   CFUNC send

    ModuleBuilderImpl builder{netlistp, partitionsp};
    builder.go();
    return builder.modules();
}

};  // namespace V3BspSched