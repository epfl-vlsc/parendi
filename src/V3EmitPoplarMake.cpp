// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit make files for poplar
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

#include "V3EmitCBase.h"
#include "V3EmitPoplar.h"
#include "V3Os.h"

VL_DEFINE_DEBUG_FUNCTIONS;
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
        const std::string listFile{EmitCBaseVisitor::prefixNameProtect(netlistp->topModulep())
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
                                           + EmitCBaseVisitor::topClassName() + ".mk"};

        ofp->puts("# Generated Makefile \n");
        ofp->puts("PARENDI_ROOT ?= " + v3Global.opt.getenvPARENDI_ROOT() + "\n\n");

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

        ofp->puts("USER_CPP = \\\n");
        for (const auto& cpp : v3Global.opt.cppFiles()) { ofp->puts("\t" + cpp + "\\\n"); }
        string graphFile
            = EmitCBaseVisitor::prefixNameProtect(netlistp->topModulep()) + ".graph.bin";
        ofp->puts("\n");
        ofp->puts("VMAIN := " + EmitCBaseVisitor::topClassName() + "\n");
        ofp->puts("VMAIN_ROOT := " + EmitCBaseVisitor::prefixNameProtect(netlistp->topModulep())
                  + "\n");
        ofp->puts("OBJ_DIR := " + v3Global.opt.makeDir() + "\n");
        ofp->puts("TILES_USED := " + cvtToStr(v3Global.opt.tiles()) + "\n");
        ofp->puts("WORKERS_USED := " + cvtToStr(v3Global.opt.workers()) + "\n");
        ofp->puts("\n");
        ofp->puts("include $(PARENDI_ROOT)/include/vlpoplar/verilated.mk\n");

        // ofp->puts("HOST_SOURCES += $(USER_CPP)\n");
        // ofp->puts("OBJS_HOST = $(HOST_SOURCES:cpp=o)\n");
        // ofp->puts("OBJS_GP = $(CODELETS:cpp=gp)\n");
        // ofp->puts("OBJS_S = $(CODELETS:cpp=s)\n");
        // ofp->puts("\n");
        // ofp->puts("INSTRUMENT ?= 0\n");
        // ofp->puts("ifneq ($(INSTRUMENT), 0)\n");
        // ofp->puts("HOST_FLAGS += -DPOPLAR_INSTRUMENT\n");
        // ofp->puts("endif\n\n");
        // ofp->puts("GRAPH_COMPILE_FLAGS = -DGRAPH_COMPILE\n");
        // ofp->puts("GRAPH_RUN_FLAGS = -DGRAPH_RUN\n");
        // ofp->puts("GRAPH_BINARY_DEP = " + graphFile + "\n");
        // ofp->puts("PRECOMPILE ?= 1\n");
        // ofp->puts("ifeq ($(PRECOMPILE), 0)\n");
        // ofp->puts("GRAPH_RUN_FLAGS += -DGRAPH_COMPILE\n");
        // ofp->puts("GRAPH_BINARY_DEP = \n");
        // ofp->puts("endif\n\n");
        // ofp->puts("all: " + EmitCBaseVisitor::topClassName() + "\n\n");
        // ofp->puts("$(OBJS_GP):%.gp: %.cpp\n");
        // ofp->puts("\t$(POPC) $^ $(IPU_FLAGS) --target ipu2 -o $@\n");
        // ofp->puts("$(OBJS_S):%.s: %.cpp\n");
        // ofp->puts("\t$(POPC) $^ $(IPU_FLAGS) -S --target ipu2 -o $@\n");
        // ofp->puts("\n");
        // ofp->puts("assembly: $(OBJS_S)\n");
        // ofp->puts("vertex: $(OBJ_GP)\n");
        // ofp->puts("\n");
        // ofp->puts("$(OBJS_HOST):%.o: %.cpp\n");
        // ofp->puts("\t$(CXX) $^ -c $(HOST_FLAGS) $(LIBS) -o $@\n");
        // ofp->puts("\n");
        // ofp->puts(EmitCBaseVisitor::topClassName()
        //           + "_graph_compiler: $(OBJS_HOST) $(OBJS_GP) $(VERILATOR_CPP)\n");
        // ofp->puts(
        //     "\t$(CXX) $(HOST_FLAGS) $(OBJS_HOST) $(VERILATOR_CPP) $(LIBS) $(GRAPH_COMPILE_FLAGS)
        //     "
        //     "-o $@\n");
        // ofp->puts(graphFile + ": " + EmitCBaseVisitor::topClassName() + "_graph_compiler\n");
        // ofp->puts("\t./$< $(GRAPH_FLAGS)\n");
        // ofp->puts(EmitCBaseVisitor::topClassName()
        //           + ": $(OBJS_HOST) $(OBJS_GP) $(VERILATOR_CPP) $(GRAPH_BINARY_DEP)\n");
        // ofp->puts("\t$(CXX) $(HOST_FLAGS) $(OBJS_HOST) $(VERILATOR_CPP) $(LIBS) "
        //           "$(GRAPH_RUN_FLAGS) -o $@\n");
        // ofp->puts("clean:\n");
        // ofp->puts("\trm -rf *.o *.gp *.s report *.graph.bin " + EmitCBaseVisitor::topClassName()
        //           + " " + EmitCBaseVisitor::topClassName() + "_graph_compiler \n");

        delete ofp;
    }
};

class EmitPoplarOptions final {
public:
    explicit EmitPoplarOptions(AstNetlist* netlistp) {

        const string fpPrefix = v3Global.opt.makeDir() + "/"
                                + EmitCBaseVisitor::prefixNameProtect(netlistp->topModulep());
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

void V3EmitPoplar::emitMake() {

    AstNetlist* netlistp = v3Global.rootp();

    UINFO(3, "Emitting Makefile");
    { EmitPoplarMake{netlistp}; }
    UINFO(10, "Emitting json options");
    { EmitPoplarOptions{netlistp}; }
}