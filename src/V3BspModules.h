// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: BSP module maker
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

#ifndef VERILATOR_V3BSPMODULES_H
#define VERILATOR_V3BSPMODULES_H
#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3BspGraph.h"
#include "V3Sched.h"
class DepGraph;
class AstNetlist;
class AstModule;

namespace V3BspSched {

class V3BspModules final {
public:
    static void makeModules(AstNetlist* origp,
                            const std::vector<std::unique_ptr<DepGraph>>& partitionsp,
                            const V3Sched::LogicByScope& initials,
                            const V3Sched::LogicByScope& statics);
};
};  // namespace V3BspSched
#endif