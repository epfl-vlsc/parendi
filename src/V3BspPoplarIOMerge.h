// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Merge Input fields in each vertex
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

#ifndef VERILATOR_V3BSPPOPLARIOMERGE_H_
#define VERILATOR_V3BSPPOPLARIOMERGE_H_

#include "config_build.h"
#include "verilatedos.h"


class AstNetlist;

class V3BspPoplarIOMerge final {
public:
    static void mergeIO(AstNetlist* nodep);
};


#endif