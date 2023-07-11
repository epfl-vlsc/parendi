// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: BSP retiming

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
//

#ifndef VERILATOR_V3BSPRETIMING_H_
#define VERILATOR_V3BSPRETIMING_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3BspGraph.h"
#include "V3BspNetlistGraph.h"
#include "V3Sched.h"

namespace V3BspSched {
namespace Retiming {

// std::vector<std::unique_ptr<NetlistGraph>>
// buildNetlistGraphs(const std::vector<std::unique_ptr<DepGraph>>& partitionsp);

void retimeAll(AstNetlist* netlistp);

};  // namespace Retiming
};  // namespace V3BspSched
#endif