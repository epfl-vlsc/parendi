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

#include "config_build.h"
#include "verilatedos.h"

#include "V3BspGraph.h"

#include "V3AstUserAllocator.h"
#include "V3File.h"
#include "V3Hasher.h"
#include "V3Stats.h"

VL_DEFINE_DEBUG_FUNCTIONS;
namespace V3BspSched {

namespace {

//==============================================================================
// Builder class for ConstrVertex, attached to AstVarScope user in the visitor
// see below
class ConstrBuilder final {
public:
    enum class Type : uint8_t { INIT = 0, DEF = 1, COMMIT = 2, POST = 3 };

private:
    ConstrInitVertex* m_initp = nullptr;
    ConstrDefVertex* m_defp = nullptr;
    ConstrCommitVertex* m_commitp = nullptr;
    ConstrPostVertex* m_postp = nullptr;

public:
    ConstrVertex* get(DepGraph* graphp, AstVarScope* vscp, Type tpe) {
        ConstrVertex* res = nullptr;
        switch (tpe) {
        case Type::INIT:
            if (!m_initp) m_initp = new ConstrInitVertex{graphp, vscp};
            res = m_initp;
            break;
        case Type::DEF:
            if (!m_defp) m_defp = new ConstrDefVertex{graphp, vscp};
            res = m_defp;
            break;
        case Type::COMMIT:
            if (!m_commitp) m_commitp = new ConstrCommitVertex{graphp, vscp};
            res = m_commitp;
            break;
        case Type::POST:
            if (!m_postp) m_postp = new ConstrPostVertex{graphp, vscp};
            res = m_postp;
            break;
        default: vscp->v3fatal("Internal: bad vertex type request!"); break;
        }
        return res;
    }
    ConstrBuilder()
        : m_initp{nullptr}
        , m_defp{nullptr}
        , m_commitp{nullptr} {}
    ~ConstrBuilder() = default;
};

class DepGraphBuilderImpl final : public VNVisitor {
private:
    // type of var refernce, whether used or defined
    enum RefType : uint8_t { VR_USE = 0x1, VR_DEF = 0x2 };
    // NODE STATE
    //  AstVarScope::user1    -> ConstrBuilder object
    //  AstVarScope::user2    -> RefType within each logic block, reset on logic
    const VNUser1InUse user1InUse;
    const VNUser2InUse user2InUse;
    AstUser1Allocator<AstVarScope, ConstrBuilder> m_builderUser;

    bool m_inClocked = false;  // in a clocked active
    bool m_inPre = false;  // under AssignPre
    bool m_inPost = false;  // undre AssignPost
    AstScope* m_scopep = nullptr;  // enclosing scope
    AstSenTree* m_domainp = nullptr;  // enclosing domain, nullptr means comb logic
    CompVertex* m_logicVtx = nullptr;  // enclosing logic vertex
    DepGraph* m_graphp = nullptr;

    // create a Compertex for logic node and iterate children to connect the
    // to the corresponding ConstrVertex objects.
    void iterateLogic(AstNode* nodep) {
        UASSERT_OBJ(!m_logicVtx, nodep, "Nesting logic?");
        // Reset the usage
        AstNode::user2ClearTree();
        m_logicVtx = new CompVertex{m_graphp, m_scopep, nodep, m_domainp};
        V3Stats::addStatSum("BspGraph, Computation nodes", 1);
        iterateChildren(nodep);
        m_logicVtx = nullptr;
    }

