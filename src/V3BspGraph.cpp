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
VL_DEFINE_DEBUG_FUNCTIONS;
namespace V3BspSched {

namespace {

//==============================================================================
// Builder class for ConstrVertex, attached to AstVarScope user in the visitor
// see below
class ConstrBuilder final {
public:
    enum class Type : uint8_t { INIT = 0, DEF = 1, COMMIT = 2 };

private:
    ConstrInitVertex* m_initp = nullptr;
    ConstrDefVertex* m_defp = nullptr;
    ConstrCommitVertex* m_commitp = nullptr;

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

        if (nodep->sensesp()->forall([](AstSenItem* p) { return p->isCombo(); })) {
            m_domainp = nullptr;
            m_inClocked = false;
        } else {
            int senItemCount = 0;
            nodep->sensesp()->foreach([&senItemCount](AstSenItem* p) { senItemCount += 1; });
            if (senItemCount > 1) {
                nodep->v3warn(E_UNSUPPORTED, "Multiple sensitivities not supported");
            }
            m_domainp = nodep->sensesp();
            if (!m_domainp->sensesp()->isClocked()) {
                nodep->v3warn(E_UNSUPPORTED, "Expected clocked block");
            }
            m_inClocked = m_domainp->hasClocked();
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
                // make sure logic is ordered before post assignments
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
    void visit(AstCCall* nodep) override { nodep->v3fatalSrc("AstCCal is not handled yet!"); }
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

std::unique_ptr<DepGraph> collectBackwardsBfs(const std::unique_ptr<DepGraph>& graphp,
                                              AnyVertex* const processSink) {
    UASSERT(processSink, "require none-nullptr sink");
    // STATE
    // AnyVertex::userp() -> pointer to the clone
    // AnyVertex::user()  -> vertex is visited
    // DepEdge::user()    -> true means cloned
    graphp->userClearEdges();
    graphp->userClearVertices();
    std::unique_ptr<DepGraph> builderp{new DepGraph};
    std::queue<AnyVertex*> toVisit;

    processSink->user(1);
    toVisit.push(processSink);

    // bfs
    while (!toVisit.empty()) {
        // pop the head of the queue
        AnyVertex* const headp = toVisit.front();
        toVisit.pop();
        // add its clone to the new graph
        AnyVertex* const clonep = headp->clone(builderp.get());
        // keep references orig <-> clone
        headp->userp(clonep);
        clonep->userp(headp);
        // follow incoming edges
        for (auto itp = headp->inBeginp(); itp; itp = itp->inNextp()) {
            AnyVertex* const fromp = reinterpret_cast<AnyVertex* const>(itp->fromp());
            // fromp is a predecessor
            if (!fromp->user() /*not visited*/) {
                fromp->user(1);
                toVisit.push(fromp);
            }
        }
    }
    // clone edges
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
                // should not happen
                UASSERT(false, "invalid pointer types!");
            }
            eitp->user(1);
        }
    }
    return builderp;
}
std::unique_ptr<DepGraph> backwardTraverseAndCollect(const std::unique_ptr<DepGraph>& graphp,
                                                     const std::vector<CompVertex*>& postp) {
    // STATE
    //  AnyVertex::userp()  -> non nullptr, pointer to the clone
    //  AnyVertex::user()   -> vertex visited
    //  DepEdge::user()     -> true if cloned
    //
    graphp->userClearVertices();
    graphp->userClearEdges();
    // the graph that is built during the backward traversal which is essentially
    // the bsp partition
    std::unique_ptr<DepGraph> builderp{new DepGraph};
    std::queue<AnyVertex*> toVisit;
    for (CompVertex* vp : postp) {
        vp->user(1);
        toVisit.push(vp);
    }
    // bfs
    while (!toVisit.empty()) {

        AnyVertex* const headp = toVisit.front();
        toVisit.pop();
        // add the vertex to the partition graph, but not the edges to avoid
        // double counting
        AnyVertex* const clonep = headp->clone(builderp.get());
        // keep a reference to the original vertex to look up edges later
        headp->userp(clonep);
        clonep->userp(headp);
        for (auto itp = headp->inBeginp(); itp; itp = itp->inNextp()) {
            AnyVertex* const fromp = static_cast<AnyVertex* const>(itp->fromp());
            if (!fromp->user()) {
                fromp->user(1);
                toVisit.push(fromp);
            }
        }
        // Special handling of multidriven sequential block (packed or unpacked)
        if (ConstrDefVertex* const defp = dynamic_cast<ConstrDefVertex* const>(headp)) {
            auto commitp = defp->vscp()->user1u().to<ConstrCommitVertex*>();
            if (commitp && !commitp->user() && !commitp->inEmpty()) {
                // backward traverse and collect other drivers if exist
                toVisit.push(commitp);
                UASSERT_OBJ(commitp->outEmpty() || commitp->outSize1(), commitp,
                            "Expected at most one successor");
            }
        }
    }
    // clone the edges

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
                // should not happen
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
        auto s1 = m_sets[m_rep[k1]].size(), s2 = m_sets[m_rep[k2]].size();
        if (s1 < s2) {
            return makeUnion__(k1, k2);
        } else {
            return makeUnion__(k2, k1);
        }
    }
    MapType& sets() { return m_sets; }
};

