// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Lower the program tree create a poplar program
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

#include "V3BspStraggler.h"

#include "V3Ast.h"
#include "V3EmitCFunc.h"
#include "V3File.h"
#include "V3Global.h"
#include "V3InstrCount.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <algorithm>
VL_DEFINE_DEBUG_FUNCTIONS;

void V3BspStraggler::report() {
    AstNetlist* const netlistp = v3Global.rootp();
    std::vector<std::pair<AstClass*, uint32_t>> estimatedCost;
    netlistp->foreach([&estimatedCost](AstClass* classp) {
        if (!classp->flag().isBsp() || classp->flag().isBspInit() || classp->flag().isBspCond()) {
            return;
        }
        classp->foreach([&estimatedCost, classp](AstCFunc* cfuncp) {
            if (cfuncp->name() == "nbaTop") {
                std::unique_ptr<std::ofstream> ofsp;
                if (dump() >= 10) {
                    ofsp = std::unique_ptr<std::ofstream>{V3File::new_ofstream(
                        v3Global.debugFilename("cost_" + classp->name() + ".txt"))};
                }
                uint32_t count = V3InstrCount::count(cfuncp, true, ofsp.get());
                estimatedCost.emplace_back(classp, count);
            }
        });
    });

    std::sort(estimatedCost.begin(), estimatedCost.end(),
              [](const auto& p1, const auto& p2) { return p1.second > p2.second; });
    const std::string filename = v3Global.opt.makeDir() + "/"
                                 + EmitCFunc::prefixNameProtect(netlistp->topModulep())
                                 + "_estimated_cost.txt";
    // UINFO(0, "Dumping estimated const to " << filename << endl);
    std::ofstream ofs(filename, std::ios::out);
    for (const auto& pair : estimatedCost) {
        ofs << pair.first->name() << "\t\t@ [" << pair.first->flag().tileId() << ","
            << pair.first->flag().workerId() << "]\t\t\t" << pair.second << std::endl;
    }
    ofs.flush();
    ofs.close();
};