    // VISITORS
    void visit(AstActive* nodep) override {
        // assertions borrowed from V3Order::OrderBuildVisitor
        UASSERT_OBJ(!nodep->sensesStorep(), nodep,
                    "AstSenTrees should have been made global in V3ActiveTop");
        UASSERT_OBJ(m_scopep, nodep, "AstActive not under AstScope");
        UASSERT_OBJ(!m_logicVtx, nodep, "AstActive under logic");
        UASSERT_OBJ(!m_inClocked && !m_domainp, nodep, "Should not nest");

        if (nodep->sensesp()->hasHybrid()) {
            nodep->v3warn(
                E_UNSUPPORTED,
                "hybrid logic detected, poplar backend is only capable of simple clocking");
        }

        m_domainp = nullptr;  // nullptr if only combinational
        m_inClocked = false;

        if (!nodep->sensesp()->hasCombo() && !nodep->sensesp()->hasHybrid()) {
            m_domainp = nodep->sensesp();
            m_inClocked = m_domainp->hasClocked();
            UASSERT_OBJ(m_domainp->hasClocked(), nodep, "Unexpected sense type");
        }

        iterateChildren(nodep);
        m_inClocked = false;
        m_domainp = nullptr;
    }

    void visit(AstVarRef* nodep) override {

        UASSERT_OBJ(m_scopep, nodep, "AstVarRef requires a scope");
        UASSERT_OBJ(m_logicVtx, nodep, "CompVertex not allocated!");
        AstVarScope* const vscp = nodep->varScopep();
        UASSERT_OBJ(vscp, nodep, "Expected valid VarScope pointer");
        auto getVtx = [this, vscp](ConstrBuilder::Type tpe) {
            return m_builderUser(vscp).get(m_graphp, vscp, tpe);
        };
        const bool alreadyDefined
            = vscp->user2() & VR_DEF;  // node was previously defined in the same logic block
        const bool alreadyUsed
            = vscp->user2() & VR_USE;  // node was previously used in the same logic block

        // only consider adding edges if not already added
        const bool firstDef = !alreadyDefined && nodep->access().isWriteOrRW();
        if (firstDef) {
            // notify next statements that we created the necessary edge(s)
            vscp->user2(vscp->user2() | VR_DEF);
            if (!m_inClocked) {
                // combinational logic and post assignments require and edge from
                // a DEF constraint to the current logic vertex
                ConstrVertex* defp = getVtx(ConstrBuilder::Type::DEF);
                m_graphp->addEdge(m_logicVtx, defp);
                // DEF constraints only exist between combinational logic
            } else if (m_inPost) {
                // AssignPost comes after all the commits by clocked logic
                ConstrVertex* commitp = getVtx(ConstrBuilder::Type::COMMIT);
                m_graphp->addEdge(commitp, m_logicVtx);
                // this is different from V3Order since we are trying to schedule
                // everything before the post assignments
                ConstrVertex* postp = getVtx(ConstrBuilder::Type::POST);
                m_graphp->addEdge(postp, m_logicVtx);
            } else if (m_inPre) {
                // creat both DEF and INIT constraints. The former may not be necessary though
                // since it should be generated by downstream clocked blocks but we add it anyways.
                ConstrVertex* defp = getVtx(ConstrBuilder::Type::DEF);
                m_graphp->addEdge(m_logicVtx, defp);
                ConstrVertex* initp = getVtx(ConstrBuilder::Type::INIT);
                m_graphp->addEdge(m_logicVtx, initp);
            } else {
                // clocked logic
                // INIT -> logic -> COMMIT
                // make sure logic comes after INIT (i.e., AssignPre)
                ConstrVertex* initp = getVtx(ConstrBuilder::Type::INIT);
                m_graphp->addEdge(initp, m_logicVtx);
                ConstrVertex* commitp = getVtx(ConstrBuilder::Type::COMMIT);
                m_graphp->addEdge(m_logicVtx, commitp);
            }
        }
        bool firstUse = !alreadyUsed && nodep->access().isReadOrRW();
        /* in case we have a comb like the following:
         *    a = something
         *    if (c) a = f(a);
         * We consider the variable not to be used in the global sense, since
         * it's defined locally again.
         * We can not have something like:
         *    if (c) a = f(a)
         * or
         *    if (c) a = something;
         *    a = f(a);
         * Without a previous assignment becuase that's basically a comb cycle or a latch.
         * In the first case, we should not add an edge from a DEF to the current
         * logic since it would artificially create a cycle in the dependence graph.
         *
         * Note that such definitions and uses are fine with clocked logic since
         * with a clocked definition, we do not create a DEF -> logic edge, rather
         * we create a INIT -> logic vertex. This is due to the parallel nature of
         * clocked blocks.
         * However, we have to create DEF -> logic edges for comb logic since they
         * refelect the read-after-write dependencies in the static schedule.
         */
        if (!m_inClocked && alreadyDefined) { firstUse = false; }

        if (firstUse) {
            // notify next iterations that edgs have been created
            vscp->user2(vscp->user2() | VR_USE);
            if (!m_inClocked) {
                // combinational logic, all uses should be before INIT, i.e., before
                // clocked logic that may define the variable
                ConstrVertex* initp = getVtx(ConstrBuilder::Type::INIT);
                m_graphp->addEdge(m_logicVtx, initp);
                // also add an edge from potential DEFs
                ConstrVertex* defp = getVtx(ConstrBuilder::Type::DEF);
                m_graphp->addEdge(defp, m_logicVtx);
                // note that the defp may not have any predessor in case it
                // is driven by sequential logic.
                ConstrVertex* postp = getVtx(ConstrBuilder::Type::POST);
                m_graphp->addEdge(m_logicVtx, postp);
            } else if (m_inPost) {
                // should come after commits
                ConstrVertex* commitp = getVtx(ConstrBuilder::Type::COMMIT);
                m_graphp->addEdge(commitp, m_logicVtx);
                // do we need DEF->logic constraints as well? Probably not since
                // the LHS of AssignPost or AlwaysPost should come from clocked
                // logic
                ConstrVertex* defp = getVtx(ConstrBuilder::Type::DEF);
                m_graphp->addEdge(defp, m_logicVtx);
            } else if (m_inPre) {
                ConstrVertex* defp = getVtx(ConstrBuilder::Type::DEF);
                m_graphp->addEdge(defp, m_logicVtx);  // not really necessary
            } else {
                // clocked logic
                ConstrVertex* defp = getVtx(ConstrBuilder::Type::DEF);
                m_graphp->addEdge(defp, m_logicVtx);

                ConstrVertex* postp = getVtx(ConstrBuilder::Type::POST);
                m_graphp->addEdge(m_logicVtx, postp);
            }
        }
    }
    // unexpected nodes
    void visit(AstNodeVarRef* nodep) override {
        nodep->v3fatalSrc("I only know how to handle AstVarRef");
    }
    void visit(AstInitial* nodep) override {
        nodep->v3fatalSrc("AstInitial should not need a dependence graph");
    }
    void visit(AstFinal* nodep) override {
        nodep->v3fatalSrc("AstFinal should not need a dependence graph");
    }
    void visit(AstInitialStatic* nodep) override {
        nodep->v3fatalSrc("AstInitialStatic does not need a dependence graph");
    }
    void visit(AstCCall* nodep) override { iterateChildren(nodep); }
    void visit(AstInitialAutomatic* nodep) override {
        nodep->v3fatalSrc("AstInitialAutomatic is not handled yet!");
    }
    void visit(AstAlwaysObserved* nodep) override {
        nodep->v3fatalSrc("AstAlwaysObserved not handled yet!");
    }
    void visit(AstAlwaysReactive* nodep) override {
        nodep->v3fatalSrc("AstAlwaysReactive not handled!");
    }
    void visit(AstCFunc* nodep) override {
        nodep->v3fatalSrc("Don't know what to do with AstCFunc");
    }
    // logic blocks
    void visit(AstAlways* nodep) override { iterateLogic(nodep); }
    void visit(/*Post assignment for memories*/ AstAlwaysPost* nodep) override {
        m_inPost = true;
        iterateLogic(nodep);
        m_inPost = false;
    }

