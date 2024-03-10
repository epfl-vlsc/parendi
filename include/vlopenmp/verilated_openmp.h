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
/// \brief Verilated header for OpenMP.
///
//*************************************************************************

#ifndef VERILATOR_OPENMP_H_
#define VERILATOR_OPENMP_H_
#define VERILATOR_OPENMP_H_INTERNAL_

#include "vlopenmp/verilatedos.h"

//=========================================================================
// Data Types

//=========================================================================
// Basic types

#include <cstdint>

// clang-format off
//    P                     // Packed data of bit type (C/S/I/Q/W)
using CData = uint8_t;    ///< Data representing 'bit' of 1-8 packed bits
using SData = uint16_t;   ///< Data representing 'bit' of 9-16 packed bits
using IData = uint32_t;   ///< Data representing 'bit' of 17-32 packed bits
using QData = uint64_t;   ///< Data representing 'bit' of 33-64 packed bits
using EData = uint32_t;   ///< Data representing one element of WData array
using WData = EData;        ///< Data representing >64 packed bits (used as pointer)
//    F     = float;        // No typedef needed; Verilator uses float
//    D     = double;       // No typedef needed; Verilator uses double
//    N     = std::string;  // No typedef needed; Verilator uses string
// clang-format on

#include "vlopenmp/verilated_openmp_context.h"
#include "vlopenmp/verilated_openmp_types.h"

//=========================================================================
// Functions

/// Print a debug message from internals with standard prefix, with printf style format
#define VL_DBG_MSGF(fmt, args...) printf("[DBG]" fmt, ##args)

#ifdef VL_DEBUG
/// Evaluate statement if VL_DEBUG defined
#define VL_DEBUG_IFDEF(stmt) \
    do { stmt } while (false)
/// Evaluate statement if VL_DEBUG defined and Verilated::debug() enabled
#define VL_DEBUG_IF(stmt) \
    do { stmt } while (false)
#else
// We intentionally do not compile the stmt to improve compile speed
#define VL_DEBUG_IFDEF(stmt) \
    do { \
    } while (false)
#define VL_DEBUG_IF(stmt) \
    do { \
    } while (false)
#endif

#define VL_WRITEF(fmt, args...) printf("[DBG]" fmt, ##args)

#define VL_FINISH_MT(filename, linenum, hier) printf("Finished! %s:%d", filename, linenum)

#undef VERILATOR_OPENMP_H_INTERNAL_
#endif  // Guard
