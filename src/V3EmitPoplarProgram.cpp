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

class EmitPoplarProgram final : public EmitCFunc {
private:
    // MEMBERS
    V3UniqueNames m_uniqueNames;

    void openNextOutputFile(const string& suggestion, bool header) {
        UASSERT(!m_ofp, "Output file already open");

        splitSizeReset();  // Reset file size tracking
        m_lazyDecls.reset();  // Need to emit new lazy declarations
        string filename;
        if (header) {
            filename = v3Global.opt.makeDir() + "/" + suggestion + ".h";
        } else {
            filename = v3Global.opt.makeDir() + "/" + m_uniqueNames.get(suggestion) + ".cpp";
        }

        m_ofp = new V3OutCFile{filename};

        ofp()->putsHeader();
        puts("// DESCRIPTION: Verilator output: Design implementation internals\n");
        puts("// Poplar vertex implementation\n");
        puts("#include \"verilated_poplar.h\"\n");
        // puts("using namespace poplar;");

        if (v3Global.dpi()) { v3fatal("dpi not supported with poplar\n"); }
    }

public:
    explicit EmitPoplarProgram(AstNetlist* netlistp) {

        openNextOutputFile("VProgram", true);
        puts("\nclass VProgram final {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);
        for (AstNode* nodep = netlistp->topModulep()->stmtsp(); nodep; nodep = nodep->nextp()) {
            if (AstVar* varp = VN_CAST(nodep, Var)) {
                puts(varp->dtypep()->cType("", false, false));
                puts(" ");
                puts(varp->nameProtect());
                puts(";\n");

            } else if (AstCFunc* cfuncp = VN_CAST(nodep, CFunc)) {

                emitCFuncHeader(cfuncp, netlistp->topModulep(), false);
                puts(";\n");
            }
        }
        netlistp->topModulep()->foreach([this, &netlistp](AstCFunc* cfuncp) {});
        ensureNewLine();
        puts("};\n");

        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);

        openNextOutputFile("VProgram", false);
        m_modp = netlistp->topModulep();
        netlistp->topModulep()->foreach(
            [this, &netlistp](AstCFunc* cfuncp) { EmitCFunc::visit(cfuncp); });

        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
    }
};

void V3EmitPoplar::emitProgram() {

    // Make parent module pointers available, enables user4
    const EmitCParentModule emitCParentModule;

    AstNetlist* netlistp = v3Global.rootp();
    // find the classes that are derived from the V3BspModules::builtinBspCompute class
    UINFO(3, "Emitting program");
    { EmitPoplarProgram{netlistp}; }
}