    // singleton logic
    void visit(AstAssignPost* nodep) override {
        m_inPost = true;
        iterateLogic(nodep);
        m_inPost = false;
    }
    void visit(AstAssignPre* nodep) override {
        m_inPre = true;
        iterateLogic(nodep);
        m_inPre = false;
    }
    void visit(AstAssignAlias* nodep) override { iterateLogic(nodep); }
    void visit(AstAssignW* nodep) override { iterateLogic(nodep); }

    // "I don't know what these are category"
    void visit(AstAlwaysPublic* nodep) override { nodep->v3fatalSrc("Unkown node"); }
    void visit(AstCoverToggle* nodep) override { nodep->v3fatalSrc("Unknown node"); }
    // nodes to bypass
    void visit(AstVarScope* nodep) override {}
    void visit(AstCell*) override {}  // Only interested in the respective AstScope
    void visit(AstTypeTable*) override {}
    void visit(AstConstPool*) override {}
    void visit(AstClass*) override {}
    // default
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    DepGraph* graphp() const { return m_graphp; }
    explicit DepGraphBuilderImpl(const V3Sched::LogicByScope& logics) {
        m_graphp = new DepGraph;
        for (const auto& p : logics) {
            m_scopep = p.first;
            iterate(p.second);
            m_scopep = nullptr;
        }
    }
};

std::unique_ptr<DepGraph> backwardTraverseAndCollect(const std::unique_ptr<DepGraph>& graphp,
                                                     const std::vector<AnyVertex*>& postp) {
    // STATE
    //  AnyVertex::userp()  -> non nullptr, pointer to the clone
    //  AnyVertex::user()   -> vertex visited
    //  DepEdge::user()     -> true if cloned
    //
    graphp->userClearVertices();
    // the graph that is built during the backward traversal which is essentially
    // the bsp partition
    std::unique_ptr<DepGraph> builderp{new DepGraph};
    std::queue<AnyVertex*> toVisit;
    for (AnyVertex* vp : postp) {
        vp->user(1);
        toVisit.push(vp);
    }
    // bfs-like, collect all reachable vertices from the postp collection
    std::vector<AnyVertex*> visited;
    while (!toVisit.empty()) {
        AnyVertex* const headp = toVisit.front();
        toVisit.pop();
        visited.push_back(headp);
        if (dynamic_cast<ConstrPostVertex* const>(headp)) {
            continue;  // do not follow
        }
        for (auto itp = headp->inBeginp(); itp; itp = itp->inNextp()) {
            AnyVertex* const fromp = static_cast<AnyVertex* const>(itp->fromp());
            if (!fromp->user()) {
                fromp->user(1);
                toVisit.push(fromp);
            }
        }
    }

    graphp->userClearVertices();
    // clone vertices collected in the traversal
    for (AnyVertex* const vtxp : visited) {
        UASSERT(!vtxp->user(), "invalid user value, double counting a vertex?");
        AnyVertex* const clonep = vtxp->clone(builderp.get());
        clonep->userp(vtxp);
        vtxp->userp(clonep);
    }

    // clone immediate successors of the collected vertices
    for (AnyVertex* const vtxp : visited) {
        CompVertex* const compp = dynamic_cast<CompVertex* const>(vtxp);
        if (!compp) { continue; /* not a CompVertex */ }
        // Special handling of the ComputeVertex: make sure all successors
        // (i.e., DefConstr, CommitConstr, or PostConstr) vertices are also added
        /// to the partition.
        // Note that the CommitConstr nodes are added from the disjoint sets but
        // the DefConstr nodes maybe lost if we do no add them here when the
        // lifetime of a variable is limited to the always_comb block where it
        // is produced:
        // always_comb begin
        //       x = fn(y);
        //       z = fn(x); // last use of x
        // end
        // will result in a DefConstr(x) node that is a sink and hence maynot
        // be added to the partition (i.e., has not been cloned yet)
        for (V3GraphEdge* eitp = compp->outBeginp(); eitp; eitp = eitp->outNextp()) {
            if (eitp->top()->userp()) {
                // already part of the partition
                continue;
            }
            ConstrVertex* const oldTop = dynamic_cast<ConstrVertex* const>(eitp->top());
            UASSERT(oldTop, "expected none-null");
            AnyVertex* const clonep = oldTop->clone(builderp.get());
            clonep->userp(oldTop);
            oldTop->userp(clonep);
        }
    }

    graphp->userClearEdges();

    for (V3GraphVertex* itp = builderp->verticesBeginp(); itp; itp = itp->verticesNextp()) {
        AnyVertex* origp = reinterpret_cast<AnyVertex*>(itp->userp());
        UASSERT_OBJ(origp, itp, "expected original vertex pointer");
        for (V3GraphEdge* eitp = origp->inBeginp(); eitp; eitp = eitp->inNextp()) {
            if (eitp->user()) { continue; /*already processed*/ }
            if (!eitp->fromp()->userp()) { continue; /*not part of the partition*/ }
            auto newFromp = reinterpret_cast<AnyVertex*>(eitp->fromp()->userp());
            auto fromCompp = dynamic_cast<CompVertex*>(newFromp);
            auto fromConstrp = dynamic_cast<ConstrVertex*>(newFromp);
            auto toCompp = dynamic_cast<CompVertex*>(itp);
            auto toConstrp = dynamic_cast<ConstrVertex*>(itp);
            if (fromCompp && toConstrp) {
                builderp->addEdge(fromCompp, toConstrp);
            } else if (fromConstrp && toCompp) {
                builderp->addEdge(fromConstrp, toCompp);
            } else {
                UASSERT(false, "invalid pointer types!");
            }
            eitp->user(1);
        }
    }

    return builderp;
}

//==============================================================================
// Data structure for creating disjoint sets, not very optimized for performance..
template <class Key, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>,
          class Allocator = std::allocator<Key>>
class DisjointSets {
private:
    using SetType = std::unordered_set<Key, Hash, KeyEqual, Allocator>;
    using MapType = std::unordered_map<Key, SetType, Hash, KeyEqual, Allocator>;
    using LookupType = std::unordered_map<Key, Key, Hash, KeyEqual, Allocator>;

