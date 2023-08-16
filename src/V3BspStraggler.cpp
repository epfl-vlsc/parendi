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

namespace {

class BspStragglerVisitor : public VNVisitor {
private:
    AstClass* m_bspClassp = nullptr;
    AstCFunc* m_funcp = nullptr;
    struct Record {
        int cost = 0;
        int memoryBytes = 0;
        int stackBytes = 0;
    };

    std::unordered_map<AstClass*, Record> m_records;

    void visit(AstVar* varp) override {
        if (!m_bspClassp) return;
        int bytes = varp->dtypep()->arrayUnpackedElements() * varp->dtypep()->widthWords()
                    * VL_BYTES_I(VL_IDATASIZE);
        if (m_funcp) {
            m_records[m_bspClassp].stackBytes += bytes;
        } else {
            m_records[m_bspClassp].memoryBytes += bytes;
        }
    }

    void visit(AstCFunc* cfuncp) override {
        if (!m_bspClassp) { return; }
        if (cfuncp->name() == "nbaTop") {
            std::unique_ptr<std::ofstream> ofsp;
            if (dump() >= 10) {
                ofsp = std::unique_ptr<std::ofstream>{V3File::new_ofstream(
                    v3Global.debugFilename("cost_" + m_bspClassp->name() + ".txt"))};
            }
            uint32_t count = V3InstrCount::count(cfuncp, true, ofsp.get());
            m_records[m_bspClassp].cost = static_cast<int>(count);
        }
        VL_RESTORER(m_funcp);
        {
            m_funcp = cfuncp;
            iterateChildren(cfuncp);
        }
    }
    void visit(AstClass* classp) override {
        VL_RESTORER(m_bspClassp);
        {

            if (!classp->flag().isBsp() || classp->flag().isBspInit()
                || classp->flag().isBspCond()) {
                return;
            }
            m_bspClassp = classp;
            m_records.emplace(classp, Record{.cost = 0, .memoryBytes = 0, .stackBytes = 0});
            iterateChildren(classp);
        }
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    BspStragglerVisitor(AstNetlist* netlistp) {
        iterate(netlistp);

        std::vector<std::pair<AstClass*, Record>> records{m_records.begin(), m_records.end()};
        std::sort(
            records.begin(), records.end(),
            [](const std::pair<AstClass*, Record>& p1, const std::pair<AstClass*, Record>& p2) {
                return p1.second.cost > p2.second.cost;
            });
        const std::string filename = v3Global.opt.makeDir() + "/" + "estimatedCost.txt";

        // UINFO(0, "Dumping estimated const to " << filename << endl);
        std::ofstream ofs(filename, std::ios::out);
        ofs << std::setw(20) << "Vertex"
            << "\t"
            << "Tile"
            << "\t"
            << "Worker"
            << "\t"
            << "Cycles"
            << "\t"
            << "Memory"
            << "\t"
            << "Stack" << std::endl;
        for (const auto& pair : records) {
            const Record& r = pair.second;
            ofs << pair.first->name() << "\t" << pair.first->flag().tileId() << "\t"
                << pair.first->flag().workerId() << "\t" << r.cost << "\t" << r.memoryBytes
                << "B\t" << r.stackBytes << "B" << std::endl;
        }
        ofs.flush();
        ofs.close();
    }
};

};  // namespace

void V3BspStraggler::report() {
    AstNetlist* const netlistp = v3Global.rootp();
    { BspStragglerVisitor{netlistp}; }
};