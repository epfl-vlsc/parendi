// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit C++ (poplar)
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

#include "V3Ast.h"
#include "V3BspModules.h"
#include "V3EmitC.h"
#include "V3EmitCBase.h"
#include "V3EmitCFunc.h"
#include "V3EmitPoplar.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3ThreadPool.h"
#include "V3UniqueNames.h"

#include <algorithm>
#include <deque>
#include <list>
#include <map>
#include <unordered_set>
#include <vector>
VL_DEFINE_DEBUG_FUNCTIONS;

class EmitPoplarVertex final : public EmitCFunc {
private:
    // MEMBERS
    // const AstClass* const m_fileClassp;
    // const bool m_slow;
    V3UniqueNames m_uniqueNames;
    bool m_usesSupervisor = false;
    const bool m_multiThreaded = false;
    std::vector<AstCFile*> m_newFilesp;

    inline bool splitNeededAndEnabled() const {
        return !m_multiThreaded && EmitCFunc::splitNeeded();
    }
    void maybeOpenNextFile() {
        if (!m_ofp || splitNeededAndEnabled()) {
            if (m_ofp) VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
            openNextOutputFile("codelet");
        }
    }

    void openNextOutputFile(const string& subFileName) {
        UASSERT(!m_ofp, "Output file already open");

        splitSizeReset();  // Reset file size tracking
        m_lazyDecls.reset();  // Need to emit new lazy declarations

        string filename = v3Global.opt.makeDir() + "/" + topClassName();
        if (!subFileName.empty()) {
            filename += "__" + subFileName;
            filename = m_uniqueNames.get(filename);
        }
        filename += ".cpp";

        AstCFile* const cfilep = new AstCFile{v3Global.rootp()->fileline(), filename};
        cfilep->slow(false);
        cfilep->source(true);
        cfilep->codelet(true);

        // v3Global.rootp()->addFilesp(cfilep);
        m_newFilesp.push_back(cfilep);

        m_ofp = new V3OutCFile{filename};
        // new AstCFile{}
        ofp()->putsHeader();
        puts("// DESCRIPTION: Verilator output: Design implementation internals\n");
        puts("// Poplar vertex implementation\n");
        if (m_usesSupervisor) { puts("#define VL_USES_IPU_SUPERVISOR\n"); }
        puts("#include <vlpoplar/verilated.h>\n");
        puts("#include <poplar/Vertex.hpp>\n");
        puts("#include \"" + topClassName() + "__structs.h\"\n");
        // puts("using namespace poplar;");

        // if (v3Global.dpi()) { v3warn("dpi not supported with poplar\n"); }
    }

    void emitClass(const AstClass* classp) {

        m_modp = classp;  // used by EmitCFunc::visit
        maybeOpenNextFile();
        puts("// at TILE = " + cvtToStr(classp->flag().tileId())
             + "   WORKER = " + cvtToStr(classp->flag().workerId()) + "\n");
        puts("\nclass ");
        puts(prefixNameProtect(classp));
        string baseClass = "Vertex";
        if (classp->flag().isSupervisor()) { baseClass = "SupervisorVertex"; }
        puts(" : public poplar::" + baseClass + " {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);  // public

        // emit the members
        ofp()->puts("using Vec = poplar::InOut<poplar::Vector<IData, "
                    "poplar::VectorLayout::COMPACT_PTR, alignof(QData)>>;\n\n");
        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstVar* vrefp = VN_CAST(stmtp, Var)) {
                UASSERT_OBJ(vrefp->isClassMember(), vrefp, "expected class member");
                auto flag = vrefp->bspFlag();
                puts("/* [" + flag.ascii() + "] */\n");
                puts("Vec ");
                // puts(vrefp->dtypep()->cType("", false, false));
                // puts("> ");
                puts(vrefp->nameProtect());
                puts("; /* " + vrefp->origName() + " : " + vrefp->fileline()->ascii() + " */\n");
            }
        }