    MapType m_sets;
    LookupType m_rep;
    SetType& makeUnion__(const Key& k1, const Key& k2) {
        auto r1 = m_rep[k1], r2 = m_rep[k2];
        if (r1 == r2) { return m_sets[r1]; }
        SetType &s1 = m_sets[r1], s2 = m_sets[r2];
        for (const auto& e : s2) { s1.insert(e); }
        m_sets.erase(r2);
        m_rep[k2] = r1;
        return s1;
    }

public:
    DisjointSets() {}
    ~DisjointSets() = default;

    // SetType& find(const Key& k) { return m_sets[m_rep[k]]; }
    bool contains(const Key& k) { return m_rep.find(k) != m_rep.end(); }
    SetType& makeSet(const Key& k) {
        auto it = m_rep.find(k);
        if (it == m_rep.end()) {
            m_rep.emplace(k, k);
            m_sets.emplace(k, SetType{k});
        }
        return m_sets[m_rep[k]];
    }

    SetType& makeUnion(const Key& k1, const Key& k2) {
        if (!contains(k1)) makeSet(k1);
        if (!contains(k2)) makeSet(k2);
        auto s1 = m_sets[m_rep[k1]].size(), s2 = m_sets[m_rep[k2]].size();
        if (s1 < s2) {
            return makeUnion__(k1, k2);
        } else {
            return makeUnion__(k2, k1);
        }
    }
    MapType& sets() { return m_sets; }
};

std::vector<std::vector<AnyVertex*>> groupCommits(const std::unique_ptr<DepGraph>& graphp) {

    // We need to group all the computation described by the graph based on the
    // values they commit. ConstrCommitVertex nodes cannot be replicated and
    // any compute node adjacent to them should also be singular. E.g.,
    // If commit x1 and commit x2 share an immediate neighbor compute v1 (can be
    // Always/AlwaysPost/AssignPost) then we make sure that v1 is never replicated and is placed on
    // the same partition as x1 and x2. If another immediate neighbor like compute v2 also exist,
    // then that one also goes to the same partition. This ensure that values
    // are never computed (or commited) multiple times and also side-effects
    // only appear once (as they should). However, in general this approach in conservative
    // and may limit parallelism. E.g., if we have
    // alway_ff @(posedge clock) begin: v1
    //    x1 = expr1(z)
    //    x2 = expr2(y)
    // end
    // It might be more efficient to execute the two lines in parallel (if independent)
    // but we don't. Essentially other passes should try to break always blocks into
    // smaller pieces to increase parallelism.
    // There might be Always node in the graph that are not connected to any
    // commit nodes. We have to also form partitions for them:
    // always_ff @(posedge clock) $display("value of t is %d", t);
    // These nodes also should not and cannot be replicated.

    // hash all the vertices, needed to have stable results between runs
    V3Hasher nodeHasher;
    for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
        V3Hash hash;
        if (auto compp = dynamic_cast<CompVertex* const>(vtxp)) {
            hash += "COMP";
            hash += nodeHasher(compp->nodep());
            if (compp->domainp()) { hash += nodeHasher(compp->domainp()); }
            compp->hash(hash);
        } else if (auto constrp = dynamic_cast<ConstrVertex* const>(vtxp)) {
            hash += constrp->nameSuffix();
            hash += nodeHasher(constrp->vscp());
            constrp->hash(hash);
        } else {
            UASSERT(false, "invalid vertex type");
        }
    }
    // STATE
    // AstVarScope::user1p()  -> pointer to ConstrDefVertex
    VNUser1InUse m_user1InUse;
    for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
        if (auto defp = dynamic_cast<ConstrDefVertex*>(vtxp)) {
            if (VN_IS(defp->vscp()->varp()->dtypep(), UnpackArrayDType)) {

                UINFO(3, "visited unpack variable " << defp->vscp()->name() << endl);
                defp->vscp()->user1u(VNUser{defp});
            }
        }
    }

