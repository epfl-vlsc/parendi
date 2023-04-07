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
#include "V3BspModules.h"
#include "V3EmitCBase.h"
#include "V3Global.h"
#include "V3UniqueNames.h"

#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

/// @brief create class member interfaces for host interactions.
/// This visitor looks for Display, Finish, and Stop nodes in the bsp classes
/// and replaces them with writes to class members. Then a hostHandle function
/// is created in the top module that handles the the system call in the host
/// Note that general DPI calls with return values is not supported.
class PoplarHostInteractionVisitor final : public VNVisitor {
private:
    AstNetlist* m_netlistp = nullptr;

    V3UniqueNames m_funcNames;
    V3UniqueNames m_memberName;
    struct ClassInfo {
        AstClass* m_classp = nullptr;
        AstVarScope* m_instp = nullptr;
        AstVarScope* m_classInteractionp = nullptr;
        AstVarScope* m_classCallId = nullptr;
        AstScope* m_scopep = nullptr;
        AstCFunc* m_hostHandlep = nullptr;
        int m_callId = 0;
        int m_numCalls = 0;
        void clear() {
            m_classp = nullptr;
            m_instp = nullptr;
            m_classInteractionp = nullptr;
            m_classCallId = nullptr;
            m_scopep = nullptr;
            m_numCalls = 0;
            m_callId = 0;
            m_hostHandlep = nullptr;
        }
    };
    std::vector<ClassInfo> m_info;
    ClassInfo m_ctx;

