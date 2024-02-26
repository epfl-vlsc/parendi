// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: BSP resynchronization graph
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

#ifndef VERILATOR_V3BSPRESYNCGRAPH_H_
#define VERILATOR_V3BSPRESYNCGRAPH_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3FunctionTraits.h"
#include "V3Graph.h"
#include "V3PairingHeap.h"

namespace V3BspSched {
namespace Resync {

class ResyncVertex;
class ResyncEdge;
class CombVertex;
class SeqVertex;
class SeqReadVertex;
class CombSeqReadVertex;
class CombSeqVertex;
class SeqCombVertex;

class ResyncGraph;

struct LogicWithActive {
    AstNode* const logicp;
    AstActive* const activep;
    explicit LogicWithActive(AstNode* lgp, AstActive* actp)
        : logicp{lgp}
        , activep{actp} {}
};

struct Utils {
    struct Key {
        ResyncGraph* graphp;
        inline bool operator<(const Key& other) const;
        inline void increase(const uint32_t v) const;
    };
    using MaxHeap = PairingHeap<Key>;
    using HeapNode = MaxHeap::Node;
    enum class ResyncResolution {
        R_UNRESOLVED = 0,  // not yet resolved
        R_UNOPT,  // unoptimizable, e.g., due to impure statements
        R_RESOLVED,  // resolved, and found a solution
        R_NA,  // resolved but did not find a solution
    };
};

class ResyncGraph final : public V3Graph {
private:
    uint32_t m_cost;
    std::unique_ptr<Utils::HeapNode> m_heapNodep;
    const int m_index;
    int m_resyncRank = 0;
    Utils::ResyncResolution m_sol = Utils::ResyncResolution::R_UNRESOLVED;

public:
    explicit ResyncGraph(const int i)
        : m_heapNodep{nullptr}
        , m_index{i}
        , m_resyncRank{0}
        , m_sol{Utils::ResyncResolution::R_UNRESOLVED} {}
    inline ResyncEdge* addEdge(ResyncVertex* fromp, ResyncVertex* top, AstVarScope* const vscp);
    inline void cost(uint32_t v) { m_cost = v; }
    inline uint32_t cost() const { return m_cost; }
    inline std::unique_ptr<Utils::HeapNode>& heapNodep() { return m_heapNodep; }
    inline int index() const { return m_index; }
    inline int resyncRank() const { return m_resyncRank; }
    inline void resyncRank(int r) {
        if (r > 0) {
            m_resyncRank = r;
            m_sol = Utils::ResyncResolution::R_RESOLVED;
        } else {
            m_resyncRank = 0;
            m_sol = Utils::ResyncResolution::R_NA;
        }
    }
    inline void setUnopt() { m_sol = Utils::ResyncResolution::R_UNOPT; }
    inline bool resynced() { return m_sol != Utils::ResyncResolution::R_UNRESOLVED; }

    template <typename Fn>
    inline void foreachVertex(Fn&& fn) {
        using Traits = FunctionTraits<Fn>;
        using Arg = typename Traits::template arg<0>::type;
        for (V3GraphVertex *vtxp = verticesBeginp(), *nextp; vtxp; vtxp = nextp) {
            nextp = vtxp->verticesNextp();
            //UASSERT(dynamic_cast<ResyncVertex*>(vtxp), "baf vertex type");
            if (Arg const vp = dynamic_cast<Arg>(vtxp)) { fn(vp); }
        }
    }
};

class ResyncVertex VL_NOT_FINAL : public V3GraphVertex {
private:
    const uint32_t m_cost;

protected:
    ResyncVertex(ResyncGraph* graphp, const uint32_t cost)
        : V3GraphVertex{graphp}
        , m_cost{cost} {}
    ~ResyncVertex() override = default;

public:
    uint32_t cost() const { return m_cost; }
    virtual ResyncVertex* clone(ResyncGraph* graphp) const = 0;

    template <typename Fn>
    inline void foreachEdge(Fn&& fn, GraphWay way);
    template <typename Fn>
    inline void foreachOutEdge(Fn&& fn) {
        foreachEdge(fn, GraphWay::FORWARD);
    }
    template <typename Fn>
    inline void foreachInEdge(Fn&& fn) {
        foreachEdge(fn, GraphWay::REVERSE);
    }
};

class LogicVertex VL_NOT_FINAL : public ResyncVertex {
public:
    LogicVertex(ResyncGraph* graphp, const uint32_t cost)
        : ResyncVertex{graphp, cost} {}
};

class CombVertex final : public LogicVertex {
private:
    LogicWithActive const m_logicp;

public:
    CombVertex(ResyncGraph* graphp, const LogicWithActive& logicp, const uint32_t cost)
        : LogicVertex{graphp, cost}
        , m_logicp{logicp} {}
    string name() const override { return "COMB " + cvtToStr(cost()); }
    string dotShape() const override { return "oval"; }
    LogicWithActive logicp() const { return m_logicp; }