    DisjointSets<AnyVertex*> sets{};
    std::vector<ConstrCommitVertex*> allCommitsp;

    auto isSinkComp = [](CompVertex* const compp) -> bool {
        if (!VN_IS(compp->nodep(), Always)) { return false; }
        bool hasNoDataDef = true;
        for (V3GraphEdge* outp = compp->outBeginp(); outp; outp = outp->outNextp()) {
            UASSERT(!dynamic_cast<ConstrInitVertex*>(outp->top()), "INIT node not expected!");
            if (dynamic_cast<ConstrCommitVertex*>(outp->top())
                || dynamic_cast<ConstrDefVertex*>(outp->top())) {
                hasNoDataDef = false;
                break;
            }
        }
        // if condition is still true then:
        // This always block does not define anything (either sequential or
        // combinationally) hence may contain DPI/PLI side-effects and can not be
        // replicated. All successor of this node are in fact simple ConstrPostVertex nodes
        // that only enforce ordering.
        return hasNoDataDef;
    };
    for (auto* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
        if (auto commitp = dynamic_cast<ConstrCommitVertex*>(vtxp)) {
            sets.makeSet(commitp);
            allCommitsp.push_back(commitp);
        } else if (auto compp = dynamic_cast<CompVertex*>(vtxp)) {
            if (isSinkComp(compp)) { sets.makeSet(compp); }
        }
    }

