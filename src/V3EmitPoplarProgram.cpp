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
        if (header) ofp()->putsGuard();
        puts("#include <verilated.h>\n");

        // puts("using namespace poplar;");
        if (!header) { puts("#include <vlpoplar/verilated_poplar_context.h>\n"); }
        if (!header) {
            AstCFile* const cfilep = new AstCFile{v3Global.rootp()->fileline(), filename};
            cfilep->slow(false);
            cfilep->source(true);
            cfilep->codelet(false);
            v3Global.rootp()->addFilesp(cfilep);
        }
        // if (v3Global.dpi()) { v3fatal("dpi not supported with poplar\n"); }
    }
    void emitDpis(const std::vector<AstCFunc*> dpis) {

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
    }

public:
    explicit EmitPoplarProgram(AstNetlist* netlistp) {
        // C++ header fileI
        openNextOutputFile(prefixNameProtect(netlistp->topModulep()), true);

        puts("class VlPoplarContext;\n");
        puts("class ");
        puts(prefixNameProtect(netlistp->topModulep()));
        puts(" final {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);
        // hardcoded constructor
        string ctxName;
        std::vector<std::pair<AstCFunc*, AstNodeModule*>> extraFuncsp;
        std::vector<AstCFunc*> dpisp;
        for (AstNode* nodep = netlistp->topModulep()->stmtsp(); nodep; nodep = nodep->nextp()) {
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
                    emitCFuncHeader(cfuncp, netlistp->topModulep(), false);
                    puts(";\n");
                }
            } else if (AstCell* const cellp = VN_CAST(nodep, Cell)) {
                if (!cellp->modp()->inLibrary()) {
                    // cellp->v3error("Do not know how emit node " << cellp << endl);
                    continue;
                }
                for (AstNode* nodep = cellp->modp()->stmtsp(); nodep; nodep = nodep->nextp()) {
                    if (AstCFunc* cfuncp = VN_CAST(nodep, CFunc)) {
                        extraFuncsp.emplace_back(cfuncp, cellp->modp());
                    }
                }
            }
        }
        puts(prefixNameProtect(netlistp->topModulep()));
        puts("(VlPoplarContext& ctx) : " + ctxName + " (ctx) {}\n");  // hacky
        ensureNewLine();
        puts("};\n");
        puts("\n");
        if (!dpisp.empty()) { emitDpis(dpisp); }
        puts("\n");
        for (const auto& extrap : extraFuncsp) {
            emitCFuncHeader(extrap.first, extrap.second, false);
            puts(";\n");
        }

        ofp()->putsEndGuard();
        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);

        // C++ implementations
        auto emitFuncImpl = [this, &netlistp](AstCFunc* cfuncp) {
            if (!m_ofp || splitNeeded()) {
                if (m_ofp) VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
                openNextOutputFile(prefixNameProtect(m_modp), false);
                puts("#include \"");
                puts(prefixNameProtect(m_modp));
                puts(".h\"\n");
            }
            EmitCFunc::visit(cfuncp);
        };
        m_modp = netlistp->topModulep();
        netlistp->topModulep()->foreach(emitFuncImpl);
        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
        for (const auto& extrap : extraFuncsp) {
            if (m_modp != extrap.second) { VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr); }
            m_modp = extrap.second;
            emitFuncImpl(extrap.first);
        }
        if (m_ofp) { VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr); }
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
        ofp->puts("INCLUDES = -I$(VERIPOPLAR_ROOT)/include -I$(VERIPOPLAR_ROOT)/vltstd -I.\n");
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
        ofp->puts("HOST_FLAGS = --std=c++17 -g $(INCLUDES) $(HOST_DEFINES)\n");
        ofp->puts("IPU_FLAGS = -O3 $(INCLUDES) -X-funroll-loops "
                  "-X-finline-functions -X-finline-hint-functions \n");
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
        ofp->puts("OBJS_HOST = $(HOST_SOURCES:cpp=o)\n");
        ofp->puts("OBJS_GP = $(CODELETS:cpp=gp)\n");
        ofp->puts("OBJS_S = $(CODELETS:cpp=s)\n");
        ofp->puts("\n");
        ofp->puts("INSTRUMENT ?= 0\n");
        ofp->puts("ifneq ($(INSTRUMENT), 0)\n");
        ofp->puts("HOST_FLAGS += -DPOPLAR_INSTRUMENT\n");
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
        string graphFile
            = EmitPoplarProgram::prefixNameProtect(netlistp->topModulep()) + ".graph.bin";
        ofp->puts(EmitPoplarProgram::topClassName()
                  + "_graph_compiler: $(OBJS_HOST) $(OBJS_GP) $(VERILATOR_CPP)\n");
        ofp->puts("\t$(CXX) $(HOST_FLAGS) $(OBJS_HOST) $(VERILATOR_CPP) $(LIBS) -DGRAPH_COMPILE "
                  "-o $@\n");
        ofp->puts(graphFile + ": " + EmitPoplarProgram::topClassName() + "_graph_compiler\n");
        ofp->puts("\t./$< $(GRAPH_FLAGS)\n");
        ofp->puts(EmitPoplarProgram::topClassName() + ": $(OBJS_HOST) $(OBJS_GP) $(VERILATOR_CPP) "
                  + graphFile + "\n");
        ofp->puts("\t$(CXX) $(HOST_FLAGS) $(OBJS_HOST) $(VERILATOR_CPP) $(LIBS) -o $@\n");
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
        putRecord("autoReport.streamAtEachRun", "false");
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