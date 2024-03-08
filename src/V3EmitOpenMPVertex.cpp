// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit C++ (OpenMP)
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
#include "V3EmitOpenMP.h"
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

class EmitOpenMPVertex final : public EmitCFunc {
private:
    // MEMBERS
    // const AstClass* const m_fileClassp;
    // const bool m_slow;
    V3UniqueNames m_uniqueNames;
    const bool m_multiThreaded = true;  // OpenMP is multithread
    std::vector<AstCFile*> m_newFilesp;

    inline bool splitNeededAndEnabled() const {
        return !m_multiThreaded && EmitCFunc::splitNeeded();  // Never since OpenMP is multithread
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
        ofp()->putsHeader();
        puts("// DESCRIPTION: Verilator output: Design implementation internals\n");
        puts("// OpenMP implementation\n");
        puts("#include <verilated.h>\n");
        puts("#include <vlopenmp/verilated_openmp.h>\n");
        puts("#include <" + topClassName() + "__memory_contents.h>\n");
        // puts("using namespace openmp;");
    }

    void emitClass(const AstClass* classp) {
        // ToDo-OpenMP: Do we actually need the class?
        m_modp = classp;  // used by EmitCFunc::visit
        maybeOpenNextFile();
        puts("// at WORKER = " + cvtToStr(classp->flag().workerId()) + "\n");
        puts("\nclass ");
        puts(prefixNameProtect(classp));
        string baseClass = "Vertex";
        puts(" : public openmp::" + baseClass + " {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);  // public

        // emit the members
        // ofp()->puts("using Vec = openmp::InOut<openmp::Vector<IData, "
        //             "openmp::VectorLayout::COMPACT_PTR, alignof(QData)>>;\n\n");
        // for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
        //     if (AstVar* vrefp = VN_CAST(stmtp, Var)) {
        //         UASSERT_OBJ(vrefp->isClassMember(), vrefp, "expected class member");
        //         auto flag = vrefp->bspFlag();
        //         puts("/* [" + flag.ascii() + "] */\n");
        //         if (flag.isInputOnly()) {
        //             puts("VecIn ");
        //         } else {
        //             puts("Vec ");
        //         }
        //         // puts(vrefp->dtypep()->cType("", false, false));
        //         // puts("> ");
        //         puts(vrefp->nameProtect());
        //         puts("; /* " + AstNode::dedotName(vrefp->origName()) + " : "
        //              + vrefp->fileline()->ascii() + " */\n");
        //     }
        // }

        // emit method decls
        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) {
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

    void emitMemory(std::vector<std::vector<const AstClass*>>& toEmitp) {
        UASSERT(!m_ofp, "file not closed");
        string filename = v3Global.opt.makeDir() + "/" + topClassName();
        filename += "__memory_contents.h";
        m_ofp = new V3OutCFile{filename};
        ofp()->putsHeader();
        puts("// DESCRIPTION: Verilator output: Design implementation internals\n");
        puts("// OpenMP implementation\n");
        puts("#ifndef VERILATED_MEM_CONTENTS_H_\n");
        puts("#define VERILATED_MEM_CONTENTS_H_  // guard\n");
        puts("#include <vlopenmp/verilated.h>\n");
        puts("#include <vlopenmp/verilated_openmp.h>\n");

        puts("\n");
        puts("class memory_contents {\n");

        for (int tid = 0; tid < toEmitp.size(); tid++) {
            for (const AstClass* classp : toEmitp[tid]) {
                for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                    if (AstVar* vrefp = VN_CAST(stmtp, Var)) {
                        UASSERT_OBJ(vrefp->isClassMember(), vrefp, "expected class member");
                        auto flag = vrefp->bspFlag();
                        if (flag.isInputOnly()) {
                            // Skip input-only fields since they must be output somewhere
                        } else {
                            // Should only be output once?
                            puts("/* [" + flag.ascii() + "] */\n");
                            puts(vrefp->dtypep()->cType(" ", false, false));
                            puts(vrefp->nameProtect());
                            puts("; /* " + AstNode::dedotName(vrefp->origName()) + " : "
                                 + vrefp->fileline()->ascii() + " */\n");
                        }
                    }
                }
            }
        }
        puts("}\n");
        puts("#endif  // guard\n");
        m_ofp->putsEndGuard();
        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
    }

    /// Unused
    explicit EmitOpenMPVertex(AstNetlist* netlistp)
        : m_multiThreaded(true) {  // OpenMP is multithreaded
        if (netlistp) {
            std::vector<const AstClass*> toEmitp;
            for (const AstNode* nodep = netlistp->modulesp(); nodep; nodep = nodep->nextp()) {
                if (const AstClass* classp = VN_CAST(nodep, Class)) {
                    if (classp->flag().isBsp()) { toEmitp.push_back(classp); }
                }
            }
            for (const AstClass* classp : toEmitp) {
                UINFO(3, "Emitting " << classp->nameProtect() << endl);
                emitClass(classp);
            }
            if (m_ofp) { VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr); }
        }
    }

