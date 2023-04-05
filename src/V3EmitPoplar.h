// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit C++ (poplar)
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

#ifndef VERILATOR_V3EMITPOPLAR_H_
#define VERILATOR_V3EMITPOPLAR_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3File.h"

// class V3EmitPoplarBaseVisitor VL_NOT_FINAL : public VNVisitor {
// protected:
//     V3OutCFile* m_ofp = nullptr;
//     V3OutCFile* ofp() const VL_MT_SAFE { return m_ofp; }
//     void puts(const string& str) { ofp()->puts(str); }
//     void putbs(const string& str) { ofp()->putbs(str); }
//     void putsDecoration(const string& str) {
//         if (v3Global.opt.decoration()) { puts(str); }
//     }
//     void putsQuoted(const string& str) { ofp()->putsQuoted(str); }
//     void ensureNewLine() { ofp()->ensureNewLine(); }

// public:
//     V3EmitPoplarBaseVisitor(V3OutCFile* ofp) : m_ofp{ofp} {}
//     ~V3EmitPoplarBaseVisitor() override = default;
// };

// class V3EmitPoplarVertex final {
//     static void emit();
// };

class V3EmitPoplar final {
public:
    static void emitVertex();

};

#endif