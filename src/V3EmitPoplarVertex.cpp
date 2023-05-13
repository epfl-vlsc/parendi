// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit C++ (poplar)
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

#include "V3Ast.h"
#include "V3BspModules.h"
#include "V3EmitC.h"
#include "V3EmitCBase.h"
#include "V3EmitCFunc.h"
#include "V3EmitPoplar.h"
#include "V3Global.h"
#include "V3UniqueNames.h"

#include <algorithm>
#include <deque>
#include <list>
#include <map>
#include <unordered_set>
#include <vector>
VL_DEFINE_DEBUG_FUNCTIONS;

class EmitPoplarVertex final : public EmitCFunc {
private:
    // MEMBERS
    // const AstClass* const m_fileClassp;
    // const bool m_slow;
    V3UniqueNames m_uniqueNames;

    // hacky override
    void visit(AstReadMemProxy* nodep) override {
        puts(nodep->cFuncPrefixp());
        puts("(");
        UASSERT_OBJ(!nodep->lsbp() && !nodep->msbp(), nodep, "can not do start/end address!");
        const AstVarRefView* const viewp = VN_CAST(nodep->memp(), VarRefView);
        const AstVarRefView* const hviewp = VN_CAST(nodep->filenamep(), VarRefView);
        if (!viewp || !hviewp) { nodep->v3error(nodep->verilogKwd() << " expected VarRefView"); }
        AstVarRef* const vrefp = viewp->vrefp();
        AstVarRef* const hvrefp = hviewp->vrefp();
        AstVectorDType* const dtypep = VN_CAST(vrefp->varp()->dtypeSkipRefp(), VectorDType);
        if (!dtypep) { nodep->v3error(nodep->verilogKwd() << "unsupported data type"); }
        puts(cvtToStr(dtypep->size()));
        puts(", ");
        iterateAndNextNull(nodep->filenamep());
        puts(", ");
        iterateAndNextNull(nodep->memp());
        puts(");\n");
    }

    void maybeOpenNextFile() {
        if (!m_ofp || splitNeeded()) {
            if (m_ofp) VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
            openNextOutputFile("codelet");
        }
    }
    void openNextOutputFile(const string& subFileName) {
        UASSERT(!m_ofp, "Output file already open");

        splitSizeReset();  // Reset file size tracking
        m_lazyDecls.reset();  // Need to emit new lazy declarations

        string filename = v3Global.opt.makeDir() + "/" + topClassName();
        if (!subFileName.empty()) {
            filename += "__" + subFileName;
            filename = m_uniqueNames.get(filename);
        }
        filename += ".cpp";

        AstCFile* const cfilep = new AstCFile{v3Global.rootp()->fileline(), filename};
        cfilep->slow(false);
        cfilep->source(true);
        cfilep->codelet(true);
        v3Global.rootp()->addFilesp(cfilep);

        m_ofp = new V3OutCFile{filename};
        // new AstCFile{}
        ofp()->putsHeader();
        puts("// DESCRIPTION: Verilator output: Design implementation internals\n");
        puts("// Poplar vertex implementation\n");
        puts("#include <vlpoplar/verilated.h>\n");
        puts("#include <poplar/Vertex.hpp>\n");
        // puts("using namespace poplar;");

        if (v3Global.dpi()) { v3fatal("dpi not supported with poplar\n"); }
    }

    void emitClass(const AstClass* classp) {

        m_modp = classp;  // used by EmitCFunc::visit
        maybeOpenNextFile();
        puts("\nclass ");
        puts(prefixNameProtect(classp));
        puts(" : public poplar::Vertex {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);  // public

        // emit the members

        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstVar* vrefp = VN_CAST(stmtp, Var)) {
                UASSERT_OBJ(vrefp->isClassMember(), vrefp, "expected class member");
                auto flag = vrefp->bspFlag();
                puts("/* [" + flag.ascii() + "] */\n");
                puts("poplar::InOut<");
                puts(vrefp->dtypep()->cType("", false, false));
                puts("> ");
                puts(vrefp->nameProtect());
                puts(";\n");
            }
        }

        // emit method decls
        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) {
                emitCFuncHeader(funcp, classp, false);
                puts(";\n");
            }
        }
        ensureNewLine();
        puts("};\n\n");

        // emit method defs

        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) { EmitCFunc::visit(funcp); }
        }
    }

public:
    explicit EmitPoplarVertex(AstNetlist* netlistp) {
        for (const AstNode* nodep = netlistp->modulesp(); nodep; nodep = nodep->nextp()) {
            if (const AstClass* classp = VN_CAST(nodep, Class)) {
                if (classp->flag().isBsp()) {
                    UINFO(3, "Emitting " << classp->nameProtect() << endl);
                    emitClass(classp);
                }
            }
        }
        if (m_ofp) VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
    }
};
void V3EmitPoplar::emitVertex() {
    // Make parent module pointers available, enables user4
    const EmitCParentModule emitCParentModule;
    AstNetlist* netlistp = v3Global.rootp();
    { EmitPoplarVertex{netlistp}; }
}