    AstVarScope* getInteractionTrigger() {
        UASSERT(m_ctx.m_classp, "not in class");
        UASSERT(m_ctx.m_scopep, "no scope!");
        if (!m_ctx.m_classInteractionp) {
            // create a new member in the class to which basically is
            // set whenever a host interaction is required
            AstVar* varp
                = new AstVar{m_ctx.m_classp->fileline(), VVarType::MEMBER,
                             m_memberName.get("needInteraction"), m_netlistp->findUInt32DType()};
            varp->bspFlag().append(VBspFlag::MEMBER_HOSTREAD).append(VBspFlag::MEMBER_OUTPUT);
            AstVarScope* vscp = new AstVarScope{varp->fileline(), m_ctx.m_scopep, varp};
            UASSERT(m_ctx.m_classp->stmtsp(), "class with no vars!");
            m_ctx.m_classp->stmtsp()->addHereThisAsNext(varp);
            m_ctx.m_scopep->addVarsp(vscp);
            m_ctx.m_classInteractionp = vscp;
        }
        return m_ctx.m_classInteractionp;
    }
    AstVarScope* getInteractionId() {
        UASSERT(m_ctx.m_classp, "not in class!");
        UASSERT(m_ctx.m_scopep, "no scope!");
        if (!m_ctx.m_classCallId) {
            UASSERT(m_ctx.m_numCalls > 0, "should not create host interface!");
            AstVar* varp = new AstVar{
                m_ctx.m_classp->fileline(), VVarType::MEMBER, m_memberName.get("callId"),
                m_netlistp->findBitDType(m_ctx.m_numCalls, std::max(m_ctx.m_numCalls, VL_EDATASIZE), VSigning::UNSIGNED)};
            varp->bspFlag().append(VBspFlag::MEMBER_HOSTREAD).append(VBspFlag::MEMBER_OUTPUT);
            AstVarScope* vscp = new AstVarScope{varp->fileline(), m_ctx.m_scopep, varp};
            UASSERT(m_ctx.m_classp->stmtsp(), "Class with no vars!");
            m_ctx.m_classp->stmtsp()->addHereThisAsNext(varp);
            m_ctx.m_scopep->addVarsp(vscp);
            m_ctx.m_classCallId = vscp;
        }
        return m_ctx.m_classCallId;
    }
    void makeHostHandle(AstNode* nodep) {
        UINFO(3, "Replacing " << nodep->shortName() << " with triggers" << endl);
        AstVarScope* triggerp = getInteractionTrigger();
        AstVarScope* callIdp = getInteractionId();

        FileLine* fl = nodep->fileline();
        AstAssign* setTriggerp = new AstAssign{fl, new AstVarRef{fl, triggerp, VAccess::WRITE},
                                               new AstConst{fl, AstConst::Unsized32{}, 1u}};
        AstAssign* setIdp = new AstAssign{
            fl, new AstSel{fl, new AstVarRef{fl, callIdp, VAccess::WRITE}, m_ctx.m_callId, 1},
            new AstConst{fl, AstConst::WidthedValue{}, 1, 1}};
        setTriggerp->addNext(setIdp);
        AstNode* oldp = nodep;
        nodep->replaceWith(setTriggerp);
        // create a function directly under the top module for handling this
        if (!m_ctx.m_hostHandlep) {
            m_ctx.m_hostHandlep = new AstCFunc{nodep->fileline(), m_funcNames.get("display"),
                                               m_netlistp->topScopep()->scopep(), "void"};
        }
        // create IF(classInst.memberCallId == (1 << callid)) DISPLAY under the host handle
        AstMemberSel* callSelp = new AstMemberSel{
            oldp->fileline(), new AstVarRef{oldp->fileline(), m_ctx.m_instp, VAccess::READ},
            VFlagChildDType{}, m_ctx.m_classCallId->name()};
        callSelp->varp(m_ctx.m_classCallId->varp());
        callSelp->dtypep(m_ctx.m_classCallId->dtypep());
        AstSel* selp = new AstSel{oldp->fileline(), callSelp, m_ctx.m_callId, 1};
        AstIf* hostIfp
            = new AstIf{oldp->fileline(),
                        new AstEq{oldp->fileline(), selp,
                                  new AstConst{oldp->fileline(), AstConst::WidthedValue{}, 1, 1}},
                        oldp, nullptr};
        m_ctx.m_hostHandlep->addStmtsp(hostIfp);
        m_ctx.m_callId++;
    }
    // VISITORS
    void visit(AstDisplay* nodep) override {
        // For every argument, create a class member. Alternatively, we could create
        // a single member for all the host functions within this class but we leave that
        // as an optimization, since it requires low-level indexing and casting.
        AstSFormatF* fmtp = nodep->fmtp();
        UASSERT(!fmtp->scopeNamep(), "did not expect op2 on AstFormatF " << fmtp << endl);
        std::vector<AstNodeExpr*> exprsp;
        for (AstNodeExpr* exprp = fmtp->exprsp(); exprp; exprp = VN_AS(exprp->nextp(), NodeExpr)) {
            exprsp.push_back(exprp);
        }
        std::vector<AstVar*> membersp;
        std::vector<AstAssign*> assignsp;
        for (AstNodeExpr* exprp : exprsp) {
            // create a class member
            AstVar* memberp = new AstVar{exprp->fileline(), VVarType::MEMBER,
                                         m_memberName.get("fmtArg"), exprp->dtypep()};
            membersp.push_back(memberp);
            // mark the variable as something that can be read by the host
            memberp->bspFlag().append(VBspFlag::MEMBER_HOSTREAD).append(VBspFlag::MEMBER_OUTPUT);
            UASSERT(m_ctx.m_classp->stmtsp(), "Expected at least one stmt!");
            m_ctx.m_classp->stmtsp()->addHereThisAsNext(memberp);
            // need a variable scope as well
            AstVarScope* memVscp = new AstVarScope{exprp->fileline(), m_ctx.m_scopep, memberp};
            m_ctx.m_scopep->addVarsp(memVscp);
            // create an assignment member = exprp
            AstNodeExpr* unlinkp = exprp->unlinkFrBack();
            AstAssign* assignp = new AstAssign{
                exprp->fileline(), new AstVarRef{exprp->fileline(), memVscp, VAccess::WRITE},
                unlinkp};
            assignsp.push_back(assignp);
        }

        // replace the fmtp with a new one that directly references the members
        // of the current class responsible for the display
        for (AstVar* memberp : membersp) {
            AstMemberSel* exprp = new AstMemberSel{
                memberp->fileline(),
                new AstVarRef{memberp->fileline(), m_ctx.m_instp, VAccess::READ},
                VFlagChildDType{}, memberp->name()};
            exprp->dtypep(memberp->dtypep());
            exprp->varp(memberp);
            fmtp->addExprsp(exprp);
        }
        // now create a host handle
        makeHostHandle(nodep);
    }
    void visit(AstFinish* nodep) { makeHostHandle(nodep); }
    void visit(AstStop* nodep) { makeHostHandle(nodep); }

