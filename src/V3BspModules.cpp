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
    AstVarScope* m_newp = nullptr;  // substition var scope from the writer graphp
    AstPoplarVertexClass* m_popClassp = nullptr;  // the poplar vertex class that prodces the value
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

    std::pair<AstPoplarVertexClass*, AstVarScope*> newp() const {
        return std::make_pair(m_popClassp, m_newp);
    }
    void newp(AstPoplarVertexClass* classp, AstVarScope* newp) {
        UASSERT_OBJ(!m_newp, m_newp, "newp already set in VarScopeReferences!");
        UASSERT_OBJ(!m_popClassp, m_popClassp, "classp already set in VarScopeReferences");
        m_newp = newp;
        m_popClassp = classp;
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
    //      VarScope::user3p    -> substition inside each partition (clear on partitions)
    VNUser1InUse user1InUse;
    VNUser1InUse user2InUse;
    AstUser1Allocator<AstVarScope, VarScopeReferences> m_vscpRefs;
    V3UniqueNames m_modNames;
    V3UniqueNames m_ctorNames;
    AstNetlist* m_netlistp;  // original netlist
    const std::vector<std::unique_ptr<DepGraph>>& m_partitionsp;  // partitions
    std::vector<AstModule*> m_modulesp;
    AstModule* m_originalTopModp = nullptr;

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

    AstModule* makeModule(const std::unique_ptr<DepGraph>& graphp) {

        AstModule* modp = new AstModule{m_netlistp->fileline() /*maybe do better that netlist..*/,
                                        m_modNames.get("module"), true};

        AstScope* newScopep = new AstScope{modp->fileline(), modp, "STATE", nullptr, nullptr};

        AstPoplarVertexComputeMethod* const computep
            = new AstPoplarVertexComputeMethod{m_netlistp->fileline()};
        AstPoplarVertexClass* const classp
            = new AstPoplarVertexClass{m_netlistp->fileline(), m_modNames.get("process")};

        modp->addStmtsp(newScopep);
        newScopep->addBlocksp(classp);
        classp->computep(computep);

        V3UniqueNames inputNames{"__VbspInput"};
        V3UniqueNames outputNames{"__VbspOutput"};
        V3UniqueNames localNames{"__VbspLocal"};

        AstNode::user2ClearTree();
        AstNode::user3ClearTree();
        std::vector<std::pair<AstVarScope*, AstVarScope*>> inputsp;
        std::vector<std::pair<AstVarScope*, AstVarScope*>> outputsp;
        std::vector<std::pair<AstVarScope*, AstVarScope*>> localsp;
        std::vector<std::pair<AstVarScope*, AstVarScope*>> memsp;
        for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
            if (ConstrVertex* constrp = dynamic_cast<ConstrVertex*>(vtxp)) {
                AstVarScope* const vscp = constrp->vscp();
                if (!vscp->user2()) {
                    // add any variable reference in the partition to the local scope
                    // for any local variable that is produced by another graph we need
                    // to create an input variable
                    auto& refInfo = m_vscpRefs(vscp);
                    AstVarScope* newVscp = newScopep->createTempLike(vscp->name(), vscp);
                    if (VN_IS(vscp->dtypep(), BasicDType)) {
                        vscp->user3p(newVscp);
                        if (refInfo.isOwned(graphp) && refInfo.isLocal()) {
                            // a persistent variable that is owned, i.e., written
                            // by a blocked logic contained withing the current
                            // inspected graph
                            localsp.emplace_back(vscp, newVscp);
                        } else if (refInfo.isOwned(graphp) && !refInfo.isLocal()) {
                            // variable is owed/produced here but also referenced
                            // by others
                            outputsp.emplace_back(vscp, newVscp);
                            refInfo.newp(classp, newVscp);  // set this to use later when
                                                            // creating AstPoplarCopyOperations
                            if (refInfo.isConsumed(graphp)) {
                                localsp.emplace_back(vscp, newVscp);
                            }
                        } else if (refInfo.isClocked()) {
                            UASSERT_OBJ(refInfo.isConsumed(graphp), vscp, "Unexpected reference!");
                            // not produced here but consumed
                            inputsp.emplace_back(vscp, newVscp);
                        }
                    } else if (VN_IS(vscp->dtypep(), UnpackArrayDType)) {
                        UASSERT_OBJ(refInfo.isLocal(), vscp, "memory should be local!");
                        memsp.emplace_back(vscp, newVscp);
                    }
                    vscp->user2(true);  // mark visited
                }
            }
        }

        // create PoplarReadVector for every local and input
        AstPoplarVertexMember* inputMembersp
            = new AstPoplarVertexMember{m_netlistp->fileline(), AstPoplarVertexMember::VPOP_INPUT};
        AstPoplarVertexMember* outputMembersp = new AstPoplarVertexMember{
            m_netlistp->fileline(), AstPoplarVertexMember::VPOP_OUTPUT};
        AstPoplarVertexMember* localMembersp
            = new AstPoplarVertexMember{m_netlistp->fileline(), AstPoplarVertexMember::VPOP_LOCAL};
        AstPoplarVertexMember* memoryMembersp = new AstPoplarVertexMember{
            m_netlistp->fileline(), AstPoplarVertexMember::VPOP_MEMORY};
        classp->addVarsp(inputMembersp);
        classp->addVarsp(outputMembersp);
        classp->addVarsp(localMembersp);
        classp->addVarsp(memoryMembersp);

        auto addMemberVar = [this, &modp, &newScopep, &computep](const AstVarScope* const origp,
                                                                 V3UniqueNames& freshNames,
                                                                 const VDirection dir) {
            AstVar* const newVarp
                = new AstVar{origp->fileline(), VVarType::VAR, freshNames.get(origp->name()),
                             m_netlistp->typeTablep()->findPoplarVectorDType(origp->widthWords())};
            newVarp->direction(dir);
            modp->addStmtsp(newVarp);
            AstVarScope* const varScopep
                = new AstVarScope{newVarp->fileline(), newScopep, newVarp};
            newScopep->addVarsp(varScopep);
            return varScopep;
        };
        auto addVectorRead = [&computep](AstVarScope* vecVscp, AstVarScope* scalarVscp,
                                         uint32_t offset) {
            UASSERT_OBJ(scalarVscp->widthWords() > 0, scalarVscp, "invalid number of words");
            AstPoplarReadVector* readExprp = new AstPoplarReadVector{
                vecVscp->fileline(), new AstVarRef{vecVscp->fileline(), vecVscp, VAccess::READ},
                new AstConst{vecVscp->fileline(), AstConst::Unsized32{}, offset},
                new AstConst{vecVscp->fileline(), AstConst::Unsized32{},
                             static_cast<uint32_t>(scalarVscp->widthWords())}};
            AstAssign* assignp = new AstAssign{
                vecVscp->fileline(),
                new AstVarRef{vecVscp->fileline(), scalarVscp, VAccess::WRITE}, readExprp};
            computep->addStmtsp(assignp);
        };

        std::vector<AstPoplarWriteVector*> commitWritesp;
        auto addVectorWrite
            = [&commitWritesp](AstVarScope* vecVscp, AstVarScope* scalarVscp, uint32_t offset) {
                  AstPoplarWriteVector* writeStmtp = new AstPoplarWriteVector{
                      scalarVscp->fileline(),
                      new AstVarRef{vecVscp->fileline(), vecVscp, VAccess::WRITE},
                      new AstVarRef{scalarVscp->fileline(), scalarVscp, VAccess::READ},
                      new AstConst{vecVscp->fileline(), AstConst::Unsized32{}, offset},
                      new AstConst{vecVscp->fileline(), AstConst::Unsized32{},
                                   static_cast<uint32_t>(scalarVscp->widthWords())}};
                  commitWritesp.push_back(writeStmtp);
              };

        for (const auto& inp : inputsp) {
            AstVarScope* vscp = addMemberVar(inp.second, inputNames, VDirection::INPUT);
            inputMembersp->addVarScopesp(vscp);
            addVectorRead(vscp, inp.second, 0);
        }
        uint32_t localOffset = 0;
        for (const auto& locp : localsp) {
            if (!localMembersp->varScopesp()) {
                AstVarScope* vscp = addMemberVar(locp.second, localNames, VDirection::INOUT);
                localMembersp->addVarScopesp(vscp);
            }
            AstVarScope* vscp = localMembersp->varScopesp();
            addVectorRead(vscp, locp.second, localOffset);
            addVectorWrite(vscp, locp.second, localOffset);
            localOffset += vscp->widthWords();
        }
        for (const auto& outp : outputsp) {
            AstVarScope* vscp = addMemberVar(outp.second, outputNames, VDirection::OUTPUT);
            outputMembersp->addVarScopesp(vscp);
            addVectorWrite(vscp, outp.second, 0);
        }

        for (const auto& memp : memsp) { memoryMembersp->addVarScopesp(memp.second); }

        // add the computation
        graphp->order();  // order the computation
        for (V3GraphVertex* itp = graphp->verticesBeginp(); itp; itp = itp->verticesNextp()) {
            if (CompVertex* vtxp = dynamic_cast<CompVertex*>(itp)) {
                // vtxp->nodep
                AstNode* nodep = vtxp->nodep();
                UASSERT(nodep, "nullptr vertex");
                UASSERT_OBJ(VN_IS(nodep, Always) || VN_IS(nodep, AlwaysPost)
                                || VN_IS(nodep, AssignPre),
                            nodep, "unexpected node type!");
                UASSERT_OBJ(!nodep->nextp(), nodep, "Did not expect nextp");

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
                computep->addStmtsp(nodeCopyp);
            }
        }
        // add the final state write
        for (const auto& wp : commitWritesp) { computep->addStmtsp(wp); }
        return modp;
    }
    std::vector<AstModule*> makeModules() {
        std::vector<AstModule*> poplarVertexClassModsp;
        // AstTopScope* topScopep =
        for (const auto& graphp : m_partitionsp) {
            AstModule* modp = makeModule(graphp);
            poplarVertexClassModsp.push_back(modp);
        }

        return poplarVertexClassModsp;
    }

    AstPoplarProgramConstructor* makeProgram(const std::vector)

public:
    explicit ModuleBuilderImpl(AstNetlist* netlistp,
                               const std::vector<std::unique_ptr<DepGraph>>& partitionsp)
        : m_modNames{"__VBspModule"}
        , m_ctorNames{"__VBspCtor"}
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
        // 2. Create modules that contain poplar "Vertex" classes
        std::vector<AstModule*> makeModules();
        // 3. create a poplar program that contains all the modules plus
        // the communication code
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