    // thread-safe emitter
    explicit EmitOpenMPVertex(AstNetlist* netlistp, const std::vector<const AstClass*>& toEmitp,
                              int threadIndex)
        : m_multiThreaded(true) {
        openNextOutputFile("codelet_" + cvtToStr(threadIndex));
        for (const AstClass* classp : toEmitp) { emitClass(classp); }
        if (m_ofp) { VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr); }
    }

public:
    /// Unused
    static inline void emitAll(AstNetlist* netlistp) {
        EmitOpenMPVertex impl{netlistp};
        for (AstCFile* cfilep : impl.m_newFilesp) { netlistp->addFilesp(cfilep); }
    }

    static inline void emitAllThreaded(AstNetlist* netlistp) {
        std::vector<std::vector<const AstClass*>> toEmitp;
        toEmitp.push_back({});
        int nodeCount = 0;

        for (const AstNode* nodep = netlistp->modulesp(); nodep; nodep = nodep->nextp()) {
            if (const AstClass* classp = VN_CAST(nodep, Class)) {
                if (classp->flag().isBsp()) {
                    toEmitp.back().push_back(classp);
                    nodeCount += classp->nodeCount();

                    if (v3Global.opt.outputSplit()
                        && nodeCount >= v3Global.opt.outputSplit() * 10) {
                        toEmitp.push_back({});
                        nodeCount = 0;
                    }
                }
            }
        }
        UINFO(3, "Emitting " << toEmitp.size() << " codelet files " << endl);
        using EmitResult = std::vector<AstCFile*>;
        std::vector<std::future<EmitResult>> results;
        for (int tid = 0; tid < toEmitp.size(); tid++) {
            auto future = V3ThreadPool::s().enqueue(std::function<EmitResult()>{[=, &toEmitp]() {
                EmitOpenMPVertex impl{netlistp, toEmitp[tid], tid};
                return impl.m_newFilesp;
            }});
            results.emplace_back(std::move(future));
        }
        int filesEmitted = 0;
        for (auto& ft : results) {
            UASSERT(ft.valid(), "invalid future");
            ft.wait();
            EmitResult res = ft.get();
            filesEmitted += res.size();
            UINFO(3, "Emitted " << filesEmitted << " files " << endl);
            for (AstCFile* cfilep : res) { netlistp->addFilesp(cfilep); }
        }
        EmitOpenMPVertex impl{nullptr}; // Need (empty) object for method
        impl.emitMemory(toEmitp);
    }
};

void V3EmitOpenMP::emitVertex() {
    // Make parent module pointers available, enables user4
    const EmitCParentModule emitCParentModule;
    AstNetlist* netlistp = v3Global.rootp();

    EmitOpenMPVertex::emitAllThreaded(netlistp);
    V3Stats::statsStage("emitVertex");
}