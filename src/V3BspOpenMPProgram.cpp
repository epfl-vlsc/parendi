// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Lower the program tree create an OpenMP program
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

#include "V3BspOpenMPProgram.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3BspModules.h"
#include "V3EmitCBase.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

/// @brief  replaces AstVarRefs of members of the classes derived from the base
/// V3BspSched::V3BspModules::builtinBaseClass with AstVarRefView so that the code generation
/// can simply emit either a reinterpret_cast or a placement new. We do this because the OpenMP
/// classes could only have openmp::Vector<> as their members and these Vector are basically
/// opaque pointers for use. So we need to cast them to appropriate types.
class OpenMPViewsVisitor final : public VNVisitor {
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

    /// Add VL_ASSIGN_W, VL_VIEW, field name as comment

    void visit(AstVarRef* vrefp) override {
        if (!m_classp) { return; }
        if (!vrefp->varp()->user1()) { return; }
        if (vrefp->user1()) { return; /*processed*/ }
        vrefp->user1(true);

        // wrap the reference in an AstVarView, this is essential for OpenMP since
        // the member variables are Vectors of contiguous data and we wish to
        // view them as Verilator data types, like VlWide and VlUnpackArray
        VNRelinker relinkHandle;
        vrefp->unlinkFrBack(&relinkHandle);
        UINFO(100, "Wrapping " << vrefp->name() << " in AstVarRefView" << endl);
        AstVarRefView* newp = new AstVarRefView{vrefp->fileline(), vrefp};
        // change vrefp dtype to become a VectorDType
        // vrefp->dtypeFrom(vrefp->varp());
        relinkHandle.relink(newp);
    }

    /// Add VL_ASSIGN_W, VL_VIEW, field name as comment

