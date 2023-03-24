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
                // do we need DEF->logic constraints as well? Probably not sice
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
    // ConstrCommit has an CompVertex holding an AlwaysPost. These are the
    // "non-blocking" memories. Any
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
                if (commitp && !commitp->outEmpty()) {
                    UASSERT_OBJ(commitp->outSize1(), commitp, "At most one ancestor");
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

    /**
     * Split the "serial" dep graph into a maximal set independent "parallel"
     * graphs that would eventually represent BSP tasks.
     * We consider the CompVertex containing AstAssignPot or AstAlwaysPost as
     * "persitence" operations that commit or persist local work by each parallel
     * BSP task. AstAssignPost could potentially turn into remote copies or send
     * whereas AstAlwaysPost ends up being performed locally.
     *
     * Other sink nodes should not exists, e.g., a commit can also be a sink node
     * but that does not matter since its just a constraint.
     *
     *
     */
    std::vector<std::unique_ptr<DepGraph>> partitions;

    // collect all the vertices that have no successors, these edges going to
    // of type CompVertex where the internal logic is either AssignPost or
    // AlwaysPost.
    std::vector<CompVertex*> sinks;
    // for every def node, there should also exist a commit node sharing the same
    // varscope. We need to create a mapping between the two so that we can
    // look up the corresponding commit vertex of every definition when we reach it
    // see below for reason why.
    // STATE
    //  AstVarScope::user1p     -> ConstrCommitVertex*
    VNUser1InUse user1InUse;
    AstNode::user1ClearTree();
    for (AnyVertex* itp = static_cast<AnyVertex*>(graphp->verticesBeginp()); itp;
         itp = static_cast<AnyVertex*>(itp->verticesNextp())) {
        auto asLogicVtxp = dynamic_cast<CompVertex*>(itp);
        if (asLogicVtxp && asLogicVtxp->outEmpty()) {
            sinks.push_back(asLogicVtxp);

        }
        if (auto asCommitVtxp = dynamic_cast<ConstrCommitVertex*>(itp)) {
            asCommitVtxp->vscp()->user1u(VNUser{asCommitVtxp});
        }
    }
    UASSERT(!sinks.empty(),
            "Empty sink set, perhaps there are no non-blocking assignments in the graph?");
    // OUTLINE OF THE PARTITIONING ALGORITHM
    // Using the graph we have just created above, we can create N parallel processes
    // to execute on N cores. We star the partitioning by finding a maximal set of
    // "compatible" processes and then iteratively merge them to have fewer
    // processes. By maximal, we mean that there are an infinite number of cores
    // that we can run the code on.
    // In BSP, we assume all computation is local and only values updated by
    // AstAssignPost can be communicated between cores. This readily means that
    // even AstAlwaysPost needs to be completely local. Hence if in the path
    // to compute some AstAssignPost, we reference (read) some DEF vertex that
    // also reaches an AlwaysPost, we need to group the AstAssignPost and AstAlwaysPost
    // together. We do this because we do not want to duplicate "memories"
    // (unpacked arrays) and frankly, that won't work for something like:
    // b1: always @(posedge) a <= mem[raddr];
    // b2: always @(posedge) if (wen) mem[waddr] <= something
    // There is no way (at least I do not think there is) to compute b1 and b2
    // on two separate cores and not copy the whole memory from b2 to b1 on every
    // cycle.
    // groupVertices takes care of this and essentially ensure that b1 and b2 end
    // up on the same core.
    // Once we have the groupings, we can start from the AstAssignPost and AstAlwaysPost
    // vertices in each group and collect (backwards traversal) everything needed
    // for their computation. This works fine as long as there are no blocking assignments
    // to unpacked arrays, or as long as all sequential writes to a variable are contained
    // withing one block for instance:
    // logic [31:0] upacked_multidriven;
    // always @(posedge) unpacked_multidriven[i] = something1;
    // always @(posedge) begin
    //      packed_multidriven[j] = something2;
    //      y <= packed_multidriven;
    // end
    // Will create a commit vertex with 2 predecessors for upacked_multidriven.
    // Notices that since the assignment is blocking, there will not be an AlwaysPost
    // vertex for upacked_multidriven so grouping is not enough to ensure both
    // always blocks endup on the same core. Here we follow DEF x to COMMIT x
    // and then collect all that drives the COMMIT for the block that references
    // DEF x. This ensures that both writes are contained within the same process.
    // Notice how this also extends to multidriven packed arrays.
    // From the explanation above, you can probably see that read-only packed or
    // unpacked arrays will never limit parallelism since they never commit nor
    // drive an AstAlwaysPost, hence they get duplicated on demand.
    // Lastly, keep in mind that the same situation applies for sink nodes
    // that are simply Always blocks (e.g., always_ff $display(...)). These
    // vertices may also have to be grouped accordingly should a multidriven
    // signal or non-blocking unpacked array appears on the RHS.
    

    for (const auto& vtxp : groupVertices(graphp, sinks)) {
        partitions.emplace_back(backwardTraverseAndCollect(graphp, vtxp));
    }
    UINFO(3, "Found " << partitions.size() << " independent processes" << endl);
    if (dump() > 3) {
        for (int i = 0; i < partitions.size(); i++) {
            partitions[i]->dumpDotFilePrefixed("partition_" + std::to_string(i));
        }
    }
    return partitions;
}

};  // namespace V3BspSched