    void visit(AstScope* nodep) override {
        if (m_ctx.m_classp) {
            UASSERT(!m_ctx.m_scopep, "should not nest scopes!");
            m_ctx.m_scopep = nodep;
            iterateChildren(nodep);
        }
    }

    void visit(AstClass* classp) override {}
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit PoplarHostInteractionVisitor(AstNetlist* nodep)
        : m_funcNames{"__VbspHost"}
        , m_memberName{"__VbspHostInterface"} {
        UASSERT(v3Global.assertScoped(), "expected scopes, run before descoping");
        m_netlistp = nodep;

        // m_netlistp->topScopep()->scopep(), "void"

        AstCFunc* hostHandlep = new AstCFunc{m_netlistp->fileline(), "hostHandle",
                                             m_netlistp->topScopep()->scopep(), "void"};
        hostHandlep->dontCombine(true);
        // non-standard traversal, walk through the top level vars, if var is
        // of base class type, jump to class definition
        nodep->topModulep()->foreach([this, &hostHandlep](AstVarScope* vscp) {
            AstClassRefDType* clsRefp = VN_CAST(vscp->varp()->dtypep(), ClassRefDType);
            if (clsRefp->classp()->flag().isBsp()) {
                // jump to the class
                m_ctx.clear();
                m_memberName.reset();
                m_ctx.m_classp = clsRefp->classp();
                m_ctx.m_instp = vscp;
                m_ctx.m_classp->foreach([this](AstNodeStmt* stmtp) {
                    // count the number of calls to $stop, $finish, and $display
                    if (VN_IS(stmtp, Display) || VN_IS(stmtp, Finish) || VN_IS(stmtp, Stop)) {
                        m_ctx.m_numCalls += 1;
                    }
                });
                if (m_ctx.m_numCalls > 0) {
                    iterateChildren(m_ctx.m_classp);
                    // reset trigger and callId
                    AstAssign* trigResetp
                        = new AstAssign{m_ctx.m_classInteractionp->fileline(),
                                        new AstVarRef{m_ctx.m_classInteractionp->fileline(),
                                                      m_ctx.m_classInteractionp, VAccess::WRITE},
                                        new AstConst{m_ctx.m_classInteractionp->fileline(),
                                                     AstConst::Unsized32{}, 0}};
                    AstAssign* callIdResetp = new AstAssign{
                        m_ctx.m_classCallId->fileline(),
                        new AstVarRef{m_ctx.m_classCallId->fileline(), m_ctx.m_classCallId,
                                      VAccess::WRITE},
                        new AstConst{m_ctx.m_classCallId->fileline(), AstConst::WidthedValue{},
                                     m_ctx.m_classCallId->width(), 0}};
                    AstCFunc* computep = nullptr;
                    m_ctx.m_classp->foreach([&computep](AstCFunc* funcp) {
                        if (funcp->name() == "compute") { computep = funcp; }
                    });
                    UASSERT(computep, "Could not fin compute method!");
                    // reset the host interfaces before starting computation
                    // we could potentially guard them as well with an unlikely
                    // condition on the trigger, but whatever...
                    computep->stmtsp()->addHereThisAsNext(callIdResetp);
                    computep->stmtsp()->addHereThisAsNext(trigResetp);
                    m_netlistp->topScopep()->scopep()->addBlocksp(m_ctx.m_hostHandlep);
                    AstMemberSel* selTrigp = new AstMemberSel{
                        m_ctx.m_classp->fileline(),
                        new AstVarRef{m_ctx.m_instp->fileline(), m_ctx.m_instp, VAccess::READ},
                        VFlagChildDType{}, m_ctx.m_classInteractionp->name()};
                    selTrigp->dtypep(m_ctx.m_classInteractionp->dtypep());
                    selTrigp->varp(m_ctx.m_classInteractionp->varp());
                    AstCCall* callHandlep
                        = new AstCCall{selTrigp->fileline(), m_ctx.m_hostHandlep, nullptr};
                    callHandlep->dtypeSetVoid();
                    AstIf* thisHandlep = new AstIf{
                        m_ctx.m_classp->fileline(),
                        new AstEq{m_ctx.m_classp->fileline(), selTrigp,
                                  new AstConst{selTrigp->fileline(), AstConst::Unsized32{}, 1}},
                        callHandlep};
                    hostHandlep->addStmtsp(thisHandlep);
                }
            }
        });
        // create a function that handles are host interactions
        m_netlistp->topScopep()->scopep()->addBlocksp(hostHandlep);
    }
};
/// @brief  replaces AstVarRefs of members of the classes derived from the base
/// V3BspSched::V3BspModules::builtinBaseClass with AstVarRefView so that the code generation can
/// simply emit either a reinterpret_cast or a placement new.
/// We do this because the poplar classes could only have poplar::Vector<> as their
/// members and these Vector are basically opaque pointers for use. So we need to
/// cast them to appropriate types.
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
            auto dims = arrayp->dimensions(false /*do not include the basic types*/);
            UASSERT(dims.first == 0, "Not sure if I can do unpack arrays yet! " << dtp << endl);
            return dims.second * calcSize(arrayp->basicp());
        } else {
            UASSERT_OBJ(false, dtp, "Can not handle data type " << dtp << endl);
            return 0;
        }
    }
    // VISITORS
    AstVectorDType* vectorTypep(AstNodeDType* fromp) {

        uint32_t size = calcSize(fromp);
        AstVectorDType* dtp
            = new AstVectorDType(fromp->fileline(), size,
                                 VN_AS(m_netlistp->typeTablep()->findUInt32DType(), BasicDType));
        m_netlistp->typeTablep()->addTypesp(dtp);
        return dtp;
    }
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
                    members.push_back(varp);
                    varp->dtypep(vectorTypep(varp->dtypep()));
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

