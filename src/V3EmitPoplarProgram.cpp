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
class EmitPoplarMake final {
public:
    explicit EmitPoplarMake(AstNetlist* netlistp) {
        // create file that lists all the codelets first
        auto iterateCFiles = [&](auto pred, auto action) {
            for (AstNodeFile* nodep = netlistp->filesp(); nodep;
                 nodep = VN_AS(nodep->nextp(), NodeFile)) {
                AstCFile* const cfilep = VN_CAST(nodep, CFile);
                if (pred(cfilep)) { action(cfilep); }
            }
        };
        const std::string listFile{EmitPoplarProgram::prefixNameProtect(netlistp->topModulep())
                                   + ".list"};
        {

            std::ofstream listFs{v3Global.opt.makeDir() + "/" + listFile, std::ios::out};
            iterateCFiles(
                [](AstCFile* cfilep) { return cfilep->codelet() || cfilep->constPool(); },
                [&](AstCFile* cfilep) {
                    listFs << V3Os::filenameNonExt(cfilep->name()) << ".gp" << std::endl;
                });
            listFs.close();
        }

        V3OutMkFile* ofp = new V3OutMkFile{v3Global.opt.makeDir() + "/"
                                           + EmitPoplarProgram::topClassName() + ".mk"};

        ofp->puts("CXX ?= g++\n");
        ofp->puts("POPC ?= popc\n");
        ofp->puts("VERIPOPLAR_ROOT ?= " + v3Global.opt.getenvVERIPOPLAR_ROOT() + "\n");
        ofp->puts(
            "INCLUDES = -I$(VERIPOPLAR_ROOT)/include -I$(VERIPOPLAR_ROOT)/include/vltstd -I.\n");
        ofp->puts("LIBS = -lpoplar -lpopops -lpoputil -lpthread "
                  "-lboost_program_options\n");
        ofp->puts("GRAPH_FLAGS ?= \n");
        ofp->puts("HOST_DEFINES =  \\\n");
        ofp->puts("\t-DVPROGRAM=");
        ofp->putsQuoted(EmitPoplarProgram::prefixNameProtect(netlistp->topModulep()));
        ofp->puts(" \\\n");
        ofp->puts("\t-DVPROGRAM_HEADER=");
        ofp->putsQuoted("\"" + EmitPoplarProgram::prefixNameProtect(netlistp->topModulep())
                        + ".h\"");
        ofp->puts(" \\\n");

        ofp->puts("\t-DROOT_NAME=");
        ofp->putsQuoted("\"" + EmitPoplarProgram::prefixNameProtect(netlistp->topModulep())
                        + "\"");
        ofp->puts("\\\n");

        ofp->puts("\t-DCODELET_LIST=");
        ofp->putsQuoted("\"" + listFile + "\"");
        ofp->puts("\\\n");
        ofp->puts("\t-DOBJ_DIR=");
        ofp->putsQuoted("\"" + v3Global.opt.makeDir() + "\"");
        ofp->puts("\n");
        ofp->puts("HOST_FLAGS = --std=c++17 -g $(INCLUDES) $(HOST_DEFINES) "
                  "-Wno-parentheses-equality \n");

        ofp->puts("HOST_FLAGS += -DVL_NUM_TILES_USED=" + cvtToStr(v3Global.opt.tiles()) + "\n");
        ofp->puts("HOST_FLAGS += -DVL_NUM_WORKERS_USED=" + cvtToStr(v3Global.opt.workers()) + "\n");

        ofp->puts("\n\n");
        ofp->puts("IPU_FLAGS = -O3 $(INCLUDES) -Wno-parentheses-equality \\\n");
        for (const string& clangFlag: {
            "-finline-functions",
            "-finline-hint-functions", // respect "inline" hints
            "-fno-builtin-memset", // do not use memset builtin function, best to unrol and inline
            "-fno-builtin-memcpy", // most copies can be done without a loop and are more efficient that way
            "-funroll-loops", // unroll loops when possible
        }) {

            ofp->puts("\t-X" + clangFlag  + " \\\n");
        }
        ofp->puts("\n");
        ofp->puts("CODELETS =  \\\n");
        iterateCFiles([](AstCFile* cfilep) { return cfilep->codelet() || cfilep->constPool(); },
                      [&](AstCFile* cfilep) {
                          ofp->puts("\t" + V3Os::filenameNonDir(cfilep->name()) + " \\\n");
                      });

        ofp->puts("\n");
        ofp->puts("HOST_SOURCES =  \\\n");
        iterateCFiles([](AstCFile* cfilep) { return !cfilep->codelet() || cfilep->constPool(); },
                      [&](AstCFile* cfilep) {
                          ofp->puts("\t" + V3Os::filenameNonDir(cfilep->name()) + " \\\n");
                      });
        ofp->puts("\n");
        ofp->puts("\n");
        ofp->puts("VERILATOR_CPP =  \\\n");
        ofp->puts("\t$(VERIPOPLAR_ROOT)/include/verilated.cpp \\\n");
        ofp->puts("\t$(VERIPOPLAR_ROOT)/include/verilated_threads.cpp \\\n");
        ofp->puts("\t$(VERIPOPLAR_ROOT)/include/vlpoplar/verilated_poplar_context.cpp\n");
        ofp->puts("\n");
        ofp->puts("USER_CPP = \\\n");
        for (const auto& cpp : v3Global.opt.cppFiles()) { ofp->puts("\t" + cpp + "\\\n"); }
        string graphFile
            = EmitPoplarProgram::prefixNameProtect(netlistp->topModulep()) + ".graph.bin";
        ofp->puts("\n");
        ofp->puts("HOST_SOURCES += $(USER_CPP)\n");
        ofp->puts("OBJS_HOST = $(HOST_SOURCES:cpp=o)\n");
        ofp->puts("OBJS_GP = $(CODELETS:cpp=gp)\n");
        ofp->puts("OBJS_S = $(CODELETS:cpp=s)\n");
        ofp->puts("\n");
        ofp->puts("INSTRUMENT ?= 0\n");
        ofp->puts("ifneq ($(INSTRUMENT), 0)\n");
        ofp->puts("HOST_FLAGS += -DPOPLAR_INSTRUMENT\n");
        ofp->puts("endif\n\n");
        ofp->puts("GRAPH_COMPILE_FLAGS = -DGRAPH_COMPILE\n");
        ofp->puts("GRAPH_RUN_FLAGS = -DGRAPH_RUN\n");
        ofp->puts("GRAPH_BINARY_DEP = " + graphFile + "\n");
        ofp->puts("PRECOMPILE ?= 1\n");
        ofp->puts("ifeq ($(PRECOMPILE), 0)\n");
        ofp->puts("GRAPH_RUN_FLAGS += -DGRAPH_COMPILE\n");
        ofp->puts("GRAPH_BINARY_DEP = \n");
        ofp->puts("endif\n\n");
        ofp->puts("all: " + EmitPoplarProgram::topClassName() + "\n\n");
        ofp->puts("$(OBJS_GP):%.gp: %.cpp\n");
        ofp->puts("\t$(POPC) $^ $(IPU_FLAGS) --target ipu2 -o $@\n");
        ofp->puts("$(OBJS_S):%.s: %.cpp\n");
        ofp->puts("\t$(POPC) $^ $(IPU_FLAGS) -S --target ipu2 -o $@\n");
        ofp->puts("\n");
        ofp->puts("assembly: $(OBJS_S)\n");
        ofp->puts("vertex: $(OBJ_GP)\n");
        ofp->puts("\n");
        ofp->puts("$(OBJS_HOST):%.o: %.cpp\n");
        ofp->puts("\t$(CXX) $^ -c $(HOST_FLAGS) $(LIBS) -o $@\n");
        ofp->puts("\n");
        ofp->puts(EmitPoplarProgram::topClassName()
                  + "_graph_compiler: $(OBJS_HOST) $(OBJS_GP) $(VERILATOR_CPP)\n");
        ofp->puts(
            "\t$(CXX) $(HOST_FLAGS) $(OBJS_HOST) $(VERILATOR_CPP) $(LIBS) $(GRAPH_COMPILE_FLAGS) "
            "-o $@\n");
        ofp->puts(graphFile + ": " + EmitPoplarProgram::topClassName() + "_graph_compiler\n");
        ofp->puts("\t./$< $(GRAPH_FLAGS)\n");
        ofp->puts(EmitPoplarProgram::topClassName()
                  + ": $(OBJS_HOST) $(OBJS_GP) $(VERILATOR_CPP) $(GRAPH_BINARY_DEP)\n");
        ofp->puts("\t$(CXX) $(HOST_FLAGS) $(OBJS_HOST) $(VERILATOR_CPP) $(LIBS) $(GRAPH_RUN_FLAGS) -o $@\n");
        ofp->puts("clean:\n");
        ofp->puts("\trm -rf *.o *.gp *.s report *.graph.bin " + EmitPoplarProgram::topClassName()
                  + " " + EmitPoplarProgram::topClassName() + "_graph_compiler \n");

        delete ofp;
    }
};
class EmitPoplarOptions final {
public:
    explicit EmitPoplarOptions(AstNetlist* netlistp) {

        const string fpPrefix = v3Global.opt.makeDir() + "/"
                                + EmitPoplarProgram::prefixNameProtect(netlistp->topModulep());
        V3OutCFile* ofp = new V3OutCFile{fpPrefix + "_compile_options.json"};
        emitOptions(ofp, true);
        delete ofp;
        ofp = new V3OutCFile{fpPrefix + "_engine_options.json"};
        emitOptions(ofp, false);
        delete ofp;
    }

    void emitOptions(V3OutFile* ofp, bool compile) {
        auto putRecord = [&](const string& k, const string& v, bool last = false) -> void {
            ofp->puts("\"" + k + "\": " + "\"" + v + "\"" + (last ? "\n" : ",\n"));
        };

        ofp->puts("{\n");
        putRecord("autoReport.all", "true");
        putRecord("autoReport.directory",
                  string{"./"} + (compile ? "" : v3Global.opt.makeDir() + "/") + "poplar_report",
                  true);
        ofp->puts("}\n");
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
    UINFO(3, "Emitting Makefile");
    { EmitPoplarMake{netlistp}; }
    UINFO(10, "Emitting json options");
    { EmitPoplarOptions{netlistp}; }
}