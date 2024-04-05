// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: AstNode sub-types representing OpenMP constructs
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

#ifndef VERILATOR_V3ASTNODESOPENMP_H_
#define VERILATOR_V3ASTNODESOPENMP_H_

#ifndef VERILATOR_V3AST_H_
#error "Use V3Ast.h as the include"
#include "V3Ast.h"  // This helps code analysis tools pick up symbols in V3Ast.h
#define VL_NOT_FINAL  // This #define fixes broken code folding in the CLion IDE
#endif

// === Abstract base node types (AstNode*) =====================================

class AstNodeOpenMPProgram VL_NOT_FINAL : public AstNode {
    // Abstract OpenMP program
    std::string m_debugContext;  // debug context to pass to the openmp API

protected:
    AstNodeOpenMPProgram(VNType t, FileLine* fl, const std::string& debugContext)
        : AstNode{t, fl}
        , m_debugContext(debugContext) {}

public:
    ASTGEN_MEMBERS_AstNodeOpenMPProgram;
    void debugContext(const std::string& ctx) { m_debugContext = ctx; }
    std::string debugContext() const { return m_debugContext; }
};

// === Concrete node types

// === AstNodeOpenMPProgram ===

class AstOpenMPCopy final : public AstNodeOpenMPProgram {
    // An openmp::program::Copy operation
    // Parents: OPENMPSEQUENCE
    // @astgen op1 := fromp  : AstVarRef // tensor to copy from
    // @astgen op2 := top    : AstVarRef // tensor to copy to
private:
    bool m_dontOutline = false;  // do not outline this copy as a function call

public:
    AstOpenMPCopy(FileLine* fl, AstVarRef* fromp, AstVarRef* top, bool dontOutline = false,
                  const std::string& debugContext = "")
        : ASTGEN_SUPER_OpenMPCopy(fl, debugContext) {
        UASSERT(fromp->dtypep()->basicp()->keyword() == VBasicDTypeKwd::POPLAR_TENSOR,
                "expcted fromp of POPLAR_TENSOR type");
        UASSERT(top->dtypep()->basicp()->keyword() == VBasicDTypeKwd::POPLAR_TENSOR,
                "expected top of POPLAR_TENSOR type");
        this->fromp(fromp);
        this->top(top);
    }
    
    ASTGEN_MEMBERS_AstOpenMPCopy;
    
    // void dump(std::ostream& str) const override;
    bool dontOutline() const { return m_dontOutline; }
    void dontOutline(bool dontOutline) { m_dontOutline = dontOutline; }
};

class AstOpenMPExecute final : public AstNodeOpenMPProgram {
    // An openmp::program::Execute operation
    // @astgen op1 := computeSetp : AstVarRef
public:
    AstOpenMPExecute(FileLine* fl, AstVarRef* csp, const std::string& debugContext = "")
        : ASTGEN_SUPER_OpenMPExecute(fl, debugContext) {
        UASSERT(csp->dtypep()->basicp()->keyword() == VBasicDTypeKwd::POPLAR_COMPUTESET,
                "invalid type");
        this->computeSetp(csp);
    }

    ASTGEN_MEMBERS_AstOpenMPExecute;
};

class AstOpenMPSequence final : public AstNodeOpenMPProgram {
    // An openmp::program::Sequence
    // @astgen op1 :=  progsp    : List[AstNodeOpenMPProgram]
public:
    AstOpenMPSequence(FileLine* fl, AstNodeOpenMPProgram* progsp,
                      const std::string& debugContext = "")
        : ASTGEN_SUPER_OpenMPSequence(fl, debugContext) {
        this->addProgsp(progsp);
    }

    ASTGEN_MEMBERS_AstOpenMPSequence;
};

class AstOpenMPWhile final : public AstNodeOpenMPProgram {
    // An openmp::program::RepeatWhileFalse or RepeatWhileTrue
    // @astgen op1 := preCondp  : Optional[AstNodeOpenMPProgram]
    // @astgen op2 := condp     : AstVarRef
    // @astgen op3 := bodyp     : AstNodeOpenMPProgram
private:
    bool m_isWhileFalse = false;

public:
    AstOpenMPWhile(FileLine* fl, AstVarRef* condp, AstNodeOpenMPProgram* bodyp,
                   bool isWhileFalse = false, AstNodeOpenMPProgram* preCondp = nullptr,
                   const std::string& debugContext = "")
        : ASTGEN_SUPER_OpenMPWhile(fl, debugContext)
        , m_isWhileFalse(isWhileFalse) {
        this->condp(condp);
        this->bodyp(bodyp);
        this->preCondp(preCondp);
    }

    ASTGEN_MEMBERS_AstOpenMPWhile;

    bool whileFalse() const { return m_isWhileFalse; }
    void whileFalse(bool isFalse) { m_isWhileFalse = isFalse; }
};

#endif