std::vector<std::vector<CompVertex*>> groupVertices(const std::unique_ptr<DepGraph>& graphp,
                                                    const std::vector<CompVertex*>& sinks) {

    // find all ConstrDefVertex that hold an unpacked array whose corresponding
    // ConstrCommit has a CompVertex holding an AlwaysPost.
    DisjointSets<CompVertex*> sets{};
    for (CompVertex* vtxp : sinks) { sets.makeSet(vtxp); }

    for (CompVertex* vtxp : sinks) {

        // STATE
        //  AnyVertex::user1()    -> 1 if already visited
        graphp->userClearVertices();
        std::queue<V3GraphVertex*> toVisit;
        toVisit.push(vtxp);
        while (!toVisit.empty()) {
            V3GraphVertex* const headp = toVisit.front();
            toVisit.pop();
            // UINFO(3, "Visiting " << headp << endl);
            headp->user(1);
            // follow ancestors
            for (V3GraphEdge* edgep = headp->inBeginp(); edgep; edgep = edgep->inNextp()) {
                if (!edgep->fromp()->user()) { toVisit.push(edgep->fromp()); }
            }
            // follow defs to commits through the user pointer
            if (ConstrDefVertex* defp = dynamic_cast<ConstrDefVertex*>(headp)) {
                ConstrCommitVertex* commitp = defp->vscp()->user1u().to<ConstrCommitVertex*>();
                if (!commitp) continue;
                if (commitp->outEmpty()) continue;
                // are we dealing with non-trivial types?
                if (VN_IS(commitp->vscp()->dtypep()->skipRefp(), BasicDType)) continue;
                UASSERT_OBJ(commitp->outSize1(), commitp,
                            "commit with more than one successor " << commitp->vscp()->name()
                                                                   << endl);
                // check if the commit actually goes to an AssignPost or AlwaysPost
                auto succp = dynamic_cast<CompVertex*>(commitp->outBeginp()->top());
                UASSERT_OBJ(succp, commitp, "invalid type!");

                UASSERT_OBJ(sets.contains(succp), succp, "not in disjoint sets!");
                if (!succp->user() && VN_IS(succp->nodep(), AlwaysPost)) {
                    UINFO(3, "Union " << vtxp << " and  " << succp << endl);
                    sets.makeUnion(vtxp, succp);
                    succp->user(1);
                }
            }
        }
    }
    graphp->userClearVertices();
    std::vector<std::vector<CompVertex*>> disjointSinks;
    for (const auto& pair : sets.sets()) {
        disjointSinks.push_back(std::vector<CompVertex*>{});
        for (const auto& s : pair.second) { disjointSinks.back().push_back(s); }
    }
    if (dump() > 0) {
        const std::string filename = v3Global.debugFilename("disjoint") + ".txt";
        const std::unique_ptr<std::ofstream> logp{V3File::new_ofstream(filename)};
        if (logp->fail()) v3fatal("Cannot write " << filename);
        for (const auto& pair : sets.sets()) {
            *logp << "{" << std::endl;
            for (const auto& s : pair.second) { *logp << "\t\t" << s << std::endl; }
            *logp << "}" << std::endl;
        }
    }
    return disjointSinks;
}
};  // namespace
std::unique_ptr<DepGraph> DepGraphBuilder::build(const V3Sched::LogicByScope& logics) {

    return std::unique_ptr<DepGraph>{DepGraphBuilderImpl{logics}.graphp()};
}