    // from every commit node, find the other commit nodes that are only apart
    // by two edges in the reverse direction, i.e., not siblings

    auto visitNeighbors = [&sets](ConstrCommitVertex* commitp, GraphWay way) {
        bool forward = way.forward();
        for (V3GraphEdge* edgep = commitp->beginp(way); edgep; edgep = edgep->nextp(way)) {
            CompVertex* const compp = dynamic_cast<CompVertex*>(edgep->furtherp(way));
            UASSERT(compp, "expected compute node");
            UASSERT(VN_IS(compp->nodep(), Always) || VN_IS(compp->nodep(), AlwaysPost)
                        || VN_IS(compp->nodep(), AssignPost),
                    "malformed graph?");
            sets.makeUnion(compp, commitp);
            for (V3GraphEdge* fEdgep = compp->beginp(way.invert()); fEdgep;
                 fEdgep = fEdgep->nextp(way.invert())) {
                ConstrCommitVertex* otherp
                    = dynamic_cast<ConstrCommitVertex*>(fEdgep->furtherp(way.invert()));
                if (!otherp || otherp == commitp) continue;
                sets.makeUnion(commitp, otherp);
            }
        }
    };
    // if a commit vertex has an underlying UnpackArrayDType, then we should also
    // find any commit or compute sink node that is reachable from the the upack variable's
    // ConstrDefNode
    auto visitReachableFromCorrespondingDef
        = [&sets, &isSinkComp, &graphp](ConstrCommitVertex* const commitp) {
              if (!VN_IS(commitp->vscp()->varp()->dtypep(), UnpackArrayDType)) { return; }
              auto defp = dynamic_cast<ConstrDefVertex*>(commitp->vscp()->user1u().toGraphVertex());
              UASSERT_OBJ(defp, commitp->vscp(), "not all unpack variables are visited?" << commitp->vscp()->user1p() << endl);
              graphp->userClearVertices();
              std::queue<AnyVertex*> toVisit;
              toVisit.push(defp);
              defp->user(1);  // mark
              while (!toVisit.empty()) {
                  AnyVertex* const headp = toVisit.front();
                  toVisit.pop();
                  if (ConstrCommitVertex* const otherp
                      = dynamic_cast<ConstrCommitVertex* const>(headp)) {
                      sets.makeUnion(commitp, otherp);
                  } else if (CompVertex* const compp = dynamic_cast<CompVertex* const>(headp)) {
                      if (isSinkComp(compp)) { sets.makeUnion(commitp, compp); }
                  }
                  // follow forward
                  for (V3GraphEdge* edgep = headp->outBeginp(); edgep; edgep = edgep->outNextp()) {
                      if (!edgep->top()->user()) {
                          AnyVertex* const top = dynamic_cast<AnyVertex* const>(edgep->top());
                          UASSERT(top, "invalid vertex type?");
                          toVisit.push(top);
                          top->user(1);
                      }
                  }
              }
          };
    for (ConstrCommitVertex* commitp : allCommitsp) {

        auto forward = GraphWay{GraphWay::FORWARD};
        visitNeighbors(commitp, forward);
        visitNeighbors(commitp, forward.invert());
        visitReachableFromCorrespondingDef(commitp);
    }

