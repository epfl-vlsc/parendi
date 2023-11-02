// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Lower the program tree create a poplar program
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

#include "V3BspPoplarProgram.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3BspDpi.h"
#include "V3BspModules.h"
#include "V3BspPlusArgs.h"
#include "V3EmitCBase.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

/// @brief  replaces AstVarRefs of members of the classes derived from the base
/// V3BspSched::V3BspModules::builtinBaseClass with AstVarRefView so that the code generation
/// can simply emit either a reinterpret_cast or a placement new. We do this because the poplar
/// classes could only have poplar::Vector<> as their members and these Vector are basically
/// opaque pointers for use. So we need to cast them to appropriate types.
class PoplarViewsVisitor final : public VNVisitor {
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

    void visit(AstVarRef* vrefp) override {

        if (!m_classp) { return; }
        if (!vrefp->varp()->user1()) { return; }
        if (vrefp->user1()) { return; /*processed*/ }
        vrefp->user1(true);
        // wrap the reference in an AstVarView, this is essential for poplar since
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
            // change all member dtypes to POPLAR_VECTOR_UINT32

            iterateChildren(nodep);
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit PoplarViewsVisitor(AstNetlist* nodep) {
        m_netlistp = nodep;
        iterate(nodep);
    }
};

class PoplarLegalizeFieldNamesVisitor final {
public:
    explicit PoplarLegalizeFieldNamesVisitor(AstNetlist* netlistp) {
        netlistp->foreach([](AstClass* classp) {
            if (classp->flag().isBsp()) {
                int nameId = 0;
                const VNUser1InUse user1Inuse;
                AstNode::user1ClearTree();
                for (AstNode* nodep = classp->stmtsp(); nodep; nodep = nodep->nextp()) {
                    if (AstVar* const varp = VN_CAST(nodep, Var)) { varp->user1(true); }
                }
                classp->foreach([&nameId](AstVarScope* vscp) {
                    if (vscp->varp()->user1()) {
                        // AstNode::dedotName()
                        // const std::string newName
                        //     = vscp->scopep()->nameDotless() + "__ARROW__" +
                        //     vscp->varp()->name();
                        if (vscp->varp()->origName().empty()) {
                            vscp->varp()->origName(vscp->varp()->name());
                        }
                        vscp->varp()->name("field_" + cvtToStr(nameId++));
                    }
                });
            }
        });
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

class PoplarComputeGraphBuilder final : public VNDeleter {
private:
    AstNetlist* m_netlistp = nullptr;
    V3UniqueNames m_newNames;
    AstBasicDType* m_ctxTypep = nullptr;
    AstBasicDType* m_tensorTypep = nullptr;
    AstBasicDType* m_vtxRefTypep = nullptr;
    AstVar* m_ctxVarp = nullptr;
    AstVarScope* m_ctxVscp = nullptr;
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
        m_ctxTypep = newType(VBasicDTypeKwd::POPLAR_CONTEXT);
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
        FileLine* fl = classp->fileline();
        AstCFunc* ctorp = new AstCFunc{classp->fileline(), "ctor_" + classp->name(),
                                       m_netlistp->topScopep()->scopep(), "void"};
        ctorp->isInline(false);
        AstVarScope* vtxVscp = createFuncVar(fl, ctorp, m_newNames.get("instance"), m_vtxRefTypep);
        auto className = EmitCBaseVisitor::prefixNameProtect(classp);
        AstAssign* mkVtx = new AstAssign{
            fl, new AstVarRef{fl, vtxVscp, VAccess::WRITE},
            mkCall(fl, "getOrAddVertex",
                   {new AstConst{fl, AstConst::String{}, className},
                    new AstConst{fl, AstConst::String{},
                                 classp->flag().isBspInit()
                                     ? "init"
                                     : classp->flag().isBspCond() ? "condeval" : "compute"}},
                   m_vtxRefTypep)};
        ctorp->addStmtsp(mkVtx);
        auto setTileMapping = [this, &fl, &ctorp](AstVarScope* vscp, uint32_t tid) {
            AstStmtExpr* tileMapp = new AstStmtExpr{
                fl, mkCall(fl, "setTileMapping",
                           {new AstVarRef{fl, vscp, VAccess::READWRITE}, mkConst(tid)})};
            ctorp->addStmtsp(tileMapp);
        };

        setTileMapping(vtxVscp, tileId);

        AstStmtExpr* perfEstp = new AstStmtExpr{
            fl, mkCall(fl, "setPerfEstimate",
                       {new AstVarRef{fl, vtxVscp, VAccess::READWRITE}, mkConst(0)})};
        ctorp->addStmtsp(perfEstp);
        // iterate through the class members and create tensors
        for (AstNode* nodep = classp->stmtsp(); nodep; nodep = nodep->nextp()) {
            AstVar* varp = VN_CAST(nodep, Var);
            if (!varp) continue;
            // AstVectorDType* dtp = VN_CAST(varp->dtypep(), VectorDType);
            // UASSERT_OBJ(dtp, varp, "expected VectorDType, need to create AstVarRefViews
            // first!"); UASSERT_OBJ(dtp->basicp()->keyword() == VBasicDTypeKwd::UINT32, dtp,
            // "expeced UINT32");
            UASSERT_OBJ(varp->isClassMember(), varp, "Expected class member");
            // create a tensor for this variable
            AstVarScope* tensorVscp
                = createFuncVar(fl, ctorp, m_newNames.get("tensor"), m_tensorTypep);
            std::string tensorDeviceHandle = className + "." + varp->nameProtect();
            m_handles(varp).tensor
                = tensorDeviceHandle;  // need this to be able to later look up the tensor
            m_handles(varp).id = m_nextTensorId++;
            const uint32_t vectorSize = getVectorSize(varp->dtypep());

            AstAssign* mkTensorp = new AstAssign{
                fl, new AstVarRef{fl, tensorVscp, VAccess::WRITE},
                mkCall(fl, "getOrAddTensor", {mkConst(vectorSize), mkConst(m_handles(varp).id)})};
            ctorp->addStmtsp(mkTensorp);
            setTileMapping(tensorVscp, tileId);
            // connect the tensor to the vertex
            ctorp->addStmtsp(new AstStmtExpr{
                fl, mkCall(fl, "connect",
                           {new AstVarRef{fl, vtxVscp, VAccess::READWRITE},
                            new AstConst{fl, AstConst::String{}, varp->nameProtect()},
                            new AstVarRef{fl, tensorVscp, VAccess::READWRITE}})});
            // check whether we need to create host read/write handles
            if (varp->bspFlag().hasHostRead()) {
                const std::string hrHandle
                    = varp->bspFlag().hasAnyHostReq() ? "interrupt" : ("hr." + tensorDeviceHandle);
                m_handles(varp).hostRead = hrHandle;
                ctorp->addStmtsp(
                    new AstStmtExpr{fl, mkCall(fl, "createHostRead",
                                               {new AstConst{fl, AstConst::String{}, hrHandle},
                                                new AstVarRef{fl, tensorVscp, VAccess::READWRITE},
                                                mkConst(vectorSize)},
                                               nullptr)});

                if (varp->bspFlag().hasAnyHostReq()) {
                    ctorp->addStmtsp(
                        new AstStmtExpr{fl, mkCall(fl, "isHostRequest",
                                                   {new AstVarRef{fl, tensorVscp, VAccess::READ},
                                                    new AstConst{fl, AstConst::BitTrue{},
                                                                 varp->bspFlag().hasHostReq()}},
                                                   nullptr)});
                }
            }
            if (varp->bspFlag().hasHostWrite()) {
                const std::string hwHandle = "hw." + tensorDeviceHandle;
                m_handles(varp).hostWrite = hwHandle;
                ctorp->addStmtsp(
                    new AstStmtExpr{fl, mkCall(fl, "createHostWrite",
                                               {new AstConst{fl, AstConst::String{}, hwHandle},
                                                new AstVarRef{fl, tensorVscp, VAccess::READWRITE},
                                                mkConst(vectorSize)},
                                               nullptr)});
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

        std::vector<AstNode*> stmtsp;
        for (AstNode* nodep = exchangep->stmtsp(); nodep;) {
            UASSERT(VN_IS(nodep, Assign), "expected AstAssign");
            AstAssign* const assignp = VN_AS(nodep, Assign);
            AstVar* const top = VN_AS(assignp->lhsp(), MemberSel)->varp();
            AstVar* const fromp = VN_AS(assignp->rhsp(), MemberSel)->varp();
            const string nextHandle = m_handles(fromp).tensor;
            UASSERT(!nextHandle.empty(), "handle not set!");
            const string currentHandle = m_handles(top).tensor;
            UASSERT(!currentHandle.empty(), "handle not set!");
            const auto totalWords
                = top->dtypep()->skipRefp()->widthWords() * top->dtypep()->arrayUnpackedElements();

            stmtsp.push_back(new AstComment{nodep->fileline(),
                                            "next: " + nextHandle + " current: " + currentHandle});
            AstNode* newp = new AstStmtExpr{nodep->fileline(),
                                            mkCall(assignp->fileline(), "addNextCurrentPair",
                                                   {mkConst(m_handles(fromp).id) /*source*/,
                                                    mkConst(m_handles(top).id) /*target*/,
                                                    mkConst(totalWords) /*number of words*/})};
            AstNode* const nextp = nodep->nextp();
            stmtsp.push_back(newp);
            // newp->addHereThisAsNext(newCommentp);
            nodep = nextp;
        }
        AstCFunc* splitFuncp = nullptr;
        const uint32_t maxFuncStmts = static_cast<uint32_t>(v3Global.opt.outputSplit());
        uint32_t funcSize = 0;
        AstCFunc* const cfuncp = new AstCFunc{exchangep->fileline(), "constructStatePairs",
                                              exchangep->scopep(), "void"};
        exchangep->scopep()->addBlocksp(cfuncp);
        cfuncp->isInline(false);
        cfuncp->isMethod(true);
        cfuncp->dontCombine(true);
        for (AstNode* const nodep : stmtsp) {
            if (!splitFuncp || (funcSize >= maxFuncStmts)) {
                funcSize = 0;
                splitFuncp = new AstCFunc{cfuncp->fileline(), m_newNames.get("statepairsplit"),
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
                    string{"Poplar, Total on-tile word copies "} + "(" + kind + ")", totalWords);
            } else {
                V3Stats::addStatSum(
                    string{"Poplar, Total off-tile word copies "} + " (" + kind + ")", totalWords);
            }
            if (m_exchangeDump && kind == "exchange") {

                AstClass* const sourceClassp = getClass(assignp->rhsp());
                AstClass* const targetClassp = getClass(assignp->lhsp());

                *m_exchangeDump << sourceClassp->name() << " " << tileIdFrom << " "
                                << targetClassp->name() << " " << tileIdTo << " "
                                << totalWords * VL_BYTES_I(VL_IDATASIZE)  << " "
                                << fromp->name() << " "
                                << AstNode::dedotName(fromp->origName()) << std::endl;
            }

            auto toHandle = m_handles(top).tensor;
            UASSERT(!toHandle.empty(), "handle not set!");
            auto fromHandle = m_handles(fromp).tensor;
            UASSERT(!fromHandle.empty(), "handle not set!");
            // AstComment* newCommentp
            //     = new AstComment{assignp->fileline(), cvtToStr(totalWords) + " words"};

            nodesp.push_back(
                new AstComment{nodep->fileline(), "Copy " + fromHandle + " -> " + toHandle});
            AstNode* newp = new AstStmtExpr{
                nodep->fileline(), mkCall(assignp->fileline(), "addCopy",
                                          {mkConst(m_handles(fromp).id) /*source*/,
                                           mkConst(m_handles(top).id) /*target*/,
                                           mkConst(totalWords) /*number of words*/,
                                           new AstConst{nodep->fileline(), AstConst::String{},
                                                        kind} /*is it part of init*/})};
            AstNode* const nextp = nodep->nextp();
            nodesp.push_back(newp);
            // newp->addHereThisAsNext(newCommentp);
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
    void patchHostHandle() {

        // find any other reachable function from hostp;
        std::set<AstCFunc*> reachablep;
        std::queue<AstCFunc*> toVisitp;
        toVisitp.push(getFunc(m_netlistp->topModulep(), "hostHandle"));
        int depth = 0;
        while (!toVisitp.empty()) {
            UASSERT(depth++ < 100000, "something is up");
            AstCFunc* toCheckp = toVisitp.front();
            toVisitp.pop();
            if (reachablep.find(toCheckp) != reachablep.end()) continue;
            toCheckp->foreach([&toVisitp, &reachablep](AstCCall* callp) {
                auto foundIt = reachablep.find(callp->funcp());
                if (foundIt == reachablep.end()) { toVisitp.push(callp->funcp()); }
            });
            reachablep.insert(toCheckp);
        }

        for (AstCFunc* nodep : reachablep) { patchHostFuncCall(nodep); }
    }

    void patchHostFuncCall(AstCFunc* cfuncp);

    // create a vertex that ORs all the needInteraction signals. Ideally this
    // vertex should be a multivertex for better performance, but I'll leave that
    // for later.

public:
    explicit PoplarComputeGraphBuilder(AstNetlist* nodep)
        : m_newNames{"__VPoplar"} {
        m_netlistp = nodep;

        // open up a file to dump the exchange information
        m_exchangeDump = std::unique_ptr<std::ofstream>(
            V3File::new_ofstream(v3Global.opt.makeDir() + "/" + "exchangeDump.txt"));
        if (m_exchangeDump) {
            *m_exchangeDump << "SourceVertex SourceTile TargetVertex TargetTile Bytes SourceVar Name"
                            << std::endl;
        }
        // AstClass*
        initBuiltinTypes();

        m_ctxVarp = new AstVar{m_netlistp->fileline(), VVarType::VAR, "ctx", m_ctxTypep};
        m_netlistp->topModulep()->stmtsp()->addHereThisAsNext(m_ctxVarp);
        m_ctxVscp = new AstVarScope{m_netlistp->fileline(), m_netlistp->topScopep()->scopep(),
                                    m_ctxVarp};
        m_netlistp->topScopep()->scopep()->addVarsp(m_ctxVscp);

        // Step 1.
        // go through each class and create constructors. All that happens here
        // depends on a hard coded PoplarContext that provides a few methods
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
        // add the plusArgs function right after construction, this will set a bunch
        // of host variables later needed to be copied to the user vertcies
        // copy the cache values of args to the tensors on startup
        AstCFunc* plusArgsCopyp = getFunc(m_netlistp->topModulep(), "plusArgsCopy");
        addInitConstCopies(plusArgsCopyp);
        // AstCCall* plusArgsCopyCallp = new AstCCall{plusArgsCopyp->fileline(), plusArgsCopyp};
        // plusArgsCopyCallp->dtypeSetVoid();
        // constructAllp->addStmtsp(
        //     new AstStmtExpr{plusArgsCopyCallp->fileline(), plusArgsCopyCallp});
        // Step 2.
        // create a poplar program with the following structure:
        // Add the copy operations
        m_netlistp->foreach([this](AstCFunc* cfuncp) {
            if (cfuncp->name() == "exchange") { addNextCurrentPairs(cfuncp); }
            if (cfuncp->name() == "exchange" || cfuncp->name() == "initialize"
                || cfuncp->name() == "dpiExchange" || cfuncp->name() == "dpiBroadcast") {
                // create copy operations
                addCopies(cfuncp, cfuncp->name());
            }
        });
        patchHostHandle();
        // remove the computeSet funciton, not used
        getFunc(m_netlistp->topModulep(), "computeSet")->unlinkFrBack()->deleteTree();
        getFunc(m_netlistp->topModulep(), "initComputeSet")->unlinkFrBack()->deleteTree();
    }

    friend class PoplarHostHandleHardenVisitor;
};

/// @brief Goes through a function and hardens all the MemberSel references
/// This is a special visitor that used by the PoplarComputeGraphBuilder.
/// It replaces all the MemberSel references with hardened "getHostData" and
/// "setHostData" calls to the poplar context:
///     if (vtx1.dpiPoint == C) dpi_call(vtx1.rv1, vtx1.rv2, vtx1.lv/*by ref*/);
/// becomes:
///     tmp0 = ctx.getHostData("vtx1.dpiPoint");
///     if (tmp0 == C) {
///         tmp1 = ctx.getHostData("vtx1.rv1");
///         tmp2 = ctx.getHostData("vtx1.rv2");
///         tmp3;
///         dpi_call(tmp1, tmp2, tmp3);
///         ctx.setHostData(tmp3);
///     }
class PoplarHostHandleHardenVisitor : public VNVisitor {
    friend class PoplarComputeGraphBuilder;

private:
    AstCFunc* m_cfuncp = nullptr;  // enclosing function
    AstNodeStmt* m_stmtp = nullptr;  // enclosing statement
    PoplarComputeGraphBuilder& m_parent;  // the parent class that uses this visitor

    void visit(AstMemberSel* memselp) {
        UASSERT_OBJ(m_stmtp, memselp, "expected to be in a statement");
        UASSERT_OBJ(VN_IS(memselp->fromp(), VarRef), memselp,
                    "Expected simple VarRef but got \"" << memselp->fromp()->prettyTypeName()
                                                        << "\"" << endl);
        AstVarRef* const vrefp = VN_AS(memselp->fromp(), VarRef);

        // create a local variable
        AstVar* const varp = new AstVar{
            vrefp->fileline(), VVarType::BLOCKTEMP, m_parent.m_newNames.get("tmphost"),
            memselp->dtypep() /* do not get it from vrefp because it is of VectorDType*/};
        AstVarScope* const vscp = new AstVarScope{varp->fileline(), m_cfuncp->scopep(), varp};
        m_cfuncp->scopep()->addVarsp(vscp);
        m_stmtp->addHereThisAsNext(varp);
        {
            const auto handle = m_parent.m_handles(memselp->varp()).hostRead;
            UASSERT_OBJ(!handle.empty(), vrefp, "empty read handle");
            // MEMBERSEL cls.var becomes ctx.getHostData<dtype>(var, dtype{})
            // memselp->dtypep() still has the "old" host side datatype
            // not the vector type that PoplarViewsVisitor creates in BSP
            // classes. Consider it a bug (since type information is broken) or a feature
            // (since things become easier here!)
            AstCMethodHard* const hostDatap = m_parent.mkCall(
                memselp->fileline(),
                "getHostData<" + memselp->dtypep()->cType("", false, false) + ">",
                {new AstConst{memselp->fileline(), AstConst::String{}, handle}},
                memselp->dtypep());

            m_stmtp->addHereThisAsNext(
                new AstAssign{vrefp->fileline(),
                              new AstVarRef{vrefp->fileline(), vscp, VAccess::WRITE}, hostDatap});
        }
        if (vrefp->access().isWriteOrRW()) {
            const auto handle = m_parent.m_handles(memselp->varp()).hostWrite;
            UASSERT_OBJ(!handle.empty(), vrefp, "empty write handle");
            AstCMethodHard* const hostSetp = m_parent.mkCall(
                memselp->fileline(), "setHostData",
                {
                    new AstConst{memselp->fileline(), AstConst::String{}, handle},
                    new AstVarRef{vrefp->fileline(), vscp, VAccess::READ},
                });
            m_stmtp->addNextHere(hostSetp->makeStmt());
            // postp.push_back(hostSetp->makeStmt());
        }
        memselp->replaceWith(new AstVarRef{memselp->fileline(), vscp, vrefp->access()});
        VL_DO_DANGLING(pushDeletep(memselp), memselp);
    }
    void visit(AstNodeStmt* nodep) override {
        m_stmtp = nodep;
        iterateChildren(nodep);
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

private:
    explicit PoplarHostHandleHardenVisitor(AstCFunc* cfuncp, PoplarComputeGraphBuilder& parent)
        : m_parent(parent) {
        m_cfuncp = cfuncp;
        iterate(cfuncp);
    }

public:
    friend class PoplarComputeGraphBuilder;
};

void PoplarComputeGraphBuilder::patchHostFuncCall(AstCFunc* cfuncp) {
    // go through all the statements within this function and replace MemberSel
    // nodes:
    // CFunc:
    //    ...
    //    Stmt(MemberSel LV, MemberSel RV) ==>
    // becomes:
    // CFunc:
    //    ...
    //    var arv = getHostData(...)
    //    var blv = getHostData(...)
    //    Stmt(blv, arv)
    //    setHostData(blv)
    PoplarHostHandleHardenVisitor{cfuncp, *this};
}

void V3BspPoplarProgram::createProgram(AstNetlist* nodep) {
    // reoder passes only if you know what you are doing
    UINFO(3, "Creating poplar program" << endl);

    V3BspPlusArgs::makeCache(nodep);

    // delegate all dpi calls to the host
    V3BspDpi::delegateAll(nodep);

    { PoplarLegalizeFieldNamesVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("bspLegal", 0, dumpTree() >= 1);
    { PoplarViewsVisitor{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspPoplarView", 0, dumpTree() >= 1);
    { PoplarComputeGraphBuilder{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspPoplarProgram", 0, dumpTree() >= 1);
}
