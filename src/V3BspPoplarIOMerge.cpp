// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Merge Input fields in each vertex
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

#include "V3BspPoplarIOMerge.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3Global.h"
#include "V3UniqueNames.h"

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

class MergeIOVisitor final : public VNVisitor {
private:
    AstNetlist* m_netlistp = nullptr;
    AstClass* m_classp = nullptr;
    int m_nextOffset = 0;

    VNUser1InUse m_user1Inuse;
    VNUser2InUse m_user2InUse;
    struct InputVarReplacement {
        const int m_offset;
        AstCFunc* const m_funcp;
        AstNodeDType* const m_dtp;
        AstVarScope* const m_inputVscp;  // the new aggregated input variable
        inline AstSliceSel* mkSlice(FileLine* flp, AstNodeExpr* fromp) {
            AstMemberSel* const memselp
                = new AstMemberSel{flp, fromp, VFlagChildDType{}, m_inputVscp->varp()->name()};
            memselp->varp(m_inputVscp->varp());
            memselp->dtypep(m_inputVscp->dtypep());

            AstSliceSel* const slicep = new AstSliceSel{flp, memselp, range()};
            slicep->dtypep(m_dtp);  // modify dtype

            return slicep;
        }
        inline VNumRange range() const {
            UASSERT(m_dtp->arrayUnpackedElements()
                        < static_cast<uint32_t>(std::numeric_limits<int>::max()),
                    "underflow array size");
            return VNumRange{m_offset, m_offset
                                           + static_cast<int>(m_dtp->arrayUnpackedElements())
                                                 * m_dtp->widthWords()
                                           - 1};
        }
        inline AstNodeExpr* mkConstRef() const {
            AstCCall* const callp = new AstCCall{m_funcp->fileline(), m_funcp, nullptr};
            callp->dtypeFrom(m_dtp);
            return callp;
        };
        InputVarReplacement(int offset, AstCFunc* funcp, AstNodeDType* dtp, AstVarScope* inputVscp)
            : m_offset(offset)
            , m_funcp(funcp)
            , m_dtp(dtp)
            , m_inputVscp(inputVscp) {}
        ~InputVarReplacement() { UINFO(3, "Deleted" << endl); }
    };
    AstUser2Allocator<AstVar, std::unique_ptr<InputVarReplacement>> m_varReplacement;

    // STATE, clear on netlist
    //  AstVar::user1() -> true if a class member
    //  AstVarRef::user1() -> true if processed
    //  AstClass::user2u() -> replacement recipes

    inline bool isClassMember(AstVar* varp) const { return varp->user1(); }
    inline bool isProcessed(AstVarRef* vrefp) const { return vrefp->user1(); }

    AstClass* getClass(AstNode* nodep) const {
        return VN_AS(VN_AS(nodep, MemberSel)->fromp()->dtypep(), ClassRefDType)->classp();
    }

    void sliceExchange(AstCFunc* cfuncp) {
        for (AstNode *nodep = cfuncp->stmtsp(), *nextp = nullptr; nodep; nodep = nextp) {
            nextp = nodep->nextp();

            UASSERT(VN_IS(nodep, Assign), "expected AstAssign");
            AstAssign* const assignp = VN_AS(nodep, Assign);

            AstMemberSel* lhsp = VN_AS(assignp->lhsp(), MemberSel);
            AstMemberSel* rhsp = VN_AS(assignp->rhsp(), MemberSel);
            // UINFO(3, "Rewriting " << assignp << endl);
            // if (debug() >= 3) {
            //     assignp->dumpTree("- rewriting");
            // }

            // UINFO(3, "RHSP " << rhsp << endl);
            // UINFO(3, "LHSP " << lhsp << endl);

            const auto& replacement = m_varReplacement(lhsp->varp());
            if (replacement) {
                // turn this into a slicing operation
                AstSliceSel* const newlhsp
                    = replacement->mkSlice(lhsp->fileline(), lhsp->fromp()->unlinkFrBack());
                lhsp->replaceWith(newlhsp);
                VL_DO_DANGLING(lhsp->deleteTree(), lhsp);
            } else {
                // probably something from the initialization code
            }

        }
    }
    void visit(AstVarRef* vrefp) override {

        if (!m_classp) { return; }
        if (!isClassMember(vrefp->varp()) || isProcessed(vrefp)) {
            // not class member or already processed
            return;
        }
        vrefp->user1(true);  // mark processed

        VNRelinker relinkHandle;
        vrefp->unlinkFrBack(&relinkHandle);
        UINFO(100, "Wrapping " << vrefp->name() << " in AstVarRefView " << endl);
        if (m_varReplacement(vrefp->varp())) {
            relinkHandle.relink(m_varReplacement(vrefp->varp())->mkConstRef());
        } else {
            AstVarRefView* newp = new AstVarRefView{vrefp->fileline(), vrefp};
            newp->dtypep(vrefp->varp()->dtypep());
            relinkHandle.relink(newp);
        }
    }