class PoplarComputeGraphBuilder final : public VNDeleter {
private:
    AstNetlist* m_netlistp = nullptr;
    V3UniqueNames m_newNames;
    AstBasicDType* m_ctxTypep = nullptr;
    AstBasicDType* m_tensorTypep = nullptr;
    AstBasicDType* m_vtxRefTypep = nullptr;
    AstVar* m_ctxVarp = nullptr;
    AstVarScope* m_ctxVscp = nullptr;

    uint32_t m_callerId = 0;
    struct TensorHandle {
        std::string tensor;
        std::string hostRead;
        std::string hostWrite;
        TensorHandle() {
            tensor.erase();
            hostRead.erase();
            hostWrite.erase();
        }
    };
    VNUser1InUse m_user1InUse;
    AstUser1Allocator<AstVar, TensorHandle> m_handles;

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
                    new AstConst{fl, AstConst::BitTrue{}, classp->flag().isBspInit()}},
                   m_vtxRefTypep)};
        ctorp->addStmtsp(mkVtx);
        auto setTileMapping = [this, &fl, &ctorp](AstVarScope* vscp, uint32_t tid) {
            AstStmtExpr* tileMapp
                = new AstStmtExpr{fl, mkCall(fl, "setTileMapping",
                                             {new AstVarRef{fl, vscp, VAccess::READWRITE},
                                              new AstConst{fl, AstConst::Unsized32{}, tid}})};
            ctorp->addStmtsp(tileMapp);
        };

        setTileMapping(vtxVscp, tileId);

        AstStmtExpr* perfEstp
            = new AstStmtExpr{fl, mkCall(fl, "setPerfEstimate",
                                         {new AstVarRef{fl, vtxVscp, VAccess::READWRITE},
                                          new AstConst{fl, AstConst::Unsized32{}, 0}})};
        ctorp->addStmtsp(perfEstp);
        // iterate through the class members and create tensors
        for (AstNode* nodep = classp->stmtsp(); nodep; nodep = nodep->nextp()) {
            AstVar* varp = VN_CAST(nodep, Var);
            if (!varp) continue;
            AstVectorDType* dtp = VN_CAST(varp->dtypep(), VectorDType);
            UASSERT_OBJ(dtp, varp, "expected VectorDType, need to create AstVarRefViews first!");
            UASSERT_OBJ(dtp->basicp()->keyword() == VBasicDTypeKwd::UINT32, dtp, "expeced UINT32");
            UASSERT_OBJ(varp->isClassMember(), varp, "Expected class member");
            // create a tensor for this variable
            AstVarScope* tensorVscp
                = createFuncVar(fl, ctorp, m_newNames.get("tensor"), m_tensorTypep);
            std::string tensorDeviceHandle = className + "." + varp->nameProtect();
            m_handles(varp).tensor
                = tensorDeviceHandle;  // need this to be able to later look up the tensor
            AstAssign* mkTensorp = new AstAssign{
                fl, new AstVarRef{fl, tensorVscp, VAccess::WRITE},
                mkCall(fl, "addTensor",
                       {new AstConst{fl, AstConst::Unsized32{}, dtp->size()},
                        new AstConst{fl, AstConst::String{}, tensorDeviceHandle}})};
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
                const std::string hrHandle = "hr." + tensorDeviceHandle;
                m_handles(varp).hostRead = hrHandle;
                ctorp->addStmtsp(
                    new AstStmtExpr{fl, mkCall(fl, "createHostRead",
                                               {new AstConst{fl, AstConst::String{}, hrHandle},
                                                new AstVarRef{fl, tensorVscp, VAccess::READWRITE}},
                                               nullptr)});
            }
            if (varp->bspFlag().hasHostWrite()) {
                const std::string hwHandle = "hw." + tensorDeviceHandle;
                m_handles(varp).hostWrite = hwHandle;
                ctorp->addStmtsp(
                    new AstStmtExpr{fl, mkCall(fl, "createHostWrite",
                                               {new AstConst{fl, AstConst::String{}, hwHandle},
                                                new AstVarRef{fl, tensorVscp, VAccess::READWRITE}},
                                               nullptr)});
            }
        }

        return ctorp;
    }

    void addCopies(AstCFunc* cfuncp, const bool isInit) {

        for (AstNode* nodep = cfuncp->stmtsp(); nodep;) {
            UASSERT(VN_IS(nodep, Assign), "expected AstAssign");
            AstAssign* const assignp = VN_AS(nodep, Assign);
            AstVar* const top = VN_AS(assignp->lhsp(), MemberSel)->varp();
            AstVar* const fromp = VN_AS(assignp->rhsp(), MemberSel)->varp();
            // get the handles from the user1
            auto toHandle = m_handles(top).tensor;
            UASSERT(!toHandle.empty(), "handle not set!");
            auto fromHandle = m_handles(fromp).tensor;
            UASSERT(!fromHandle.empty(), "handle not set!");
            AstNode* newp = new AstStmtExpr{
                nodep->fileline(),
                mkCall(assignp->fileline(), "addCopy",
                       {new AstConst{nodep->fileline(), AstConst::String{}, fromHandle} /*source*/,
                        new AstConst{nodep->fileline(), AstConst::String{}, toHandle} /*target*/,
                        new AstConst{nodep->fileline(), AstConst::Unsized32{},
                                     static_cast<uint32_t>(top->widthWords())} /*number of words*/,
                        new AstConst{nodep->fileline(), AstConst::BitTrue{},
                                     isInit} /*is it part of init*/})};
            nodep->replaceWith(newp);
            VL_DO_DANGLING(nodep->deleteTree(), nodep);
            nodep = newp->nextp();
        }
    }

    void patchHostHandle() {
        // find the host handle top function
        AstCFunc* hostp = getFunc(m_netlistp->topModulep(), "hostHandle");
        // find any other reachable function from hostp;
        std::set<AstCFunc*> reachablep;
        std::queue<AstCFunc*> toVisitp;
        toVisitp.push(hostp);
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

        for (AstCFunc* nodep : reachablep) {
            // replace all the AstMemberSel with calls to appropriate members
            // to PoplarContext
            nodep->foreach([this](AstMemberSel* memselp) {
                // MEMBERSEL cls.var becomes ctx.getHostData<dtype>(var, dtype{})
                // memselp->dtypep() still has the "old" host side datatype
                // not the vector type that PoplarViewsVisitor creates in BSP
                // classes. Consider it a bug (since type information is broken) or a feature
                // (since things become easier here!)
                auto handle = m_handles(memselp->varp()).hostRead;
                UASSERT(!handle.empty(), "empty handle!");
                AstCMethodHard* callp
                    = mkCall(memselp->fileline(),
                             "getHostData", /*getHostData is a templated function, we use a empty
                                               constant to help the C++ compiler infer the type*/
                             {new AstConst{memselp->fileline(), AstConst::String{}, handle},
                              new AstConst{memselp->fileline(), AstConst::DTyped{},
                                           memselp->dtypep()} /*used to resolve template types*/},
                             memselp->dtypep());
                memselp->replaceWith(callp);
                VL_DO_DANGLING(pushDeletep(memselp), memselp);
            });
        }
    }

