// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: AstNode sub-types representing poplar constructs
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

#ifndef VERILATOR_V3ASTNODESPOPLAR_H_
#define VERILATOR_V3ASTNODESPOPLAR_H_

#ifndef VERILATOR_V3AST_H_
#error "Use V3Ast.h as the include"
#include "V3Ast.h"  // This helps code analysis tools pick up symbols in V3Ast.h
#define VL_NOT_FINAL  // This #define fixes broken code folding in the CLion IDE
#endif

// === Abstract base node types (AstNode*) =====================================

class AstNodePoplarProgram VL_NOT_FINAL : public AstNode {
    // abstract poplar program
    std::string m_debugContext;  // debug context to pass to the poplar API
protected:
    AstNodePoplarProgram(VNType t, FileLine* fl, const std::string& debugContext)
        : AstNode{t, fl}
        , m_debugContext(debugContext) {}

public:
    ASTGEN_MEMBERS_AstNodePoplarProgram;
    void debugContext(const std::string& ctx) { m_debugContext = ctx; }
    std::string debugContext() const { return m_debugContext; }
};

// === Concrete node types

// === AstNodePoplarProgram ===

class AstPoplarCopy final : public AstNodePoplarProgram {
    // A poplar::program::Copy operation
    // Parents: POPLARSEQUENCE
    // @astgen op1 := fromp  : AstVarRef // tensor to copy from
    // @astgen op2 := top    : AstVarRef // tensor to copy to
private:
    bool m_dontOutline = false;  // do not outline this copy as a function call
public:
    AstPoplarCopy(FileLine* fl, AstVarRef* fromp, AstVarRef* top, bool dontOutline = false,
                  const std::string& debugContext = "")
        : ASTGEN_SUPER_PoplarCopy(fl, debugContext) {
        UASSERT(fromp->dtypep()->basicp()->keyword() == VBasicDTypeKwd::POPLAR_TENSOR,
                "expcted fromp of POPLAR_TENSOR type");
        UASSERT(top->dtypep()->basicp()->keyword() == VBasicDTypeKwd::POPLAR_TENSOR,
                "expected top of POPLAR_TENSOR type");
        this->fromp(fromp);
        this->top(top);
    }
    ASTGEN_MEMBERS_AstPoplarCopy;
    // void dump(std::ostream& str) const override;
    bool dontOutline() const { return m_dontOutline; }
    void dontOutline(bool dontOutline) { m_dontOutline = dontOutline; }
};

class AstPoplarExecute final : public AstNodePoplarProgram {
    // A poplar::program::Execute operation
    // @astgen op1 := computeSetp : AstVarRef
public:
    AstPoplarExecute(FileLine* fl, AstVarRef* csp, const std::string& debugContext = "")
        : ASTGEN_SUPER_PoplarExecute(fl, debugContext) {
        UASSERT(csp->dtypep()->basicp()->keyword() == VBasicDTypeKwd::POPLAR_COMPUTESET,
                "invalid type");
        this->computeSetp(csp);
    }
    ASTGEN_MEMBERS_AstPoplarExecute;
};

class AstPoplarSequence final : public AstNodePoplarProgram {
    // A poplar::program::Sequence
    // @astgen op1 :=  progsp    : List[AstNodePoplarProgram]
public:
    AstPoplarSequence(FileLine* fl, AstNodePoplarProgram* progsp, const std::string& debugContext = "")
        : ASTGEN_SUPER_PoplarSequence(fl, debugContext) {
            this->addProgsp(progsp);
        }
    ASTGEN_MEMBERS_AstPoplarSequence;
};

class AstPoplarWhile final : public AstNodePoplarProgram {
    // A poplar::program::RepeatWhileFalse or RepeatWhileTrue
    // @astgen op1 := preCondp  : Optional[AstNodePoplarProgram]
    // @astgen op2 := condp     : AstVarRef
    // @astgen op3 := bodyp     : AstNodePoplarProgram
private:
    bool m_isWhileFalse = false;

public:
    AstPoplarWhile(FileLine* fl, AstVarRef* condp, AstNodePoplarProgram* bodyp,
                   bool isWhileFalse = false, AstNodePoplarProgram* preCondp = nullptr,
                   const std::string& debugContext = "")
        : ASTGEN_SUPER_PoplarWhile(fl, debugContext)
        , m_isWhileFalse(isWhileFalse) {
        this->condp(condp);
        this->bodyp(bodyp);
        this->preCondp(preCondp);
    }
    ASTGEN_MEMBERS_AstPoplarWhile;

    bool whileFalse() const { return m_isWhileFalse; }
    void whileFalse(bool isFalse) { m_isWhileFalse = isFalse; }

};


#endif
