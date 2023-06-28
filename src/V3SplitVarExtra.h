// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Try to split more variables (automatically)
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
#ifndef VERILATOR_V3SPLITVAREXTRA_H_
#define VERILATOR_V3SPLITVAREXTRA_H_

class AstNetlist;

class V3SplitVarExtra final {
public:
    // Analyze the netlist and automatically mark some variables for splitting
    static void splitVariableExtra(AstNetlist* nodep);
};

#endif