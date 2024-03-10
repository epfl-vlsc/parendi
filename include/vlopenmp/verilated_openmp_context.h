// -*- mode: C++; c-file-style: "cc-mode" -*-
//=============================================================================
//
// Code available from: https://verilator.org
//
// Copyright 2001-2023 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//=============================================================================
///
/// \file  verilated_openmp_context.h
/// \brief Verilated OpenMP context implementation
///
//=============================================================================

#ifndef VERILATOR_POPLARCONTEXT_H_
#define VERILATOR_POPLARCONTEXT_H_

#include <vlopenmp/verilated_openmp.h>

///
/// VlPoplarContext is responsbile for create a poplar CDFG, currently we treat
/// computation in two distict phases: initial and nba.
/// The initial phase runs before nba to completion and the nba is a loop.
/// There is a special host request variable, let's call it hasDpi that can
/// break the flow of execution. The initial phase has the following structure:
///
///     init cache for $plusargs and maybe $readmem
///     copy cache to device
///     hasDpi = 0;
///     forall dpiVec: dpiVec = 0;
///     on the host:
///             do:
///                IPU| Execute(initComputeSet);
///                IPU| dpiExchange;
///                IPU| dpiEval;
///                IPU| dpiBroadcast;
///                hostHandle();
///             while hasDpi
///             IPU| initExchage;
///
///
/// The nba phase is slightly more complicated:
///
///     on the host:
///         clear hasDpi; clear dpiVec
///         let simLoop be:
///         IPU|    dpiBroadcast
///         IPU|    Execute(nba)
///         IPU|    while !hasDpi:
///         IPU|        (pre) dpiExchange
///         IPU|        (pre) dpiEval
///         IPU|        nbaExchange
///         IPU|        Execute(nba)
///
///         while !finished:
///             IPU| dpiEval
///             IPU| dpiBroadcast
///             IPU| if hasDpi:
///             IPU|    Execute(nba)
///             IPU|    dpiExchange
///             IPU|    dpiEval
///             IPU|    if !hasDpi:
///             IPU|        nbaExchange
///             IPU| if !hasDpi:
///             IPU|        simLoop
///             hostHandle();

class VlOpenMPContext final {

};
#endif