    void visit(AstClass* nodep) override {
        VL_RESTORER(m_classp);
        if (nodep->flag().isBsp()) {
            m_classp = nodep;
            AstNode::user1ClearTree();
            std::vector<AstVar*> members;
            for (AstNode* stmtp = nodep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                if (AstVar* varp = VN_CAST(stmtp, Var)) {
                    varp->user1(true);
                    // members.push_back(varp);
                    // varp->dtypep(vectorTypep(varp->dtypep()));
                }
            }
            iterateChildren(nodep);
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit OpenMPViewsVisitor(AstNetlist* nodep) {
        m_netlistp = nodep;
        iterate(nodep);
    }
};

/// Convert variable name to fieldNN

class OpenMPLegalizeFieldNamesVisitor final {
public:
    explicit OpenMPLegalizeFieldNamesVisitor(AstNetlist* netlistp) {
        // Do not do this for OpenMP.
    }
};

namespace {

struct TensorHandle {
    std::string tensor;
    std::string hostRead;
    std::string hostWrite;
    int id;
    bool isReq;
    TensorHandle()
        : id(std::numeric_limits<int>::max()) {
        tensor.erase();
        hostRead.erase();
        hostWrite.erase();
        isReq = false;
    }
};

}  // namespace

class OpenMPComputeGraphBuilder final : public VNDeleter {
private:
    AstNetlist* m_netlistp = nullptr;
    V3UniqueNames m_newNames;
    AstBasicDType* m_ctxTypep = nullptr;
    AstBasicDType* m_tensorTypep = nullptr;
    AstBasicDType* m_vtxRefTypep = nullptr;
    AstVar* m_ctxVarp = nullptr;
    AstVarScope* m_ctxVscp = nullptr;
    AstVar* m_numCoresVarp = nullptr;
    AstVarScope* m_numCoresVscp = nullptr;
    int m_nextTensorId = 0;
    VNUser1InUse m_user1InUse;
    AstUser1Allocator<AstVar, TensorHandle> m_handles;

    // dump this matrix for summarization
    std::unique_ptr<std::ofstream> m_exchangeDump;

    void initBuiltinTypes() {
        auto newType = [this](VBasicDTypeKwd kwd) {
            AstBasicDType* const typep
                = new AstBasicDType{m_netlistp->fileline(), kwd, VSigning::UNSIGNED};
            m_netlistp->typeTablep()->addTypesp(typep);
            return typep;
        };
        m_ctxTypep = newType(VBasicDTypeKwd::OPENMP_CONTEXT);
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

    uint32_t getVectorSize(AstNodeDType* const dtypep) {
        const AstBasicDType* const basicp = VN_CAST(dtypep->skipRefp(), BasicDType);
        if (basicp && basicp->isTriggerVec()) {
            // trigger vec is internally considered a bit vector, but the implementation
            // uses a std::array<uint32_t, WIDTH>, hence we need to allocate not just one
            // word, but WIDTH words!
            return basicp->width();
        } else {
            return dtypep->widthWords() * dtypep->arrayUnpackedElements();
        }
    }

    AstCFunc* createVertexCons(AstClass* classp, uint32_t tileId) {
        // OpenMP does not need code generated for constructors, but the other
        // metadata updating is necessary or the system fails.
        FileLine* fl = classp->fileline();
        AstCFunc* ctorp = new AstCFunc{classp->fileline(), "ctor_" + classp->name(),
                                       m_netlistp->topScopep()->scopep(), "void"};
        ctorp->isInline(false);
        auto className = EmitCBaseVisitor::prefixNameProtect(classp);
        for (AstNode* nodep = classp->stmtsp(); nodep; nodep = nodep->nextp()) {
            AstVar* varp = VN_CAST(nodep, Var);
            if (!varp) continue;
            UASSERT_OBJ(varp->isClassMember(), varp, "Expected class member");
            // create a tensor for this variable
            std::string tensorDeviceHandle = className + "." + varp->nameProtect();
            m_handles(varp).tensor
                = tensorDeviceHandle;  // need this to be able to later look up the tensor
            m_handles(varp).id = m_nextTensorId++;
            if (varp->bspFlag().hasHostRead()) {
                const std::string hrHandle
                    = varp->bspFlag().hasAnyHostReq() ? "interrupt" : ("hr." + tensorDeviceHandle);
                m_handles(varp).hostRead = hrHandle;
            }
            if (varp->bspFlag().hasHostWrite()) {
                const std::string hwHandle = "hw." + tensorDeviceHandle;
                m_handles(varp).hostWrite = hwHandle;
            }
        }

        return ctorp;
    }

    AstConst* mkConst(int n) const {
        UASSERT(n >= 0, "underflow");
        return new AstConst{m_netlistp->fileline(), AstConst::WidthedValue{}, 32,
                            static_cast<uint32_t>(n)};
    }

    AstConst* mkConst(uint32_t n) const {
        return new AstConst{m_netlistp->fileline(), AstConst::WidthedValue{}, 32, n};
    }

    void addNextCurrentPairs(AstCFunc* exchangep) {
        // This is unnecessary in OpenMP with shared memory.
    }

    AstClass* getClass(AstNode* nodep) {
        return VN_AS(VN_AS(nodep, MemberSel)->fromp()->dtypep(), ClassRefDType)->classp();
    }

    void addCopies(AstCFunc* cfuncp, const string& kind) {
        std::vector<AstNode*> nodesp;
        for (AstNode* nodep = cfuncp->stmtsp(); nodep;) {
            UASSERT(VN_IS(nodep, Assign), "expected AstAssign");
            AstAssign* const assignp = VN_AS(nodep, Assign);

            AstVar* const top = VN_AS(assignp->lhsp(), MemberSel)->varp();
            AstVar* const fromp = VN_AS(assignp->rhsp(), MemberSel)->varp();
            // get the handles from the user1

            auto getTileId = [](AstNodeExpr* np) {
                return VN_AS(VN_AS(np, MemberSel)->fromp()->dtypep(), ClassRefDType)
                    ->classp()
                    ->flag()
                    .tileId();
            };
            auto tileIdFrom = getTileId(assignp->rhsp());
            auto tileIdTo = getTileId(assignp->lhsp());
            const auto totalWords
                = top->dtypep()->skipRefp()->widthWords() * top->dtypep()->arrayUnpackedElements();
            // auto totalWords = VN_AS(top->dtypep(), VectorDType)->size();

            if (tileIdFrom == tileIdTo) {
                V3Stats::addStatSum(
                    string{"OpenMP, Total on-tile word copies "} + "(" + kind + ")", totalWords);
            } else {
                V3Stats::addStatSum(
                    string{"OpenMP, Total off-tile word copies "} + " (" + kind + ")", totalWords);
            }
            if (m_exchangeDump && kind == "exchange") {
                AstClass* const sourceClassp = getClass(assignp->rhsp());
                AstClass* const targetClassp = getClass(assignp->lhsp());

                *m_exchangeDump << sourceClassp->name() << " " << tileIdFrom << " "
                                << targetClassp->name() << " " << tileIdTo << " "
                                << totalWords * VL_BYTES_I(VL_IDATASIZE) << " " << fromp->name()
                                << " " << AstNode::dedotName(fromp->origName()) << std::endl;
            }

            auto toHandle = m_handles(top).tensor;
            UASSERT(!toHandle.empty(), "handle not set!");
            auto fromHandle = m_handles(fromp).tensor;
            UASSERT(!fromHandle.empty(), "handle not set!");
            // OpenMP does not need code generated. But metadata needs to be set.
            AstNode* const nextp = nodep->nextp();
            VL_DO_DANGLING(nodep->unlinkFrBack()->deleteTree(), nodep);
            nodep = nextp;
        }

        AstCFunc* splitFuncp = nullptr;
        const uint32_t maxFuncStmts = 4000;
        uint32_t funcSize = 0;
        for (AstNode* const nodep : nodesp) {
            if (!splitFuncp || (funcSize >= maxFuncStmts)) {
                funcSize = 0;
                splitFuncp = new AstCFunc{cfuncp->fileline(), m_newNames.get("cpsplit"),
                                          cfuncp->scopep(), "void"};
                splitFuncp->isInline(false);
                splitFuncp->isMethod(true);
                splitFuncp->dontCombine(true);
                cfuncp->scopep()->addBlocksp(splitFuncp);
                AstCCall* const callp = new AstCCall{cfuncp->fileline(), splitFuncp};
                callp->dtypeSetVoid();
                cfuncp->addStmtsp(callp->makeStmt());
            }
            funcSize++;
            splitFuncp->addStmtsp(nodep);
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

    // create a vertex that ORs all the needInteraction signals. Ideally this
    // vertex should be a multivertex for better performance, but I'll leave that
    // for later.

public:
    explicit OpenMPComputeGraphBuilder(AstNetlist* nodep)
        : m_newNames{"__VOpenMP"} {
        m_netlistp = nodep;

        // open up a file to dump the exchange information
        m_exchangeDump = std::unique_ptr<std::ofstream>(
            V3File::new_ofstream(v3Global.opt.makeDir() + "/" + "exchangeDump.txt"));
        if (m_exchangeDump) {
            *m_exchangeDump
                << "SourceVertex SourceTile TargetVertex TargetTile Bytes SourceVar Name"
                << std::endl;
        }
        // AstClass*
        initBuiltinTypes();

        // VlOpenMPContext is in ctx member variable
        m_ctxVarp = new AstVar{m_netlistp->fileline(), VVarType::MEMBER, "ctx", m_ctxTypep};
        m_netlistp->topModulep()->stmtsp()->addHereThisAsNext(m_ctxVarp);
        m_ctxVscp = new AstVarScope{m_netlistp->fileline(), m_netlistp->topScopep()->scopep(),
                                    m_ctxVarp};
        m_netlistp->topScopep()->scopep()->addVarsp(m_ctxVscp);

        // Number of processes is in NUM_CORES member variable (literal).
        m_numCoresVarp = new AstVar{m_netlistp->fileline(), VVarType::MEMBER, "NUM_CORES",
                                    nodep->findSigned32DType()};
        m_numCoresVarp->direction(VDirection::CONSTREF);
        m_netlistp->topModulep()->stmtsp()->addHereThisAsNext(m_numCoresVarp);
        m_numCoresVscp = new AstVarScope{m_netlistp->fileline(), m_netlistp->topScopep()->scopep(),
                                         m_numCoresVarp};
        m_netlistp->topScopep()->scopep()->addVarsp(m_numCoresVscp);

        // Step 1.
        // go through each class and create constructors. All that happens here
        // depends on a hard coded OpenMPContext that provides a few methods
        // for constructing graphs from codelets and connecting tensors to vertices
        AstNode::user1ClearTree();
        AstCFunc* constructAllp = new AstCFunc{m_netlistp->fileline(), "constructAll",
                                               m_netlistp->topScopep()->scopep(), "void"};
        constructAllp->isMethod(true);
        constructAllp->dontCombine(true);
        m_netlistp->topScopep()->scopep()->addBlocksp(constructAllp);

        m_netlistp->topModulep()->foreach([this, &constructAllp](AstVar* varp) {
            auto clsRefp = VN_CAST(varp->dtypep(), ClassRefDType);
            if (!clsRefp || !clsRefp->classp()->flag().isBsp()) return;
            // go through each deriviation of the base bsp class and create
            // host constructors
            AstClass* classp = clsRefp->classp();
            AstCFunc* ctorp = createVertexCons(classp, classp->flag().tileId());
            m_netlistp->topScopep()->scopep()->addBlocksp(ctorp);

            AstCCall* callp = new AstCCall{ctorp->fileline(), ctorp};
            callp->dtypeSetVoid();
            constructAllp->addStmtsp(new AstStmtExpr{ctorp->fileline(), callp});
        });

        // Do not create PlusArgsCopy function for OpenMP.

        // Step 2.
        // create an OpenMP program with the following structure:
        // Add the copy operations
        m_netlistp->foreach([this](AstCFunc* cfuncp) {
            if (cfuncp->name() == "exchange") { addNextCurrentPairs(cfuncp); }
            if (cfuncp->name() == "exchange" || cfuncp->name() == "initialize") {
                // create copy operations
                addCopies(cfuncp, cfuncp->name());
            }
        });
        // remove the function that are not used
        getFunc(m_netlistp->topModulep(), "computeSet")->unlinkFrBack()->deleteTree();
        getFunc(m_netlistp->topModulep(), "initComputeSet")->unlinkFrBack()->deleteTree();
    }
};

void V3BspOpenMPProgram::createProgram(AstNetlist* nodep) {
    // reoder passes only if you know what you are doing
    UINFO(3, "Creating OpenMP program" << endl);

    // Do not create PlusArgs functions for OpenMP with V3BspPlusArgs::makeCache

    // Do not create Dpi functions for OpenMP with V3BspDpi::delegateAll

    { OpenMPLegalizeFieldNamesVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("bspLegal", 0, dumpTree() >= 1);
    { OpenMPViewsVisitor{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspOpenMPView", 0, dumpTree() >= 1);
    { OpenMPComputeGraphBuilder{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspOpenMPProgram", 0, dumpTree() >= 1);
}