    CombVertex* clone(ResyncGraph* graphp) const override {
        return new CombVertex{graphp, logicp(), cost()};
    }
};

class SeqVertex final : public LogicVertex {
private:
    std::multimap<ResyncGraph*, SeqReadVertex*> m_consumersp;
    AstSenTree* const m_sentreep;
    const std::vector<LogicWithActive> m_logicsp;
    const std::vector<AstVarScope*> m_lvsp;
    bool m_unopt = false;

public:
    SeqVertex(ResyncGraph* graphp, const uint32_t cost, AstSenTree* sentreep,
              const std::vector<LogicWithActive>& logicsp, const std::vector<AstVarScope*>& lvsp)
        : LogicVertex{graphp, cost}
        , m_sentreep{sentreep}
        , m_logicsp{logicsp}
        , m_lvsp{lvsp} {}

    string name() const override { return "SEQ " + cvtToStr(cost()); }
    string dotShape() const override { return "rect"; }

    const std::vector<LogicWithActive>& logicsp() const { return m_logicsp; }
    const std::vector<AstVarScope*>& lvsp() const { return m_lvsp; }
    std::multimap<ResyncGraph*, SeqReadVertex*>& consumersp() { return m_consumersp; }
    AstSenTree* sentreep() const { return m_sentreep; }
    bool unopt() const { return m_unopt; }
    void unopt(bool b) { m_unopt = b; }

    SeqVertex* clone(ResyncGraph* graphp) const override {
        return new SeqVertex{graphp, cost(), sentreep(), logicsp(), lvsp()};
    }
};

class VarReadVertex VL_NOT_FINAL : public ResyncVertex {
protected:
    AstVarScope* const m_vscp;

public:
    VarReadVertex(ResyncGraph* graphp, AstVarScope* const vscp)
        : ResyncVertex{graphp, 0}
        , m_vscp{vscp} {}
    string dotShape() const override { return "invhous"; }
    string name() const override { return m_vscp->prettyName(); }
    AstVarScope* vscp() const { return m_vscp; }
};

class SeqReadVertex final : public VarReadVertex {
private:
    SeqVertex* const m_writerp;

public:
    SeqReadVertex(ResyncGraph* graphp, AstVarScope* const vscp, SeqVertex* writerp)
        : VarReadVertex{graphp, vscp}
        , m_writerp{writerp} {}
    string dotColor() const override { return "brown"; }
    string name() const override { return "SeqRead " + VarReadVertex::name(); }
    SeqVertex* writerp() const { return m_writerp; }

    SeqReadVertex* clone(ResyncGraph* graphp) const override {
        return new SeqReadVertex{graphp, vscp(), writerp()};
    }
};

class CombSeqReadVertex final : public VarReadVertex {
private:
    AstSenTree* const m_sentreep;

public:
    CombSeqReadVertex(ResyncGraph* graphp, AstVarScope* const vscp, AstSenTree* const sentreep)
        : VarReadVertex{graphp, vscp}
        , m_sentreep{sentreep} {}
    string dotColor() const override { return "red"; }
    string name() const override { return "CombSeqRead " + VarReadVertex::name(); }
    AstSenTree* sentreep() const { return m_sentreep; }
    CombSeqReadVertex* clone(ResyncGraph* graphp) const override {
        return new CombSeqReadVertex{graphp, vscp(), sentreep()};
    }
};

class ProxyVertex VL_NOT_FINAL : public ResyncVertex {
public:
    ProxyVertex(ResyncGraph* graphp, uint32_t cost)
        : ResyncVertex{graphp, cost} {}
    string dotColor() const override { return "orange"; }
    string dotShape() const override { return "hexagon"; }
};
class CombSeqVertex final : public ProxyVertex {
private:
    AstSenTree* const m_sentreep;
    AstVarScope* const m_vscp;

public:
    CombSeqVertex(ResyncGraph* graphp, AstVarScope* vscp, AstSenTree* sentreep)
        : ProxyVertex{graphp, 0}
        , m_sentreep{sentreep}
        , m_vscp{vscp} {}
    AstVarScope* vscp() const { return m_vscp; }
    AstSenTree* sentreep() const { return m_sentreep; }
    CombSeqVertex* clone(ResyncGraph* graphp) const override {
        return new CombSeqVertex{graphp, vscp(), sentreep()};
    }
    string name() const override { return "CombSeq " + vscp()->prettyName(); }
};
class SeqCombVertex final : public ProxyVertex {
private:
    AstSenTree* const m_sentreep;
    const std::vector<LogicWithActive> m_logicsp;
    const std::vector<AstVarScope*> m_lvsp;

public:
    SeqCombVertex(ResyncGraph* graphp, const uint32_t cost, AstSenTree* sentreep,
                  const std::vector<LogicWithActive>& logicsp,
                  const std::vector<AstVarScope*>& lvsp)
        : ProxyVertex{graphp, cost}
        , m_sentreep{sentreep}
        , m_logicsp{logicsp}
        , m_lvsp{lvsp} {}

