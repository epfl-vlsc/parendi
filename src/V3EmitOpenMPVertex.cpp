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
        string filenameBase = filename;
        filename += ".cpp";

        AstCFile* const cfilep = new AstCFile{v3Global.rootp()->fileline(), filename};
        cfilep->slow(false);
        cfilep->source(true);
        cfilep->codelet(true);

        m_newFilesp.push_back(cfilep);

        m_ofp = new V3OutCFile{filename};
        ofp()->putsHeader();
        puts("// DESCRIPTION: Verilator output: Design implementation internals\n");
        puts("// OpenMP implementation\n");
        puts("// Codelet implementations\n\n");
        puts("#include \"verilated.h\"\n");
        puts("#include <" + topClassName() + "__memory_contents.h>\n");
        puts("#include <" + topClassName() + "__codelet.h>\n");
    }

    void emitClass(const AstClass* classp) {
        // ToDo-OpenMP: Do we actually need the class?
        m_modp = classp;  // used by EmitCFunc::visit
        maybeOpenNextFile();
        puts("// at WORKER = " + cvtToStr(classp->flag().workerId()) + "\n");
        puts("\nclass ");
        puts(prefixNameProtect(classp));
        puts(" : public CodeletBase {\n");
        ofp()->resetPrivate();
        ofp()->putsPrivate(false);  // public

        // emit method and member decls
        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) {
                emitCFuncHeader(funcp, classp, false);
                puts(";\n");
            } else if (AstVar* vrefp = VN_CAST(stmtp, Var)) {
                UASSERT_OBJ(vrefp->isClassMember(), vrefp, "expected class member");
                if (vrefp->bspFlag().hasLocal()) {
                    puts(vrefp->dtypep()->cType("", false, false));
                    puts(" ");
                    puts(vrefp->nameProtect());
                    puts("; /* " + AstNode::dedotName(vrefp->origName()) + " : "
                         + vrefp->fileline()->ascii() + " */\n");
                }
            }
        }
        ensureNewLine();
        puts("};\n\n");

        // emit method defs
        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstCFunc* funcp = VN_CAST(stmtp, CFunc)) {
                V3EmitOpenMP::addCopyIn(funcp);
                EmitCFunc::visit(funcp);
            }
        }
    }

    void emitClassList(const std::vector<const AstClass*>& toEmitp) {
        puts("\nCodeletBase* CodeletInstances[] = {\n");
        for (const AstClass* classp : toEmitp) {
            if (!classp->flag().isBspInit()) {
                puts("  new " + prefixNameProtect(classp) + "(),\n");
            }
        }
        puts("nullptr};\n");
        puts("\nCodeletBase* CodeletInitInstances[] = {\n");
        for (const AstClass* classp : toEmitp) {
            if (classp->flag().isBspInit()) {
                puts("  new " + prefixNameProtect(classp) + "(),\n");
            }
        }
        puts("nullptr};\n");
    }

    void emitHeaderFile(std::vector<std::vector<const AstClass*>>& toEmitp) {
        UASSERT(!m_ofp, "file not closed");
        for (int tid = 0; tid < toEmitp.size(); tid++) {
            string filename = v3Global.opt.makeDir() + "/" + topClassName();
            filename += "__codelet.h";
            // Really should follow implementation name, but it is hard to find
            // names in other files's includes, and we only use one file for all
            // codelets since one only have one IPU.
            //
            // filename += "__codelet_" + cvtToStr(tid); filename =
            // m_uniqueNames.get(filename); filename += ".h";
            m_ofp = new V3OutCFile{filename};

            ofp()->putsHeader();
            puts("// DESCRIPTION: Verilator output: Design implementation internals\n");
            puts("// OpenMP implementation\n");
            puts("// Codelet classes headers\n\n");
            puts("#ifndef VERILATED_CODELETS_H_\n");
            puts("#define VERILATED_CODELETS_H_  // guard\n");
            puts("#include \"verilated.h\"\n");
            puts("#include <" + topClassName() + "__memory_contents.h>\n");

            for (const AstClass* classp : toEmitp[tid]) {
                puts("\nclass ");
                puts(prefixNameProtect(classp));
                puts(";\n");
            }

            // Null terminated lists
            puts("\nextern CodeletBase* CodeletInstances[];\n");
            puts("extern CodeletBase* CodeletInitInstances[];\n");

            puts("static MemoryContents MemIn;\n");
            puts("static MemoryContents MemOut;\n");

            puts("\n#endif  // guard\n");
            m_ofp->putsEndGuard();
            VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
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
        puts("// Shared memory structure\n\n");
        puts("#ifndef VERILATED_MEM_CONTENTS_H_\n");
        puts("#define VERILATED_MEM_CONTENTS_H_  // guard\n");
        puts("#include \"verilated.h\"\n");

        puts("\n");
        puts("class MemoryContents {\n");
        puts("  public: \n");

        class findClassMembers final : public VNVisitor {
        public:
            EmitOpenMPVertex* m_parent;
            vl_unordered_map<string, int> m_varNames;
            int m_bytesUsed = 0;
            int m_classID = 0;
            const AstClass* m_classp;

            void visit(AstVarRef* vrefp) override {
                AstVar* varp = vrefp->varp();
                auto flag = varp->bspFlag();
                if (!flag.hasLocal() && !flag.isInputOnly() && !varp->isFuncLocal()
                    && !varp->isParam() && vrefp->access() != VAccess::READ) {
                    // Skip class variables since they are class members. Skip
                    // input-only and read-only fields since they are declared
                    // by the codelet that writes them.
                    string varName = varp->nameProtect();
                    if (!m_varNames.count(varName)) {  // Output only once
                        m_parent->puts("/* [" + flag.ascii() + "] */\n");
                        m_parent->puts(varp->dtypep()->cType(" ", false, false));
                        m_parent->puts(varName);
                        m_parent->puts("; /* " + AstNode::dedotName(varp->origName()) + " : "
                                       + varp->fileline()->ascii() + " */\n");
                        m_varNames[varName] = m_classID;
                        m_bytesUsed += varp->dtypep()->widthTotalBytes();
                    } else {
                        if (m_varNames[varName] != m_classID && !m_classp->flag().isBspInit()) {
                            v3warn(V3ErrorCode::EC_INFO,
                                   "Multiple codelets write to MemOut." + varName);
                        }
                    }
                }
            }
            void visit(AstNode* nodep) override { iterateChildren(nodep); }

            explicit findClassMembers(EmitOpenMPVertex* parent) { m_parent = parent; }
        } fcm(this);

        for (int tid = 0; tid < toEmitp.size(); tid++) {
            for (const AstClass* classp : toEmitp[tid]) {
                if (fcm.m_bytesUsed % VL_CACHE_LINE_BYTES) {
                    puts("/* Padding */\n");
                    int padSize = VL_CACHE_LINE_BYTES - (fcm.m_bytesUsed % VL_CACHE_LINE_BYTES);
                    puts("char pad_" + cvtToStr(fcm.m_classID) + "[" + cvtToStr(padSize) + "];\n");
                }
                puts("/* Class " + cvtToStr(fcm.m_classID) + " */\n");

                fcm.m_bytesUsed = 0;
                fcm.m_classp = classp;
                for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                    fcm.iterate(stmtp);
                }
                fcm.m_classID += 1;
            }
        }
        puts("};\n");
        puts("#endif  // guard\n");
        m_ofp->putsEndGuard();
        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
    }

    /// Only used to produce a class instance.
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
            emitClassList(toEmitp);
            if (m_ofp) { VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr); }
        }
    }

    // thread-safe emitter
    explicit EmitOpenMPVertex(AstNetlist* netlistp, const std::vector<const AstClass*>& toEmitp,
                              int threadIndex)
        : m_multiThreaded(true) {
        openNextOutputFile("codelet_" + cvtToStr(threadIndex));
        for (const AstClass* classp : toEmitp) { emitClass(classp); }
        emitClassList(toEmitp);
        if (m_ofp) { VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr); }
    }

    static std::vector<std::vector<const AstClass*>> findClassesToEmit(AstNetlist* netlistp) {
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
        return toEmitp;
    }