    void visit(AstScope* scopep) override {

        if (!m_classp) { return; }

        std::vector<AstVarScope*> inputMembersp;
        int numWords = 0;
        for (AstVarScope* vscp = scopep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
            if (vscp->varp()->bspFlag().isInputOnly()) {
                UASSERT_OBJ(vscp->varp()->user1(), vscp->varp(), "expected to be marked");
                inputMembersp.push_back(vscp);
                numWords += vscp->dtypep()->arrayUnpackedElements() * vscp->dtypep()->widthWords();
            }
        }
        AstUnpackArrayDType* const dtp
            = new AstUnpackArrayDType{m_netlistp->fileline(), m_netlistp->findSigned32DType(),
                                      new AstRange{m_netlistp->fileline(), 0, numWords - 1}};
        m_netlistp->typeTablep()->addTypesp(dtp);
        AstVar* const inputVarp
            = new AstVar{m_classp->fileline(), VVarType::MEMBER, "__VInputs", dtp};
        inputVarp->bspFlag(VBspFlag{}.append(VBspFlag::MEMBER_INPUT));
        scopep->modp()->addStmtsp(inputVarp);
        AstVarScope* const inputVscp = new AstVarScope{m_classp->fileline(), scopep, inputVarp};
        scopep->addVarsp(inputVscp);

        for (AstVarScope* vscp : inputMembersp) {
            // create a getter method for the const data
            string funcReturnTypeStr = "const " + vscp->varp()->dtypep()->cType("", true, true);
            AstCFunc* const getterp = new AstCFunc{vscp->varp()->fileline(), vscp->varp()->name(),
                                                   scopep, funcReturnTypeStr};

            AstVarRef* const inputRef = new AstVarRef{vscp->fileline(), inputVscp, VAccess::READ};
            inputRef->user1(true);  // mark processed
            AstVarRefView* const viewp = new AstVarRefView{
                vscp->fileline(), inputRef,
                new AstConst{vscp->fileline(), AstConst::Signed32{}, m_nextOffset}};
            viewp->dtypep(vscp->varp()->dtypep());

            AstCReturn* const returnp = new AstCReturn{vscp->fileline(), viewp};
            getterp->addStmtsp(returnp);
            scopep->addBlocksp(getterp);

            UINFO(3, "In class " << m_classp->name() << " var " << vscp->varp()->prettyNameQ()
                                 << " has offset " << m_nextOffset << "  " << vscp->varp() << endl);
            m_varReplacement(vscp->varp()) = std::make_unique<InputVarReplacement>(
                m_nextOffset, getterp, vscp->varp()->dtypep(), inputVscp);
            m_nextOffset += vscp->dtypep()->arrayUnpackedElements() * vscp->dtypep()->widthWords();
            // unlink and push delete (do not delete here since still referenced)
            vscp->unlinkFrBack();
            pushDeletep(vscp);
            vscp->varp()->unlinkFrBack();
            pushDeletep(vscp->varp());
        }
        // go through the code and replace variable references
        iterateAndNextNull(scopep->blocksp());
    }
    // void visit(AstClass* classp) override {
    //     VL_RESTORER(m_classp);
    //     if (!classp->flag().isBsp()) { return; }
    //     m_classp = classp;
    //     m_nextOffset = 0;
    //     iterateChildren(classp);
    // }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit MergeIOVisitor(AstNetlist* netlistp) {
        // iterate BSP classes and merge inputs
        AstNode::user1ClearTree();
        AstNode::user2ClearTree();
        m_netlistp = netlistp;
        for (AstNodeModule* modp = netlistp->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            if (AstClass* classp = VN_CAST(modp, Class)) {
                if (classp->flag().isBsp()) {
                    UINFO(3, "Visiting class " << classp->name() << endl);
                    for (AstNode* nodep =  classp->stmtsp(); nodep; nodep = nodep->nextp()) {
                        if (AstVar* const varp = VN_CAST(nodep, Var)) {
                            varp->user1(true); // mark as member
                        }
                    }
                    m_classp = classp;
                    m_nextOffset = 0;
                    iterateChildren(classp);
                    m_classp = nullptr;
                }
            }
        }
        // now fix up the exchange code with slices
        for (AstNode* nodep = netlistp->topScopep()->scopep()->blocksp(); nodep;
             nodep = nodep->nextp()) {

            AstCFunc* const cfuncp = VN_CAST(nodep, CFunc);
            if (cfuncp->name() == "exchange" || cfuncp->name() == "initialize"
                || cfuncp->name() == "dpiExchange" || cfuncp->name() == "dpiBroadcast") {
                sliceExchange(cfuncp);
            }
        }
    }
};

};  // namespace

void V3BspPoplarIOMerge::mergeIO(AstNetlist* netlistp) {

    { MergeIOVisitor{netlistp}; }
    V3Global::dumpCheckGlobalTree("bspMergeIO", 0, dumpTree() >= 1);
}