public:
    explicit PoplarComputeGraphBuilder(AstNetlist* nodep)
        : m_newNames{"__VPoplar"} {
        m_netlistp = nodep;
        // AstClass*
        initBuiltinTypes();

        m_ctxVarp = new AstVar{m_netlistp->fileline(), VVarType::VAR, "ctx", m_ctxTypep};
        m_netlistp->topModulep()->stmtsp()->addHereThisAsNext(m_ctxVarp);
        m_ctxVscp = new AstVarScope{m_netlistp->fileline(), m_netlistp->topScopep()->scopep(),
                                    m_ctxVarp};
        m_netlistp->topScopep()->scopep()->addVarsp(m_ctxVscp);

        uint32_t tileId = 0;
        const uint32_t maxTileId = 1472;  // should be a cli arg
        // Step 1.
        // go through each class and create constructors. All that happens here
        // depends on a hard coded PoplarContext that provides a few methods
        // for constructing graphs from codelets and connecting tensors to vertices
        AstNode::user1ClearTree();
        // AstCFunc* computeSetp = nullptr;
        // m_netlistp->topModulep()->foreach([&computeSetp](AstCFunc* funcp) {
        //     if (funcp->name() == "computeSet") computeSetp = funcp;
        // });
        // UASSERT(computeSetp, "expected computeSet method!");
        // computeSetp->name("constructAll"); // rename
        // computeSetp->stmtsp()->unlinkFrBackWithNext()->deleteTree(); // remove old function
        // calls
        AstCFunc* constructAllp = new AstCFunc{m_netlistp->fileline(), "constructAll",
                                               m_netlistp->topScopep()->scopep(), "void"};
        constructAllp->isMethod(true);
        constructAllp->dontCombine(true);
        m_netlistp->topScopep()->scopep()->addBlocksp(constructAllp);
        m_netlistp->foreach([this, &constructAllp, &tileId](AstClass* classp) {
            // go through each deriviation of the base bsp class and create
            // host constructors
            if (!classp->flag().isBsp()) { return; /*some other class*/ }
            AstCFunc* ctorp = createVertexCons(classp, tileId);
            tileId++;
            tileId %= maxTileId;
            m_netlistp->topScopep()->scopep()->addBlocksp(ctorp);

            AstCCall* callp = new AstCCall{ctorp->fileline(), ctorp};
            callp->dtypeSetVoid();
            constructAllp->addStmtsp(new AstStmtExpr{ctorp->fileline(), callp});
        });
        // Step 2.
        // create a poplar program with the following structure:
        // Add the copy operations
        m_netlistp->foreach([this](AstCFunc* cfuncp) {
            if (cfuncp->name() == "exchange" || cfuncp->name() == "initialize") {
                // create copy operations
                addCopies(cfuncp, cfuncp->name() == "initialize");
            }
        });
        patchHostHandle();
        // remove the computeSet funciton, not used
        getFunc(m_netlistp->topModulep(), "computeSet")->unlinkFrBack()->deleteTree();



    }
};
void V3BspPoplarProgram::createProgram(AstNetlist* nodep) {

    UINFO(3, "Creating poplar program" << endl);
    { PoplarHostInteractionVisitor{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspPoplarHost", 0, dumpTree() >= 1);
    { PoplarViewsVisitor{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bspPoplarView", 0, dumpTree() >= 1);
    { PoplarComputeGraphBuilder{nodep}; }  // destroy before checking
    V3Global::dumpCheckGlobalTree("bscPoplalProgram", 0, dumpTree() >= 1);
}
