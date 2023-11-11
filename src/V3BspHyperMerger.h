// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Merge and balance BSP using hypergraph partitioning
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
//

#ifndef VERILATOR_V3BSPHYPERMERGER_H_
#define VERILATOR_V3BSPHYPERMERGER_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3BspGraph.h"
#include "V3Sched.h"

#include <climits>
#include <limits>
namespace V3BspSched {

class V3BspHyperMerger final {
public:
    static void mergeAll(std::vector<std::unique_ptr<DepGraph>>& partitionsp, uint32_t numTiles,
                         uint32_t numWorkers);
};
}  // namespace V3BspSched

#endif