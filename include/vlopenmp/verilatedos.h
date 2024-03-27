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
/// \brief Verilated/Verilator common header for OS portability
///
/// This header is included by user wrappers and defines the Verilated
/// public-facing API.
///
/// User wrapper code does not generally need to include this, instead
/// include verilated.h.
///
/// This header is used by both the Verilator source code (run on the build
/// and host system), and the Verilated output (run on the target system).
///
/// Configuration code needed by only the host system is in
/// config_build.h.in, code needed by Verilated code only is in
/// verilated.h, and code needed by both is here (verilatedos.h).
///
//*************************************************************************

#define VL_USER_FINISH  ///< Define this to override the vl_finish function

#include "../verilatedos.h"
