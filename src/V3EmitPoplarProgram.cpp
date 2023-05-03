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
        if (v3Global.dpi()) { v3fatal("dpi not supported with poplar\n"); }
    }

    // void visit(AstFinish* nodep) override {
    //     puts("vl_finish(");
    //     putsQuoted(protect(nodep->fileline()->filename()));
    //     puts(", ");
    //     puts(cvtToStr(nodep->fileline()->lineno()));
    //     puts(", \"\");\n");
    // }
    // void visit(AstStop* nodep) override {
    //     puts("vl_stop(");
    //     putsQuoted(protect(nodep->fileline()->filename()));
    //     puts(", ");
    //     puts(cvtToStr(nodep->fileline()->lineno()));
    //     puts(", \"\");\n");
    // }

public:
    explicit EmitPoplarProgram(AstNetlist* netlistp) {

        openNextOutputFile(prefixNameProtect(netlistp->topModulep()), true);

        puts("class VlPoplarContext;\n");
        puts("class ");
        puts(prefixNameProtect(netlistp->topModulep()));
        puts(" final {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);
        // hardcoded constructor
        string ctxName;
        for (AstNode* nodep = netlistp->topModulep()->stmtsp(); nodep; nodep = nodep->nextp()) {
            if (AstVar* varp = VN_CAST(nodep, Var)) {
                if (varp->dtypep()->basicp()->keyword() == VBasicDTypeKwd::POPLAR_CONTEXT)
                    ctxName = varp->name();  // hacky
                puts(varp->dtypep()->cType("", false, false));
                puts(" ");
                puts(varp->nameProtect());
                puts(";\n");

            } else if (AstCFunc* cfuncp = VN_CAST(nodep, CFunc)) {

                emitCFuncHeader(cfuncp, netlistp->topModulep(), false);
                puts(";\n");
            }
        }
        puts(prefixNameProtect(netlistp->topModulep()));
        puts("(VlPoplarContext& ctx) : " + ctxName + " (ctx) {}\n");  // hacky
        netlistp->topModulep()->foreach([this, &netlistp](AstCFunc* cfuncp) {});
        ensureNewLine();
        puts("};\n");
        ofp()->putsEndGuard();
        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);

        openNextOutputFile(prefixNameProtect(netlistp->topModulep()), false);
        puts("#include \"");
        puts(prefixNameProtect(netlistp->topModulep()));
        puts(".h\"\n");
        m_modp = netlistp->topModulep();
        netlistp->topModulep()->foreach(
            [this, &netlistp](AstCFunc* cfuncp) { EmitCFunc::visit(cfuncp); });

        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
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
        const std::string listFile{v3Global.opt.makeDir() + "/"
                                   + EmitPoplarProgram::prefixNameProtect(netlistp->topModulep())
                                   + ".list"};
        {

            std::ofstream listFs{listFile, std::ios::out};
            iterateCFiles([](AstCFile* cfilep) { return cfilep->codelet(); },
                          [&](AstCFile* cfilep) {
                              listFs << V3Os::filenameDir(cfilep->name()) + "/"
                                            + V3Os::filenameNonExt(cfilep->name())
                                     << ".gp" << std::endl;
                          });
        }

        V3OutMkFile* ofp = new V3OutMkFile{v3Global.opt.makeDir() + "/Makefile"};

        ofp->puts("CXX ?= g++\n");
        ofp->puts("POPC ?= popc\n");
        // ofp->puts("VERIPOPLAR_ROOT ?= " + v3Global.opt.getenvVERILATOR_ROOT() + "\n");
        ofp->puts("VERIPOPLAR_ROOT = /home/mayy/workspace/veripoplar\n");
        ofp->puts("INCLUDES = -I$(VERIPOPLAR_ROOT)/include -I.\n");
        ofp->puts("LIBS = -lpoplar -lpopops -lpoputil -lpthread -lboost_filesystem "
                  "-lboost_program_options\n");
        ofp->puts("HOST_DEFINES =  \\\n");
        ofp->puts("\t-DVPROGRAM=");
        ofp->putsQuoted(EmitPoplarProgram::prefixNameProtect(netlistp->topModulep()));
        ofp->puts(" \\\n");
        ofp->puts("\t-DVPROGRAM_HEADER=");
        ofp->putsQuoted("\"" + EmitPoplarProgram::prefixNameProtect(netlistp->topModulep())
                        + ".h\"");
        ofp->puts(" \\\n");
        ofp->puts("\t-DCODELET_LIST=");
        ofp->putsQuoted("\"" + listFile + "\"");

        ofp->puts("\n");
        ofp->puts("HOST_FLAGS = --std=c++17 -g $(INCLUDES) $(HOST_DEFINES)\n");
        ofp->puts("IPU_FLAGS = -O2 $(INCLUDES)\n");
        ofp->puts("\n");
        ofp->puts("CODELETS =  \\\n");
        iterateCFiles([](AstCFile* cfilep) { return cfilep->codelet(); },
                      [&](AstCFile* cfilep) {
                          ofp->puts("\t" + V3Os::filenameNonDir(cfilep->name()) + " \\\n");
                      });

        ofp->puts("\n");
        ofp->puts("HOST_SOURCES =  \\\n");
        iterateCFiles([](AstCFile* cfilep) { return !cfilep->codelet(); },
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
        ofp->puts("all: main\n\n");
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
        ofp->puts("main: $(OBJS_HOST) $(OBJS_GP) $(VERILATOR_CPP)\n");
        ofp->puts("\t$(CXX) $(HOST_FLAGS) $(OBJS_HOST) $(VERILATOR_CPP) $(LIBS) -o $@\n");

        ofp->puts("clean:\n");
        ofp->puts("\trm -rf main *.o *.gp *.s report\n");

        delete ofp;
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
}