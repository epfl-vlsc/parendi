// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit make files for OpenMP
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
#include "V3EmitOpenMP.h"
#include "V3Os.h"

VL_DEFINE_DEBUG_FUNCTIONS;
class EmitOpenMPMake final {
public:
    explicit EmitOpenMPMake(AstNetlist* netlistp) {
        // create file that lists all the thread functions first
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
        iterateCFiles([](AstCFile* cfilep) { return !cfilep->codelet() && !cfilep->constPool(); },
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
        ofp->puts("NUM_THREADS := " + cvtToStr(v3Global.opt.openmp_threads()) + "\n");
        ofp->puts("\n");
        ofp->puts("include $(PARENDI_ROOT)/include/vlopenmp/verilated.mk\n");

        delete ofp;
    }
};

class EmitOpenMPOptions final {
public:
    explicit EmitOpenMPOptions(AstNetlist* netlistp) {

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
        ofp->puts("}\n");
    }
};

void V3EmitOpenMP::emitMake() {

    AstNetlist* netlistp = v3Global.rootp();

    UINFO(3, "Emitting Makefile");
    { EmitOpenMPMake{netlistp}; }
    UINFO(10, "Emitting json options");
    { EmitOpenMPOptions{netlistp}; }
}