        // emit method decls
        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) {
                if (classp->flag().isSupervisor() && funcp->name() == "compute") {
                    puts("__attribute__((target(\"supervisor\"))) ");
                }
                emitCFuncHeader(funcp, classp, false);
                puts(";\n");
            }
        }
        ensureNewLine();
        puts("};\n\n");

        // emit method defs

        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) { EmitCFunc::visit(funcp); }
        }
    }

    explicit EmitPoplarVertex(AstNetlist* netlistp)
        : m_multiThreaded(false) {
        std::vector<const AstClass*> toEmitp;
        for (const AstNode* nodep = netlistp->modulesp(); nodep; nodep = nodep->nextp()) {
            if (const AstClass* classp = VN_CAST(nodep, Class)) {
                if (classp->flag().isBsp()) {
                    if (classp->flag().isSupervisor()) { m_usesSupervisor = true; }
                    toEmitp.push_back(classp);
                }
            }
        }
        for (const AstClass* classp : toEmitp) {
            UINFO(3, "Emitting " << classp->nameProtect() << endl);
            emitClass(classp);
        }

        if (m_ofp) { VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr); }
    }

    // thread-safe emitter
    explicit EmitPoplarVertex(AstNetlist* netlistp, const std::vector<const AstClass*>& toEmitp,
                              bool useSupervisor, int threadIndex)
        : m_usesSupervisor(useSupervisor)
        , m_multiThreaded(true) VL_MT_SAFE {
        openNextOutputFile("codelet_" + cvtToStr(threadIndex));
        for (const AstClass* classp : toEmitp) { emitClass(classp); }
        if (m_ofp) { VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr); }
    }

public:
    static inline void emitAll(AstNetlist* netlistp) {
        EmitPoplarVertex impl{netlistp};
        for (AstCFile* cfilep : impl.m_newFilesp) { netlistp->addFilesp(cfilep); }
    }

    static inline void emitAllThreaded(AstNetlist* netlistp) {

        std::vector<std::vector<const AstClass*>> toEmitp;
        toEmitp.push_back({});
        bool useSupervisor = false;
        int nodeCount = 0;
        for (const AstNode* nodep = netlistp->modulesp(); nodep; nodep = nodep->nextp()) {
            if (const AstClass* classp = VN_CAST(nodep, Class)) {
                if (classp->flag().isBsp()) {
                    if (classp->flag().isSupervisor()) { useSupervisor = true; }
                    toEmitp.back().push_back(classp);
                    nodeCount += classp->nodeCount();

                    if (v3Global.opt.outputSplit() && nodeCount >= v3Global.opt.outputSplit()) {
                        toEmitp.push_back({});
                        nodeCount = 0;
                    }
                }
            }
        }
        using EmitResult = std::vector<AstCFile*>;
        std::vector<std::future<EmitResult>> results;
        for (int tid = 0; tid < toEmitp.size(); tid++) {

            auto future = V3ThreadPool::s().enqueue(std::function<EmitResult()>{[=, &toEmitp]() {
                EmitPoplarVertex impl{netlistp, toEmitp[tid], useSupervisor, tid};
                return impl.m_newFilesp;
            }});
            results.emplace_back(std::move(future));
        }
        for (auto& ft : results) {
            UASSERT(ft.valid(), "invalid future");
            ft.wait();
            EmitResult res = ft.get();
            for (AstCFile* cfilep : res) { netlistp->addFilesp(cfilep); }
        }
    }
};
void V3EmitPoplar::emitVertex() {
    // Make parent module pointers available, enables user4
    const EmitCParentModule emitCParentModule;
    AstNetlist* netlistp = v3Global.rootp();

    // EmitPoplarVertex::emitAll(netlistp);
    EmitPoplarVertex::emitAllThreaded(netlistp);
    V3Stats::statsStage("emitVertex");
}