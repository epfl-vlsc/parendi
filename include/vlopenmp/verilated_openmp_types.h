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
/// \brief Verilated OpenMP common data type containers
///
/// verilated_openmp.h should be included instead of this file.
///
/// Those macro/function/variable starting or ending in _ are internal,
/// however many of the other function/macros here are also internal.
///
//*************************************************************************

#ifndef VERILATED_OPENMP_TYPES_H_
#define VERILATED_OPENMP_TYPES_H_

#include <vlopenmp/verilated_openmp_context.h>

#include <array>

//===================================================================
// Activity trigger vector

template <std::size_t T_size>  //
class VlTriggerVec final {
    // TODO: static assert T_size > 0, and don't generate when empty
private:
    // MEMBERS
    std::array<uint32_t, T_size> m_flags;  // State of the assoc array

public:
    // CONSTRUCTOR
    VlTriggerVec() { clear(); }
    ~VlTriggerVec() = default;

    // METHODS

    // Set all elements to false
    void clear() { m_flags.fill(false); }

    bool at(size_t index) const { return m_flags.at(index); }

    // Return true iff at least one element is set
    bool any() const {
        for (size_t i = 0; i < m_flags.size(); ++i)
            if (m_flags[i]) return true;
        return false;
    }
    bool empty() const { return !any(); }

    void set(uint32_t index, bool value) { m_flags[index] = value; }
};

class MemoryContents;  // Defined in application
class CodeletBase {
    void nbaTop(const VlTriggerVec<1>& __VBspMember_trigArg__0, const MemoryContents& memIn,
                MemoryContents& memOut);
    VlTriggerVec<1> triggerEval();
    void compute(const MemoryContents& memIn, MemoryContents& memOut);
};
class OpenMPRoot {
public:
    VlOpenMPContext* m_ctx;
    int m_numCores = -1;
    CodeletBase** m_codelets = nullptr;
    CodeletBase** m_initCodelets = nullptr;

    int NUM_CORES;

    void setCtx(VlOpenMPContext* ctx) { m_ctx = ctx; }
    VlOpenMPContext* ctx() { return m_ctx; }

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
