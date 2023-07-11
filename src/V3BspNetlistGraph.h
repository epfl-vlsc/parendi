// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: BSP netlist graph
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
//

#ifndef VERILATOR_V3BSPNETLISTGRAPH_H_
#define VERILATOR_V3BSPNETLISTGRAPH_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3Graph.h"

#include <limits>

namespace V3BspSched {
namespace Retiming {

class NetlistVertex;
class SeqWriteVertex;
class SeqReadVertex;

using LogicWithDomain = std::pair<AstSenTree*, AstNode*>;

class NetlistGraph final : public V3Graph {
private:
    uint32_t m_retimeRank = 0;
    uint32_t m_totalCost = 0;

public:
    // METHODS
    inline void addEdge(NetlistVertex* fromp, NetlistVertex* top, AstVarScope* const vscp);
    uint32_t retimeRank() const { return m_retimeRank; }
    void retimeRank(uint32_t r) { m_retimeRank = r; }
    uint32_t cost() const { return m_totalCost; }
    void cost(uint32_t c) { m_totalCost = c; }
};

class NetlistVertex VL_NOT_FINAL : public V3GraphVertex {
private:
    // the cost of this vertex alone
    const uint32_t m_cost;
    V3Hash m_hash;
    // bvalue: the cummulative cost of all vertices that are ranked higher (stricly)
    // than this vertex. Effectively, bvalue gives the cost of executing all vertices that
    // come at the same time or after this vertex.
    uint32_t m_bvalue = std::numeric_limits<uint32_t>::max();
    // rvalue: the cummulative cost of all vertices that have the same rank as this vertex
    uint32_t m_rvalue = std::numeric_limits<uint32_t>::max();
    // tvalue: the cummulative cost of all vertices that are ranked lower (strictly) than this
    // vertex. tvalue effectively computes minimum cost of executing everything within the graph
    // up to this vertex, excluding self, any parallel (same-ranked) or subsequent vertices.
    // For instance, the tvalue of exit vertices encode the cost of executing everything up to the
    // last vertex.
    uint32_t m_tvalue = std::numeric_limits<uint32_t>::max();
    // note that bvalue + rvalue + tvalue should equal the total cost of the graph.
    // on the other hand, tvalue + cost is the cost of executing everything needed
    // to compute this vertex.

protected:
    NetlistVertex(NetlistGraph* graphp, const uint32_t cost)
        : V3GraphVertex{graphp}
        , m_cost(cost) {}
    ~NetlistVertex() override = default;

public:
    uint32_t cost() const { return m_cost; }
    V3Hash hash() const { return m_hash; }
    void hash(V3Hash const hsh) { m_hash = hsh; }
    uint32_t bvalue() const { return m_bvalue; }
    void bvalue(uint32_t b) { m_bvalue = b; }
    uint32_t tvalue() const { return m_tvalue; }
    void tvalue(uint32_t t) { m_tvalue = t; }

    uint32_t rvalue() const { return m_rvalue; }
    void rvalue(uint32_t r) { m_rvalue = r; }
};

class CombVertex final : public NetlistVertex {
private:
    AstNode* const m_logicp;
    bool m_morphed = false;

public:
    CombVertex(NetlistGraph* graphp, AstNode* const logicp, uint32_t cost)
        : NetlistVertex{graphp, cost}
        , m_logicp(logicp) {}

    AstNode* logicp() const { return m_logicp; }

    string dotShape() const override final { return "ellipse"; }
};

// class SeqVertex VL_NOT_FINAL : public NetlistVertex {
// protected:
//     SeqVertex(NetlistGraph* graphp, const std::vector<LogicWithDomain> logicsp, uint32_t cost)
//         : NetlistVertex{graphp, cost}
//         , m_logicsp(logicsp) {}

// public:
//     string dotShape() const override final { return "rect"; }
// };

class SeqWriteVertex final : public NetlistVertex {
private:
    std::vector<NetlistGraph*> m_readsGraphp;
    const std::vector<LogicWithDomain> m_logicsp;

public:
    SeqWriteVertex(NetlistGraph* graphp, const std::vector<LogicWithDomain>& logicsp,
                   uint32_t cost)
        : NetlistVertex{graphp, cost}
        , m_logicsp{logicsp} {}
    const std::vector<LogicWithDomain>& logicsp() { return m_logicsp; }
    std::vector<NetlistGraph*>& readsp() { return m_readsGraphp; }
    void addReadp(NetlistGraph* vtxp) { m_readsGraphp.push_back(vtxp); }
    string dotShape() const override final { return "rect"; }
    inline NetlistGraph* slowestReader() const;
};

class SeqReadVertex final : public NetlistVertex {
private:
    AstVarScope* const m_vscp = nullptr;

public:
    SeqReadVertex(NetlistGraph* graphp, AstVarScope* const vscp)
        : NetlistVertex{graphp, 0}
        , m_vscp{vscp} {}
    string dotShape() const override final { return "rect"; }
    AstVarScope* vscp() const { return m_vscp; }
};
class NetlistEdge final : public V3GraphEdge {
private:
    AstVarScope* const m_vscp = nullptr;

public:
    NetlistEdge(NetlistGraph* graphp, NetlistVertex* fromp, NetlistVertex* top,
                AstVarScope* const vscp)
        : V3GraphEdge{graphp, fromp, top, 1, false}
        , m_vscp(vscp) {}
    string name() const override { return m_vscp->prettyName(); }
    AstVarScope* vscp() const { return m_vscp; }
};

inline void NetlistGraph::addEdge(NetlistVertex* fromp, NetlistVertex* top,
                                  AstVarScope* const vscp) {
    new NetlistEdge{this, fromp, top, vscp};
}
inline NetlistGraph* SeqWriteVertex::slowestReader() const {
    return *(std::max_element(m_readsGraphp.cbegin(), m_readsGraphp.cend(),
                              [](const NetlistGraph* rp1, const NetlistGraph* rp2) {
                                  return rp1->cost() < rp2->cost();
                              }));
}

};  // namespace Retiming

}  // namespace V3BspSched
#endif