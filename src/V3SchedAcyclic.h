// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Scheduling - break combinational cycles
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

#ifndef VERILATOR_SCHEDACYCLIC_H_
#define VERILATOR_SCHEDACYCLIC_H_
#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3Graph.h"

namespace V3Sched {
namespace V3SchedAcyclic {
// ##############################################################################
//  Data structures (graph types)

class LogicVertex final : public V3GraphVertex {
    AstNode* const m_logicp;  // The logic node this vertex represents
    AstScope* const m_scopep;  // The enclosing AstScope of the logic node

public:
    LogicVertex(V3Graph* graphp, AstNode* logicp, AstScope* scopep)
        : V3GraphVertex{graphp}
        , m_logicp{logicp}
        , m_scopep{scopep} {}
    AstNode* logicp() const { return m_logicp; }
    AstScope* scopep() const { return m_scopep; }

    // LCOV_EXCL_START // Debug code
    string name() const override {
        return cvtToHex(m_logicp) + "\n" + m_logicp->fileline()->ascii();
    };
    string dotShape() const override { return "rectangle"; }
    // LCOV_EXCL_STOP
};

class VarVertex final : public V3GraphVertex {
    AstVarScope* const m_vscp;  // The AstVarScope this vertex represents

public:
    VarVertex(V3Graph* graphp, AstVarScope* vscp)
        : V3GraphVertex{graphp}
        , m_vscp{vscp} {}
    AstVarScope* vscp() const { return m_vscp; }
    AstVar* varp() const { return m_vscp->varp(); }

    // LCOV_EXCL_START // Debug code
    string name() const override { return m_vscp->name(); }
    string dotShape() const override { return "ellipse"; }
    string dotColor() const override { return "blue"; }
    // LCOV_EXCL_STOP
};

class Graph final : public V3Graph {
    void loopsVertexCb(V3GraphVertex* vtxp) override {
        // TODO: 'typeName' is an internal thing. This should be more human readable.
        if (LogicVertex* const lvtxp = dynamic_cast<LogicVertex*>(vtxp)) {
            AstNode* const logicp = lvtxp->logicp();
            std::cerr << logicp->fileline()->warnOtherStandalone()
                      << "     Example path: " << logicp->typeName() << endl;
        } else {
            VarVertex* const vvtxp = dynamic_cast<VarVertex*>(vtxp);
            UASSERT(vvtxp, "Cannot be anything else");
            AstVarScope* const vscp = vvtxp->vscp();
            std::cerr << vscp->fileline()->warnOtherStandalone()
                      << "     Example path: " << vscp->prettyName() << endl;
        }
    }
};

// remove noncyclic parts of the graph
void removeNonCyclic(Graph* graphp);

// A VarVertex together with its fanout
using Candidate = std::pair<VarVertex*, unsigned>;
// gather all the scc candidates
std::vector<Candidate> gatherSCCCandidates(Graph* graphp, V3GraphVertex* vtxp);

// find all the vertices on the cuts
std::vector<VarVertex*> findCutVertices(Graph* graphp);
};  // namespace V3SchedAcyclic
};  // namespace V3Sched
#endif
