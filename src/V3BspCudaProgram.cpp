// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Lower the program tree create a poplar program
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

#include "V3BspCudaProgram.h"

#include "V3Ast.h"
#include "V3AstUserAllocator.h"
#include "V3EmitCBase.h"
#include "V3Global.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

void V3BspCudaProgram::createProgram(AstNetlist* nodep) {
    UINFO(3, "Creating cuda program" << endl);
}