    std::vector<std::vector<AnyVertex*>> disjointSinks;
    for (const auto& pair : sets.sets()) {
        if (pair.second.empty()) continue;
        disjointSinks.push_back({});
        for (const auto& s : pair.second) { disjointSinks.back().push_back(s); }
    }

    if (dump() > 0) {
        const std::string filename = v3Global.debugFilename("disjoint") + ".txt";
        const std::unique_ptr<std::ofstream> logp{V3File::new_ofstream(filename)};
        if (logp->fail()) v3fatal("Cannot write " << filename);
        for (const auto& pair : sets.sets()) {
            *logp << "{" << std::endl;
            for (const auto& s : pair.second) { *logp << "\t\t" << s << std::endl; }
            *logp << "}" << std::endl << std::endl;
        }
    }
    V3Stats::addStat("BspGraph, Independent processes", disjointSinks.size());
    return disjointSinks;
}

};  // namespace
std::unique_ptr<DepGraph> DepGraphBuilder::build(const V3Sched::LogicByScope& logics) {

    return std::unique_ptr<DepGraph>{DepGraphBuilderImpl{logics}.graphp()};
}

std::vector<std::unique_ptr<DepGraph>>
DepGraphBuilder::splitIndependent(const std::unique_ptr<DepGraph>& graphp) {

    auto groups = groupCommits(graphp); /*groups vertices that must go to the same partition*/
    std::vector<std::unique_ptr<DepGraph>> partitionsp;
    for (const auto group : groups) {
        partitionsp.emplace_back(backwardTraverseAndCollect(graphp, group));

        if (dumpGraph() > 0) {
            partitionsp.back()->dumpDotFilePrefixed("partition_"
                                                    + std::to_string(partitionsp.size() - 1));
        }
    }

    return partitionsp;
}

};  // namespace V3BspSched