    const std::vector<LogicWithActive>& logicsp() const { return m_logicsp; }
    const std::vector<AstVarScope*>& lvsp() const { return m_lvsp; }
    AstSenTree* sentreep() const { return m_sentreep; }

    SeqCombVertex* clone(ResyncGraph* graphp) const override {
        return new SeqCombVertex{graphp, cost(), sentreep(), logicsp(), lvsp()};
    }

    string name() const override { return "SeqComb " + cvtToStr(cost()); }
};

class CombCombVertex final : public ProxyVertex {
private:
    LogicWithActive const m_logicp;
    AstSenTree* const m_sentreep;

public:
    CombCombVertex(ResyncGraph* graphp, const LogicWithActive& logicp, AstSenTree* sentreep,
                   const uint32_t cost)
        : ProxyVertex{graphp, cost}
        , m_logicp{logicp}
        , m_sentreep{sentreep} {}
    LogicWithActive logicp() const { return m_logicp; }
    AstSenTree* sentreep() const { return m_sentreep; }
    CombCombVertex* clone(ResyncGraph* graphp) const override {
        return new CombCombVertex{graphp, logicp(), sentreep(), cost()};
    }
    string name() const override {
        return "CombComb " + cvtToStr(cost());
    }
};

class ResyncEdge final : public V3GraphEdge {
private:
    AstVarScope* const m_vscp = nullptr;

public:
    ResyncEdge(ResyncGraph* graphp, ResyncVertex* fromp, ResyncVertex* top,
               AstVarScope* const vscp)
        : V3GraphEdge{graphp, fromp, top, 1, true}
        , m_vscp{vscp} {}
    string dotLabel() const override final { return m_vscp ? m_vscp->prettyName() : ""; }
    AstVarScope* vscp() const { return m_vscp; }
};
inline ResyncEdge* ResyncGraph::addEdge(ResyncVertex* fromp, ResyncVertex* top,
                                        AstVarScope* vscp) {
    return new ResyncEdge{this, fromp, top, vscp};
}

inline bool Utils::Key::operator<(const Utils::Key& other) const {
    return graphp->cost() < other.graphp->cost();
}

inline void Utils::Key::increase(const uint32_t v) const {
    UASSERT(graphp->cost() < v, "expected increase");
    graphp->cost(v);
}

template <typename Fn>
inline void ResyncVertex::foreachEdge(Fn&& fn, GraphWay way) {
    using Traits = FunctionTraits<Fn>;
    using Arg = typename Traits::template arg<0>::type;
    using ArgNoPtr = typename std::remove_pointer<Arg>::type;
    using ArgNoPtrCv = typename std::remove_cv<ArgNoPtr>::type;
    static_assert(vlstd::is_invocable<Fn, ArgNoPtrCv*>::value
                      && std::is_base_of<ResyncEdge, ArgNoPtrCv>::value,
                  "Callable 'Fn' does not have the signature Fn(ResyncEdge*)");
    for (V3GraphEdge *edgep = beginp(way), *nextp; edgep; edgep = nextp) {
        nextp = edgep->nextp(way);
        ResyncEdge* redgep = dynamic_cast<ResyncEdge*>(edgep);
        UASSERT(redgep, "invalid edge type");
        fn(redgep);
    }
}

}  // namespace Resync
}  // namespace V3BspSched

#endif