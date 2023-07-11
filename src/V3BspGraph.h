// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: BSP fine-grained dependence grpah
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

#ifndef VERILATOR_V3BSPGRAPH_H_
#define VERILATOR_V3BSPGRAPH_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3Graph.h"
#include "V3Sched.h"
namespace V3BspSched {

//=============================================================================
class CompVertex;
class ConstrVertex;
class AnyVertex;
//=============================================================================
// Graph type

class DepGraph final : public V3Graph {
private:
    AstModule* m_modp = nullptr;

public:
    // METHODS
    // All edges are noncuttable, but there is never and edge between two compute vertices
    inline void addEdge(CompVertex* fromp, ConstrVertex* top);
    inline void addEdge(ConstrVertex* fromp, CompVertex* top);
    inline AstModule* modp() const { return m_modp; }
    inline void modp(AstModule* modp) { m_modp = modp; }
};

//=============================================================================
// Vertex types

// abstract vertex type, all other types are derived from BspDepVertex
class AnyVertex VL_NOT_FINAL : public V3GraphVertex {
private:
    AstSenTree* m_domainp = nullptr;
    V3Hash m_hash;

protected:
    // CONSTRUCTOR
    AnyVertex(DepGraph* graphp, AstSenTree* domainp)
        : V3GraphVertex{graphp}
        , m_domainp{domainp}
        , m_hash{} {}
    ~AnyVertex() override = default;

public:
    // METHODS
    V3Hash hash() const { return m_hash; }
    void hash(const V3Hash h) { m_hash = h; }
    bool isClocked() const { return m_domainp != nullptr; }
    AstSenTree* domainp() const { return m_domainp; }
    void domainp(AstSenTree* domainp) {
        UASSERT(!m_domainp, "Domain should only be set once");
        m_domainp = domainp;
    }

    virtual AnyVertex* clone(DepGraph* graphp) const = 0;
};

class CompVertex final : public AnyVertex {
private:
    AstNode* const m_nodep;  // the logic represented by this vertex
    AstScope* const m_scopep;  // the scope that m_nodep belongs to
    AstActive* const m_activep;  // the active aroudn the logic
public:
    CompVertex(DepGraph* graphp, AstScope* scopep, AstNode* nodep, AstSenTree* domainp,
               AstActive* activep)
        : AnyVertex{graphp, domainp}
        , m_nodep{nodep}
        , m_scopep{scopep}
        , m_activep{activep} {
        UASSERT_OBJ(scopep, nodep, "logic requires scope!");
        UASSERT(nodep, "Can not have null logic!");
    }
    ~CompVertex() override = default;

    AstNode* nodep() const { return m_nodep; }
    AstScope* scopep() const { return m_scopep; }
    AstActive* activep() const { return m_activep; }
    CompVertex* clone(DepGraph* graphp) const override {
        return new CompVertex{graphp, scopep(), nodep(), domainp(), activep()};
    }
    // LCOV_EXCL_START // Debug code
    string name() const override {
        return ((domainp() ? "@" + cvtToHex(domainp()) + "\\n" : "") + cvtToHex(m_nodep) + "\\n"
                + cvtToStr(m_nodep->typeName()) + "\\n" + cvtToStr(m_nodep->fileline()));
    }
    string dotShape() const override { return VN_IS(m_nodep, Active) ? "doubleoctogon" : "rect"; }
    // LCOV_EXCEL_STOP
};

class ConstrVertex VL_NOT_FINAL : public AnyVertex {
private:
    AstVarScope* const m_vscp;  // the variable scope that this ordering constraint references
public:
    ConstrVertex(DepGraph* graphp, AstVarScope* vscp)
        : AnyVertex{graphp, nullptr}
        , m_vscp{vscp} {}
    ~ConstrVertex() override = default;

    // ACCESSOR
    AstVarScope* vscp() const { return m_vscp; }