public:
    static inline void emitAllThreaded(AstNetlist* netlistp) {
        std::vector<std::vector<const AstClass*>> toEmitp = findClassesToEmit(netlistp);

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
        EmitOpenMPVertex impl{nullptr};  // Need (empty) object for method
        impl.emitHeaderFile(toEmitp);
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

// Under the BSP model for OpenMP, a thread reads values from MemIn and writes
// results to MemOut. The use of two memories causes problems when a thread
// writes intermediate values to a location, since reading MemIn will see the
// unmodified value. The easiest solution (implemented below) is to copy from
// MemIn to MemOut on function entry and just operate on MemOut. This assumes
// that each function uses a distinct subset of MemOut so there are no data
// races. A more optimized solution is allow all reads before the first write to
// access MemIn and thereafter access MemOut. However, this requires analysis
// similar to producing SSA since multiple definitions can reach a single use,
// e.g.:
//
// if (p) { x = x + 1 } y = x
//
void V3EmitOpenMP::addCopyIn(AstCFunc* cfuncp) {
    // Find BSP RO and RW variables
    struct CatVisitor : public VNVisitor {
        std::unordered_set<AstVar*> m_varsRead;
        std::unordered_set<AstVar*> m_varsWritten;

        void visit(AstNode* nodep) override { iterateChildren(nodep); }
        void visit(AstVarRefView* nodep) override {
            if (nodep->vrefp()->access() == VAccess::READ) {
                m_varsRead.insert(nodep->vrefp()->varp());
                // Set to RW so access will go to MemOut
                // nodep->vrefp()->access(VAccess::READWRITE);
            } else {
                m_varsWritten.insert(nodep->vrefp()->varp());
            }
        }
        explicit CatVisitor(AstCFunc* cfuncp) {}
    } categorizeVRV(cfuncp);
    categorizeVRV.iterate(cfuncp);

    // Update RO references of RW variables so they access MemOut.
    struct FixVisitor : public VNVisitor {
        std::unordered_set<AstVar*> m_varsWritten;

        void visit(AstNode* nodep) override { iterateChildren(nodep); }
        void visit(AstVarRefView* nodep) override {
            if (nodep->vrefp()->access() == VAccess::READ
                && m_varsWritten.count(nodep->vrefp()->varp()) != 0) {
                // Set to RW so access will go to MemOut
                nodep->vrefp()->access(VAccess::READWRITE);
            }
        }
        explicit FixVisitor(AstCFunc* cfuncp) {}
    } updateVRV(cfuncp);
    updateVRV.m_varsWritten = categorizeVRV.m_varsWritten;
    updateVRV.iterate(cfuncp);

    // If a variable in shared memory is read, add a copy from MemIn to MemOut
    // so the read sees the current (MemIn) value. May introduce unnecessary
    // copies when value is written before read, but avoids reaching def
    // analysis.
    for (auto varp = categorizeVRV.m_varsWritten.begin();
         varp != categorizeVRV.m_varsWritten.end(); varp++) {
        if (categorizeVRV.m_varsRead.count(*varp) != 0 && !(*varp)->bspFlag().hasLocal()) {
            FileLine* fl = cfuncp->fileline();
            AstVarScope* vscp = new AstVarScope{fl, cfuncp->scopep(), *varp};
            cfuncp->scopep()->addVarsp(vscp);
            // Copy from MemIn to MemOut
            AstAssign* copy
                = new AstAssign{fl, new AstVarRefView(fl, new AstVarRef{fl, vscp, VAccess::WRITE}),
                                new AstVarRefView(fl, new AstVarRef{fl, vscp, VAccess::READ})};
            cfuncp->addInitsp(copy);
        }
    }
}
