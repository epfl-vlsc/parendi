// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit Typedefs for poplar
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

#include "V3EmitCBase.h"
#include "V3EmitPoplar.h"
#include "V3File.h"
#include "V3Os.h"

VL_DEFINE_DEBUG_FUNCTIONS;

class EmitPoplarStructs : public EmitCBaseVisitor {
private:
    void decorateFirst(bool& first, const string& str) {
        if (first) {
            putsDecoration(str);
            first = false;
        }
    }
    void emitStructDecl(const AstNodeModule* modp, AstNodeUOrStructDType* sdtypep,
                        std::set<AstNodeUOrStructDType*>& emitted) {
        if (emitted.count(sdtypep) > 0) return;
        emitted.insert(sdtypep);
        for (const AstMemberDType* itemp = sdtypep->membersp(); itemp;
             itemp = VN_AS(itemp->nextp(), MemberDType)) {
            AstNodeUOrStructDType* subp = VN_CAST(itemp->skipRefp(), NodeUOrStructDType);
            if (subp && !subp->packed()) {
                // Recurse if it belongs to the current module
                if (subp->classOrPackagep() == modp) {
                    emitStructDecl(modp, subp, emitted);
                    puts("\n");
                }
            }
        }
        puts(sdtypep->verilogKwd());  // "struct"/"union"
        puts(" " + EmitCBaseVisitor::prefixNameProtect(sdtypep) + " {\n");
        for (const AstMemberDType* itemp = sdtypep->membersp(); itemp;
             itemp = VN_AS(itemp->nextp(), MemberDType)) {
            puts(itemp->dtypep()->cType(itemp->nameProtect(), false, false));
            puts(";\n");
        }
        puts("};\n");
    }
    void emitTypdefsInModule(AstNodeModule* modp) {
        bool first = true;
        // keep track of emitted structs
        std::set<AstNodeUOrStructDType*> emitted;
        for (AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            const AstTypedef* const tdefp = VN_CAST(nodep, Typedef);
            if (!tdefp) continue;
            AstNodeUOrStructDType* const sdtypep
                = VN_CAST(tdefp->dtypep()->skipRefToEnump(), NodeUOrStructDType);
            if (!sdtypep) continue;
            if (sdtypep->packed()) continue;
            decorateFirst(first, "\n// UNPACKED STRUCT TYPES\n");
            emitStructDecl(modp, sdtypep, emitted);
        }
    }
    void visit(AstNode* nodep) { iterateChildren(nodep); }

public:
    explicit EmitPoplarStructs(AstNetlist* netlistp) {

        /// open a header file
        string filename = v3Global.opt.makeDir() + "/" + topClassName() + "__structs.h";
        m_ofp = new V3OutCFile{filename};
        ofp()->putsHeader();
        puts("// DESCRIPTION: Verilator output: typedef structures\n");
        puts("// included by both codelets and the host code");
        ofp()->putsGuard();
        ofp()->puts("#ifdef __IPU__\n");
        ofp()->puts("#include <vlpoplar/verilated.h>\n");
        ofp()->puts("#else\n");
        ofp()->puts("#include <verilated.h>\n");
        ofp()->puts("#endif\n\n");
        emitTypdefsInModule(netlistp->topModulep());
        ofp()->putsEndGuard();
        VL_DO_CLEAR(delete m_ofp, m_ofp = nullptr);
    }
};

void V3EmitPoplar::emitStructs() {

    { EmitPoplarStructs{v3Global.rootp()}; }
}