    // LCOV_EXCL_START // Debug code
    string dotShape() const override final { return "ellipse"; }
    virtual string nameSuffix() const = 0;
    string name() const override final {
        return cvtToHex(m_vscp) + " " + nameSuffix() + "\\n " + m_vscp->name() + "\\n";
    }
    // LCOV_EXCL_STOP
};

class ConstrInitVertex final : public ConstrVertex {
public:
    // CONSTRUCTOR
    ConstrInitVertex(DepGraph* graphp, AstVarScope* vscp)
        : ConstrVertex{graphp, vscp} {}
    ~ConstrInitVertex() override = default;
    ConstrInitVertex* clone(DepGraph* graphp) const override {
        return new ConstrInitVertex{graphp, vscp()};
    }
    // LCOV_EXCL_START // Debug code
    string nameSuffix() const override { return "INIT"; }
    string dotColor() const override { return "grey"; }
    // LCOV_EXCL_STOP
};

class ConstrDefVertex final : public ConstrVertex {
public:
    // CONSTRUCTOR
    ConstrDefVertex(DepGraph* graphp, AstVarScope* vscp)
        : ConstrVertex{graphp, vscp} {}
    ~ConstrDefVertex() override = default;
    ConstrDefVertex* clone(DepGraph* graphp) const override {
        return new ConstrDefVertex{graphp, vscp()};
    }
    // LCOV_EXCL_START // Debug code
    string nameSuffix() const override { return "DEF"; }
    string dotColor() const override { return "green"; }
    // LCOV_EXCL_STOP
};

class ConstrCommitVertex final : public ConstrVertex {
public:
    // CONSTRUCTOR
    ConstrCommitVertex(DepGraph* graphp, AstVarScope* vscp)
        : ConstrVertex{graphp, vscp} {}
    ~ConstrCommitVertex() override = default;
    ConstrCommitVertex* clone(DepGraph* graphp) const override {
        return new ConstrCommitVertex{graphp, vscp()};
    }
    // LCOV_EXCL_START // Debug code
    string nameSuffix() const override { return "COMMIT"; }
    string dotColor() const override { return "red"; }
    // LCOV_EXCL_STOP
};

class ConstrPostVertex final : public ConstrVertex {
public:
    // CONSTRUCTOR
    ConstrPostVertex(DepGraph* graphp, AstVarScope* vscp)
        : ConstrVertex{graphp, vscp} {}
    ~ConstrPostVertex() override = default;
    ConstrPostVertex* clone(DepGraph* graphp) const override {
        return new ConstrPostVertex{graphp, vscp()};
    }
    // LCOV_EXCL_START // Debug code
    string nameSuffix() const override { return "POST"; }
    string dotColor() const override { return "grey"; }
    // LCOV_EXCL_STOP
};

//==============================================================================
// edge type
class DepEdge final : public V3GraphEdge {
    friend class DepGraph;

private:
    DepEdge(DepGraph* graphp, AnyVertex* fromp, AnyVertex* top)
        : V3GraphEdge{graphp, fromp, top, 1, false /*not cuttable*/} {}
    ~DepEdge() override = default;

public:
    string dotColor() const override { return "red"; }
};

void DepGraph::addEdge(CompVertex* fromp, ConstrVertex* top) { new DepEdge{this, fromp, top}; }
void DepGraph::addEdge(ConstrVertex* fromp, CompVertex* top) { new DepEdge{this, fromp, top}; }

//==============================================================================
// builder class

class DepGraphBuilder final {
public:
    static std::unique_ptr<DepGraph> build(const V3Sched::LogicByScope& logics);
    static std::vector<std::unique_ptr<DepGraph>>
    splitIndependent(const std::unique_ptr<DepGraph>& graphp);
};

};  // namespace V3BspSched
template <>
struct std::hash<V3BspSched::AnyVertex*> {
    std::size_t operator()(V3BspSched::AnyVertex* const vtxp) const noexcept {
        return vtxp->hash().value();
    }
};
#endif