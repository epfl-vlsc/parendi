// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator BSP: handle BSP class with DPI calls
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2005-2023 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
#ifndef VERILATOR_V3BSPIPUPROFILE_H
#define VERILATOR_V3BSPIPUPROFILE_H

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"


//============================================================================

class V3BspIpuProfile final {
public:
    static void instrument(AstNetlist* nodep);
};

#endif  // Guard