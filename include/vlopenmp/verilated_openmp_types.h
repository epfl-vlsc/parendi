// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
//
// Code available from: https://verilator.org
//
// Copyright 2003-2023 by Wilson Snyder. This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
///
/// \file
/// \brief Verilated OpenMP data types
///
//*************************************************************************

#ifndef VERILATOR_VERILATED_OPENMP_TYPES_H_
#define VERILATOR_VERILATED_OPENMP_TYPES_H_

#ifndef VERILATOR_VERILATED_H_INTERNAL_
#error "verilated_openmp_types.h should only be included by verilated.h"
#endif

class MemoryContents;

class CodeletBase {
public:
    virtual void nbaTop(const VlTriggerVec<1>& __VBspMember_trigArg__0,
                        const MemoryContents& memIn, MemoryContents& memOut)
        = 0;
    virtual VlTriggerVec<1> triggerEval(const MemoryContents& memIn, MemoryContents& memOut) = 0;
    virtual void compute(const MemoryContents& memIn, MemoryContents& memOut) = 0;
};

#include "verilated_openmp_context.h"
class OpenMPRoot {
public:
    int m_numCores = -1;
    CodeletBase** m_codelets = nullptr;
    CodeletBase** m_initCodelets = nullptr;

    int NUM_CORES;

    void setNumCores(int nc) {
        m_numCores = nc;
        if (m_codelets) { delete (m_codelets); }
        m_codelets = new CodeletBase*[nc];
        if (m_initCodelets) { delete (m_initCodelets); }
        m_initCodelets = new CodeletBase*[nc];
    }
    int numCores() { return m_numCores; }
    void setCodelets(CodeletBase** codelets) { m_codelets = codelets; }
    CodeletBase** codelets() { return m_codelets; }
    void setinitCodelets(CodeletBase** codelets) { m_initCodelets = codelets; }
    CodeletBase** initCodelets() { return m_initCodelets; }

    void exchange();
    void initialize();
    void constructAll();

    OpenMPRoot() {}
};

#endif  // Guard
