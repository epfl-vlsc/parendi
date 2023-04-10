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
    const AstClass* const m_fileClassp;
    const bool m_slow;
    V3UniqueNames m_uniqueNames;

    void emitFileList() {}
    void openNextOutputFile(const string& subFileName) {
        UASSERT(!m_ofp, "Output file already open");

        splitSizeReset();  // Reset file size tracking
        m_lazyDecls.reset();  // Need to emit new lazy declarations

        string filename = v3Global.opt.makeDir() + "/" + prefixNameProtect(m_fileClassp);
        if (!subFileName.empty()) {
            filename += "__" + subFileName;
            filename = m_uniqueNames.get(filename);
        }
        if (m_slow) filename += "__Slow";
        filename += ".cpp";

        AstCFile* const cfilep = new AstCFile{v3Global.rootp()->fileline(), filename};
        cfilep->slow(m_slow);
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

    void emitClass() {

        openNextOutputFile(m_fileClassp->name());

        puts("\nclass ");
        puts(prefixNameProtect(m_fileClassp));
        puts(" : public poplar::Vertex {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);  // public

        // emit the members

        for (AstNode* stmtp = m_fileClassp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstVar* vrefp = VN_CAST(stmtp, Var)) {
                UASSERT_OBJ(vrefp->isClassMember(), vrefp, "expected class member");
                puts("poplar::InOut<");
                puts(vrefp->dtypep()->cType("", false, false));
                puts("> ");
                puts(vrefp->nameProtect());
                puts(";\n");
            }
        }

        // emit method decls
        for (AstNode* stmtp = m_fileClassp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) {
                emitCFuncHeader(funcp, m_fileClassp, false);
                puts(";");
            }
        }
        ensureNewLine();
        puts("};\n\n");

        // emit method defs

        for (AstNode* stmtp = m_fileClassp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) { EmitCFunc::visit(funcp); }
        }
        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
    }

public:
    explicit EmitPoplarVertex(const AstClass* classp, bool slow)
        : m_fileClassp{classp}
        , m_slow{slow} {
        m_modp = classp;
        emitClass();
    }
};

void V3EmitPoplar::emitVertex() {

    // Make parent module pointers available, enables user4
    const EmitCParentModule emitCParentModule;

    AstNetlist* netlistp = v3Global.rootp();
    // find the classes that are derived from the V3BspModules::builtinBspCompute class

    for (const AstNode* nodep = netlistp->modulesp(); nodep; nodep = nodep->nextp()) {
        if (const AstClass* classp = VN_CAST(nodep, Class)) {
            if (classp->flag().isBsp()) {
                UINFO(3, "Emitting " << classp->nameProtect() << endl);
                { EmitPoplarVertex{classp, false /*slow*/}; }
            }
        }
    }
}