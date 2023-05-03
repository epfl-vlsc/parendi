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
/// \brief Verilated common header, include for all Verilated C files
///
/// This file is included automatically by Verilator at the top of all C++
/// files it generates.  It contains standard macros and classes required
/// by the Verilated code.
///
/// User wrapper code may need to include this to get appropriate
/// structures, however they would generally just include the
/// Verilated-model's header instead (which then includes this).
///
/// Those macro/function/variable starting or ending in _ are internal,
/// however many of the other function/macros here are also internal.
///
//*************************************************************************

#ifndef VERILATOR_VERILATED_H_
#define VERILATOR_VERILATED_H_
#define VERILATOR_VERILATED_H_INTERNAL_


#include "vlpoplar/verilatedos.h"
#include "verilated_config.h"
#include <print.h> // poplar print
#include <cmath>

//=========================================================================
// Basic types

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

using WDataInP = const WData*;  ///< 'bit' of >64 packed bits as array input to a function
using WDataOutP = WData*;  ///< 'bit' of >64 packed bits as array output from a function

enum VerilatedVarType : uint8_t {
    VLVT_UNKNOWN = 0,
    VLVT_PTR,  // Pointer to something
    VLVT_UINT8,  // AKA CData
    VLVT_UINT16,  // AKA SData
    VLVT_UINT32,  // AKA IData
    VLVT_UINT64,  // AKA QData
    VLVT_WDATA,  // AKA WData
    VLVT_STRING  // C++ string
};

enum VerilatedVarFlags {
    VLVD_0 = 0,  // None
    VLVD_IN = 1,  // == vpiInput
    VLVD_OUT = 2,  // == vpiOutput
    VLVD_INOUT = 3,  // == vpiInOut
    VLVD_NODIR = 5,  // == vpiNoDirection
    VLVF_MASK_DIR = 7,  // Bit mask for above directions
    // Flags
    VLVF_PUB_RD = (1 << 8),  // Public readable
    VLVF_PUB_RW = (1 << 9),  // Public writable
    VLVF_DPI_CLAY = (1 << 10)  // DPI compatible C standard layout
};


//=========================================================================
// Functions overridable by user defines
// (Internals however must use VL_PRINTF_MT, which calls these.)

// clang-format off
#ifndef VL_PRINTF
# define VL_PRINTF printf  ///< Print ala printf, called from main thread; redefine if desired
#endif
#ifndef VL_VPRINTF
# define VL_VPRINTF vprintf  ///< Print ala vprintf, called from main thread; redefine if desired
#endif
// clang-format on

//=========================================================================
// Data Types

#include "vlpoplar/verilated_types.h"

//=========================================================================
// Functions

#include "vlpoplar/verilated_funcs.h"

#undef VERILATOR_VERILATED_H_INTERNAL_
#endif  // Guard
