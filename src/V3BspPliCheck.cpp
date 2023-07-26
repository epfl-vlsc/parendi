// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Check sub-tree for PLI/DPI nodes
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

#include "V3BspPliCheck.h"

#include "V3Ast.h"

namespace V3BspSched {
class InstrPliChecker final : public VNVisitor {
private:
    bool m_hasPli = false;
    inline void setPli() { m_hasPli = true; }
    void visit(AstCCall* callp) {
        if (callp->funcp()->dpiImportWrapper()) { setPli(); }
        iterateChildren(callp);
    }
    void visit(AstDisplay* nodep) override { setPli(); }
    void visit(AstFinish* nodep) override { setPli(); }
    void visit(AstStop* nodep) override { setPli(); }
    void visit(AstNodeReadWriteMem* nodep) override { setPli(); }
    void visit(AstNode* nodep) {
        if (!nodep->isPure()) {
            setPli();
            return;
        }
        iterateChildren(nodep);
    }
    explicit InstrPliChecker(AstNode* nodep) {
        m_hasPli = false;
        iterate(nodep);
    }

public:
    static bool hasPli(AstNode* nodep) {
        const InstrPliChecker visitor{nodep};
        return visitor.m_hasPli;
    }
};

bool PliCheck::check(AstNode* nodep) { return InstrPliChecker::hasPli(nodep); }

};  // namespace V3BspSched