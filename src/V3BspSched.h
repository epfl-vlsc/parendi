// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Bulk-synchronous Parallel Scheduling
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

#ifndef VERILATOR_BSP_SCHED_H
#define VERILATOR_BSP_SCHED_H

#include "config_build.h"
#include "verilatedos.h"

#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

class AstNetlist;
//=============================================================================
namespace V3BspSched {

void schedule(AstNetlist*);

};

#endif