std::vector<std::unique_ptr<DepGraph>>
DepGraphBuilder::splitIndependent(const std::unique_ptr<DepGraph>& graphp) {

    // We partition the graph into a maximal set of indepdent processes. These are
    // are BSP processes that can run independently between synchronization points
    // and share no data during computation.
    // All the race conditions that may exist in the original Verilog code
    // are localized within a processor. e.g.:
    // always @(posedge clock) x = something1;
    // always @(posedge clock) if (c) x = something2;
    // are ensured to the be fully local to one processor. This way we could simply
    // take an arbitrary ordering of execution and respect the execution semantics.
    // Note that since we do not have a shared-shared across processes, running
    // the two always blocks above in two different processes would essentially
    // be wrong from Verilog's execution semantics.
    //
    // To create such processes, we need to look for three patterns in the graph:
    //
    // 1) commit node with at least one predecessor and no successor
    // +----------------+
    // |                |
    // |    always      |
    // |                |
    // +-------+--------+
    //         |
    //         |
    //         |
    //  +------v-------+
    //  |              |
    //  |   commit lhs |
    //  |          rhs |
    //  +--------------+
    // These commit nodes exist due to sequential "blocking" logic, i.e.,
    // normal assignment in always blocks.
    // 2) An always block with no successor
    // +----------------+
    // |                |
    // |    always      |
    // |                |
    // +-------+--------+
    // This patterns correspond to always blocks with system/dpi side effects
    // e.g.,
    // always_ff @(posedge clock) $display("my counter is %d", counter);
    // 3) A commit node with Assign/AlwaysPost as it successor. The successor
    // will then have most likely (if not assign from constant) that is driven
    // by other always blocks.
    // +--------------+         +--------------+
    // |              |         |              | this one will probably have
    // |  commit lhs  |         |  commit rhs  | a predecessor that is an always
    // |              |         |              | block
    // +------+-------+         +----------+---+
    //        |                            |
    //        |                            |
    //        |   +--------------------+   |
    //        |   |  Assign/AlwaysPost |   |
    //        +--->  lhs = rhs         <---+
    //            |                    |
    //            +--------------------+
    // These configurations exist due to "nonblocking" (i.e., x <= expr) logic
    //
    // stranded commit nodes do not exist, i.e., they either have no successor
    // (i.e., commit rhs in #1) or have exactly one (i.e., commit lhs in #3)
    // so we can find all these patterns quit easily with scanning the vertices
    // just once.

    std::vector<AnyVertex*> processSinks;
    for (AnyVertex* itp = static_cast<AnyVertex*>(graphp->verticesBeginp()); itp;
         itp = static_cast<AnyVertex*>(itp->verticesNextp())) {
        // is it pattern 3?
        auto compVtx = dynamic_cast<CompVertex*>(itp);
        auto commitVtx = dynamic_cast<ConstrCommitVertex*>(itp);
        if (compVtx
            && (VN_IS(compVtx->nodep(), AlwaysPost) || VN_IS(compVtx->nodep(), AssignPost))) {
            UASSERT_OBJ(compVtx->outSize1() == 0, compVtx,
                        "Assign/AlwaysPost can not have a successor");
            processSinks.push_back(itp);
        } /* pattern 2 */ else if (compVtx && VN_IS(compVtx->nodep(), Always)
                                   && compVtx->outSize1() == 0) {
            processSinks.push_back(itp);
        } /* pattern 1 */ else if (commitVtx && commitVtx->outSize1() == 0) {
            UASSERT_OBJ(commitVtx->inSize1() > 0, commitVtx, "stranded commit?");
            processSinks.push_back(itp);
        }
    }

    std::vector<std::unique_ptr<DepGraph>> partitionsp;
    for (AnyVertex* const vtx : processSinks) {
        partitionsp.emplace_back(collectBackwardsBfs(graphp, vtx));
    }

    return partitionsp;
}

};  // namespace V3BspSched