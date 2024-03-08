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
/// \brief Verilated common data type containers
///
/// verilated.h should be included instead of this file.
///
/// Those macro/function/variable starting or ending in _ are internal,
/// however many of the other function/macros here are also internal.
///
//*************************************************************************

#ifndef OPENMP_VERILATED_TYPES_H_
#define OPENMP_VERILATED_TYPES_H_


class MemoryContents;
class CodeletBase {
    void nbaTop(const VlTriggerVec<1> &__VBspMember_trigArg__0, const MemoryContents &memIn, MemoryContents &memOut);
    VlTriggerVec<1> triggerEval();
    void compute(const MemoryContents &memIn, MemoryContents &memOut);
};

#endif  // Guard
