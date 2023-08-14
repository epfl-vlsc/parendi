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
#include "V3Os.h"
#include "V3Stats.h"
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
    AstNetlist* m_netlistp = nullptr;
    std::vector<string> m_headers;
    void openNextOutputFile(const string& suggestion, bool header) {
        UASSERT(!m_ofp, "Output file already open");

        splitSizeReset();  // Reset file size tracking
        m_lazyDecls.reset();  // Need to emit new lazy declarations
        string filename;
        if (header) {
            m_headers.emplace_back(suggestion + ".h");
            filename = v3Global.opt.makeDir() + "/" + suggestion + ".h";
        } else {
            filename = v3Global.opt.makeDir() + "/" + m_uniqueNames.get(suggestion) + ".cpp";
        }

        m_ofp = new V3OutCFile{filename};
        ofp()->putsHeader();
        puts("// DESCRIPTION: Verilator output: Design implementation internals\n");
        puts("// Poplar vertex implementation\n");
        if (header) ofp()->putsGuard();
        puts("#include <verilated.h>\n");
        puts("#include \"" + topClassName() + "__structs.h\"\n");
        // puts("using namespace poplar;");
        if (!header) {
            puts("#include <vlpoplar/verilated_poplar_context.h>\n");
            for (const auto& hdr : m_headers) { puts("#include \"" + hdr + "\"\n"); }
        }
        if (!header) {
            AstCFile* const cfilep = new AstCFile{v3Global.rootp()->fileline(), filename};
            cfilep->slow(false);
            cfilep->source(true);
            cfilep->codelet(false);
            v3Global.rootp()->addFilesp(cfilep);
        }

        // if (v3Global.dpi()) { v3fatal("dpi not supported with poplar\n"); }
    }

    void closeFile(bool header) {
        m_ofp->putsEndGuard();
        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
    }
    void emitDpis(const std::vector<AstCFunc*> dpis) {

        UASSERT(!m_ofp, "file not closed");
        openNextOutputFile(topClassName() + "__Dpi", true);
        puts("\n");
        puts("#include \"svdpi.h\"\n");
        puts("\n");
        puts("#ifdef __cplusplus\n");
        puts("extern \"C\" {\n");
        puts("#endif\n");
        puts("\n");
        int firstExp = 0;
        int firstImp = 0;
        for (AstCFunc* nodep : dpis) {
            if (nodep->dpiExportDispatcher()) {
                if (!firstExp++) puts("\n// DPI EXPORTS\n");
                putsDecoration("// DPI export" + ifNoProtect(" at " + nodep->fileline()->ascii())
                               + "\n");
                puts("extern " + nodep->rtnTypeVoid() + " " + nodep->nameProtect() + "("
                     + cFuncArgs(nodep) + ");\n");
            } else if (nodep->dpiImportPrototype()) {
                if (!firstImp++) puts("\n// DPI IMPORTS\n");
                putsDecoration("// DPI import" + ifNoProtect(" at " + nodep->fileline()->ascii())
                               + "\n");
                puts("extern " + nodep->rtnTypeVoid() + " " + nodep->nameProtect() + "("
                     + cFuncArgs(nodep) + ");\n");
            }
        }
        puts("\n");
        puts("#ifdef __cplusplus\n");
        puts("}\n");
        puts("#endif\n");
        closeFile(true);
    }

    std::vector<AstCFunc*> emitModule(AstNodeModule* modp) {
        openNextOutputFile(prefixNameProtect(modp), true);
        // emit any typedef structure that may live under the modp

        puts("class VlPoplarContext;\n");
        puts("class ");
        puts(prefixNameProtect(modp));
        puts(" final {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);
        // hardcoded constructor
        string ctxName;
        std::vector<AstCFunc*> dpisp;
        for (AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            if (AstVar* varp = VN_CAST(nodep, Var)) {
                if (varp->dtypep()->basicp()
                    && varp->dtypep()->basicp()->keyword() == VBasicDTypeKwd::POPLAR_CONTEXT)
                    ctxName = varp->name();  // hacky
                puts(varp->dtypep()->cType("", false, false));
                puts(" ");
                puts(varp->nameProtect());
                puts(";\n");

            } else if (AstCFunc* cfuncp = VN_CAST(nodep, CFunc)) {
                if (cfuncp->dpiImportPrototype()) {
                    dpisp.push_back(cfuncp);
                } else {
                    emitCFuncHeader(cfuncp, modp, false);
                    puts(";\n");
                }
            }
        }
        puts(prefixNameProtect(modp));
        puts("(VlPoplarContext& ctx) : " + ctxName + " (ctx) {}\n");  // hacky
        ensureNewLine();
        puts("};\n");
        puts("\n");
        puts("\n");
        closeFile(true);
        return dpisp;
    }

    void emitPackageDollarUnitHeader() {
        if (AstPackage* pkgp = m_netlistp->dollarUnitPkgp()) {
            openNextOutputFile(prefixNameProtect(pkgp), true);
            pkgp->foreach([this, pkgp](AstCFunc* cfuncp) {
                emitCFuncHeader(cfuncp, pkgp, false);
                puts(";\n");
            });
            closeFile(true);
        }
    }
    void emitCFuncImpl(AstCFunc* cfuncp) {
        if (cfuncp->dpiImportPrototype()) return;
        if (!m_ofp || splitNeeded()) {
            if (m_ofp) VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
            openNextOutputFile(prefixNameProtect(m_modp), false);
        }
        EmitCFunc::visit(cfuncp);
    }

    void emitModuleImpl(AstNodeModule* modp) {
        m_modp = modp;
        modp->foreach([this](AstCFunc* cfuncp) { emitCFuncImpl(cfuncp); });
        m_modp = nullptr;
    }

    void emitDollarPackageImpl() {

        if (AstPackage* pkgp = m_netlistp->dollarUnitPkgp()) {
            m_modp = pkgp;
            pkgp->foreach([this](AstCFunc* cfuncp) { emitCFuncImpl(cfuncp); });
            m_modp = nullptr;
        }
    }

public:
    explicit EmitPoplarProgram(AstNetlist* netlistp) {
        // C++ header files
        m_netlistp = netlistp;
        auto dpisp = emitModule(netlistp->topModulep());
        if (!dpisp.empty()) { emitDpis(dpisp); }
        emitPackageDollarUnitHeader();

        // C++ implementations
        emitModuleImpl(netlistp->topModulep());
        if (m_ofp) closeFile(false);

        emitDollarPackageImpl();
        if (m_ofp) closeFile(false);
    }
};

// class EmitPoplarMake
void V3EmitPoplar::emitProgram() {

    // Make parent module pointers available, enables user4
    const EmitCParentModule emitCParentModule;

    AstNetlist* netlistp = v3Global.rootp();
    // find the classes that are derived from the V3BspModules::builtinBspCompute class
    UINFO(3, "Emitting program");
    { EmitPoplarProgram{netlistp}; }
    V3Stats::statsStage("emitProgram");
}