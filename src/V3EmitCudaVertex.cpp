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
#include "V3EmitC.h"
#include "V3EmitCFunc.h"
#include "V3EmitCuda.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;  // NOLINT

class EmitCudaVertex final : public EmitCFunc {
private:
    V3UniqueNames m_uniqueNames;
    std::vector<AstCFile*> m_newFilesp;
    bool splitNeededAndEnabled() const { return EmitCFunc::splitNeeded(); }
    void maybeOpenNextFile() {
        if (!m_ofp || splitNeededAndEnabled()) {
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

        // v3Global.rootp()->addFilesp(cfilep);
        m_newFilesp.push_back(cfilep);

        m_ofp = new V3OutCFile{filename};
        // new AstCFile{}
        ofp()->putsHeader();
        puts("// DESCRIPTION: Verilator output: Design implementation internals\n");
        puts("// Poplar vertex implementation\n");
        puts("#include <vlpoplar/verilated.h>\n");
        puts("#include <poplar/Vertex.hpp>\n");
        puts("#include \"" + topClassName() + "__structs.h\"\n");
        // puts("using namespace poplar;");

        // if (v3Global.dpi()) { v3warn("dpi not supported with poplar\n"); }
    }
    void emitClass(const AstClass* classp) {
        m_modp = classp;  // used by EmitCFunc::visit
        maybeOpenNextFile();
        puts("// at TILE = " + cvtToStr(classp->flag().tileId())
             + "   WORKER = " + cvtToStr(classp->flag().workerId()) + "\n");
        puts("\nclass ");
        puts(prefixNameProtect(classp));
        string baseClass = "Vertex";
        if (classp->flag().isSupervisor()) { baseClass = "SupervisorVertex"; }
        puts(" : public poplar::" + baseClass + " {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);  // public

        // emit the members
        // ofp()->puts("using Vec = poplar::InOut<poplar::Vector<IData, "
        //             "poplar::VectorLayout::COMPACT_PTR, alignof(QData)>>;\n\n");
        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstVar* vrefp = VN_CAST(stmtp, Var)) {
                UASSERT_OBJ(vrefp->isClassMember(), vrefp, "expected class member");
                auto flag = vrefp->bspFlag();
                puts("/* [" + flag.ascii() + "] */\n");
                if (flag.isInputOnly()) {
                    puts("VecIn ");
                } else {
                    puts("Vec ");
                }
                // puts(vrefp->dtypep()->cType("", false, false));
                // puts("> ");
                puts(vrefp->nameProtect());
                puts("; /* " + AstNode::dedotName(vrefp->origName()) + " : "
                     + vrefp->fileline()->ascii() + " */\n");
            }
        }

        // emit method decls
        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) {
                if (classp->flag().isSupervisor() && funcp->name() == "compute") {
                    puts("__attribute__((target(\"supervisor\"))) ");
                }
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
    explicit EmitCudaVertex(AstNetlist* netlistp) {
        std::vector<const AstClass*> toEmitp;
        for (const AstNode* nodep = netlistp->modulesp(); nodep; nodep = nodep->nextp()) {
            if (const AstClass* classp = VN_CAST(nodep, Class)) { toEmitp.push_back(classp); }
        }
        for (const AstClass* classp : toEmitp) {
            UINFO(3, "Emitting " << classp->nameProtect() << '\n');
            emitClass(classp);
        }
        if (m_ofp) { VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr); }
    }
};
void V3EmitCuda::emitVertex() {
    // Make parent module pointers available, enables user4
    const EmitCParentModule emitCParentModule;
    AstNetlist* netlistp = v3Global.rootp();
    { EmitCudaVertex{netlistp}; }
    V3Stats::statsStage("emitVertex");
}
