
// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: IPU cost model (generated)
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
#ifndef VERILATOR_V3BSPIPUCOSTMODEL_H_
#define VERILATOR_V3BSPIPUCOSTMODEL_H_
#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"

class IpuCostModelLinReg final {
public:
    static inline std::pair<int, bool> tryEstimate(AstNode* nodep, bool useMean = false);
    static inline int estimate(AstNode* nodep, bool useMean = false) {
        return tryEstimate(nodep, useMean).first;
    }
};
namespace {
class IpuCostModelGen final : public VNVisitor {
public:
    int m_count = 0;
    bool m_found = true;
    const bool m_useMean;
    explicit IpuCostModelGen(AstNode* nodep, bool useMean = false)
        : m_useMean{useMean} {
        iterate(nodep);
    }

private:
    inline bool isQData(AstNode* nodep) const { return nodep->isQuad(); }
    inline bool isVlWide(AstNode* nodep) const { return nodep->isWide(); }
    inline bool isEData(AstNode* nodep) const { return nodep->widthWords() == 1; }
    inline bool useMean() const { return m_useMean; }
    inline int defaultLatency(AstNode* nodep) {
        m_found = false;
        return 14;
    }
    inline void set(int m) { m_count = m; }
    void visit(AstExtendS* nodep) {
        if (isEData(nodep) && isEData(nodep->lhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(24.08));
            else
                return set(static_cast<uint32_t>(24.08f));
        if (isQData(nodep) && isEData(nodep->lhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(17.73));
            else
                return set(static_cast<uint32_t>(17.73f));
        if (isQData(nodep) && isQData(nodep->lhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(25.10));
            else
                return set(static_cast<uint32_t>(25.10f));
        if (isVlWide(nodep) && isEData(nodep->lhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(35.30));
            else
                return set(static_cast<uint32_t>((0.89f * nodep->widthWords()) + 25.81f));
        if (isVlWide(nodep) && isQData(nodep->lhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(20.00));
            else
                return set(static_cast<uint32_t>((1.09f * nodep->widthWords()) + 9.68f));
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(86.83));
            else
                return set(static_cast<uint32_t>((0.21f * nodep->widthWords())
                                                 + (6.81f * nodep->lhsp()->widthWords())
                                                 + 23.92f));
        return set(defaultLatency(nodep));
    }
    void visit(AstRedAnd* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(13.00));
            else
                return set(static_cast<uint32_t>(13.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(23.00));
            else
                return set(static_cast<uint32_t>(23.00f));
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(64.32));
            else
                return set(static_cast<uint32_t>((6.14f * nodep->lhsp()->widthWords()) + 7.61f));
        return set(defaultLatency(nodep));
    }
    void visit(AstRedOr* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(13.00));
            else
                return set(static_cast<uint32_t>(13.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(20.00));
            else
                return set(static_cast<uint32_t>(20.00f));
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(1891.66));
            else
                return set(
                    static_cast<uint32_t>((225.90f * nodep->lhsp()->widthWords()) + -193.84f));
        return set(defaultLatency(nodep));
    }
    void visit(AstRedXor* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(2105.57));
            else
                return set(
                    static_cast<uint32_t>((236.98f * nodep->lhsp()->widthWords()) + -136.98f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(19.00));
            else
                return set(static_cast<uint32_t>(19.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstCountOnes* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(86.00));
            else
                return set(static_cast<uint32_t>(86.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(93.00));
            else
                return set(static_cast<uint32_t>(93.00f));
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(519.34));
            else
                return set(
                    static_cast<uint32_t>((58.06f * nodep->lhsp()->widthWords()) + -59.33f));
        return set(defaultLatency(nodep));
    }
    void visit(AstAnd* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(117.96));
            else
                return set(static_cast<uint32_t>((14.00f * nodep->lhsp()->widthWords()) + -0.00f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(16.00));
            else
                return set(static_cast<uint32_t>(16.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstOr* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(117.96));
            else
                return set(static_cast<uint32_t>((14.00f * nodep->lhsp()->widthWords()) + -0.00f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(16.00));
            else
                return set(static_cast<uint32_t>(16.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstXor* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(117.96));
            else
                return set(static_cast<uint32_t>((14.00f * nodep->lhsp()->widthWords()) + -0.00f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(16.00));
            else
                return set(static_cast<uint32_t>(16.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstNot* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(109.54));
            else
                return set(static_cast<uint32_t>((13.00f * nodep->lhsp()->widthWords()) + 0.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstGt* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(192.17));
            else
                return set(static_cast<uint32_t>((23.17f * nodep->rhsp()->widthWords()) + -3.06f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(23.00));
            else
                return set(static_cast<uint32_t>(23.00f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstLt* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(200.35));
            else
                return set(static_cast<uint32_t>((23.24f * nodep->rhsp()->widthWords()) + 4.51f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(23.00));
            else
                return set(static_cast<uint32_t>(23.00f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstEq* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(69.06));
            else
                return set(static_cast<uint32_t>((6.35f * nodep->rhsp()->widthWords()) + 15.58f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(32.00));
            else
                return set(static_cast<uint32_t>(32.00f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstNeq* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(69.06));
            else
                return set(static_cast<uint32_t>((6.35f * nodep->rhsp()->widthWords()) + 15.58f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(32.00));
            else
                return set(static_cast<uint32_t>(32.00f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstGtS* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(31.38));
            else
                return set(static_cast<uint32_t>(31.38f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(47.04));
            else
                return set(static_cast<uint32_t>(47.04f));
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(356.24));
            else
                return set(static_cast<uint32_t>((37.58f * nodep->rhsp()->widthWords()) + 39.60f));
        return set(defaultLatency(nodep));
    }
    void visit(AstGteS* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(37.38));
            else
                return set(static_cast<uint32_t>(37.38f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(50.04));
            else
                return set(static_cast<uint32_t>(50.04f));
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(227.11));
            else
                return set(static_cast<uint32_t>((22.51f * nodep->rhsp()->widthWords()) + 37.45f));
        return set(defaultLatency(nodep));
    }
    void visit(AstLtS* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(31.38));
            else
                return set(static_cast<uint32_t>(31.38f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(47.04));
            else
                return set(static_cast<uint32_t>(47.04f));
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(220.37));
            else
                return set(static_cast<uint32_t>((22.54f * nodep->rhsp()->widthWords()) + 30.44f));
        return set(defaultLatency(nodep));
    }
    void visit(AstLteS* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(37.38));
            else
                return set(static_cast<uint32_t>(37.38f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(50.04));
            else
                return set(static_cast<uint32_t>(50.04f));
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(349.06));
            else
                return set(static_cast<uint32_t>((36.32f * nodep->rhsp()->widthWords()) + 43.06f));
        return set(defaultLatency(nodep));
    }
    void visit(AstNegate* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(198.65));
            else
                return set(
                    static_cast<uint32_t>((25.00f * nodep->lhsp()->widthWords()) + -12.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstAdd* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(185.22));
            else
                return set(
                    static_cast<uint32_t>((24.00f * nodep->lhsp()->widthWords()) + -17.00f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(26.00));
            else
                return set(static_cast<uint32_t>(26.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstSub* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(200.22));
            else
                return set(static_cast<uint32_t>((24.00f * nodep->lhsp()->widthWords()) + -2.00f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(22.00));
            else
                return set(static_cast<uint32_t>(22.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstMul* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(9814.24));
            else
                return set(
                    static_cast<uint32_t>((2338.45f * nodep->lhsp()->widthWords()) + -9889.34f));
        if (isEData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(14.00));
            else
                return set(static_cast<uint32_t>(14.00f));
        if (isQData(nodep))
            if (useMean())
                return set(static_cast<uint32_t>(72.00));
            else
                return set(static_cast<uint32_t>(72.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstShiftL* nodep) {
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(211.55));
            else
                return set(static_cast<uint32_t>(
                    (9.08f * nodep->widthWords()) + (0.26f * nodep->lhsp()->widthWords())
                    + (0.26f * nodep->rhsp()->widthWords()) + 122.34f));
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(265.15));
            else
                return set(static_cast<uint32_t>(
                    (8.60f * nodep->widthWords()) + (-0.22f * nodep->lhsp()->widthWords())
                    + (-0.22f * nodep->rhsp()->widthWords()) + 196.72f));
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(213.78));
            else
                return set(static_cast<uint32_t>(
                    (9.39f * nodep->widthWords()) + (-0.33f * nodep->lhsp()->widthWords())
                    + (-0.33f * nodep->rhsp()->widthWords()) + 134.32f));
        if (isEData(nodep) && isEData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(83.23));
            else
                return set(static_cast<uint32_t>(83.23f));
        if (isEData(nodep) && isEData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(35.45));
            else
                return set(static_cast<uint32_t>(35.45f));
        if (isQData(nodep) && isQData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(106.13));
            else
                return set(static_cast<uint32_t>(106.13f));
        if (isQData(nodep) && isQData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(58.47));
            else
                return set(static_cast<uint32_t>(58.47f));
        if (isQData(nodep) && isQData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(44.00));
            else
                return set(static_cast<uint32_t>(44.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstShiftR* nodep) {
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(205.27));
            else
                return set(static_cast<uint32_t>(
                    (10.71f * nodep->widthWords()) + (0.03f * nodep->lhsp()->widthWords())
                    + (0.03f * nodep->rhsp()->widthWords()) + 104.89f));
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(257.45));
            else
                return set(static_cast<uint32_t>(
                    (9.48f * nodep->widthWords()) + (-0.32f * nodep->lhsp()->widthWords())
                    + (-0.32f * nodep->rhsp()->widthWords()) + 183.32f));
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(211.30));
            else
                return set(static_cast<uint32_t>(
                    (10.42f * nodep->widthWords()) + (0.23f * nodep->lhsp()->widthWords())
                    + (0.23f * nodep->rhsp()->widthWords()) + 110.91f));
        if (isEData(nodep) && isEData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(83.23));
            else
                return set(static_cast<uint32_t>(83.23f));
        if (isEData(nodep) && isEData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(35.45));
            else
                return set(static_cast<uint32_t>(35.45f));
        if (isQData(nodep) && isQData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(106.13));
            else
                return set(static_cast<uint32_t>(106.13f));
        if (isQData(nodep) && isQData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(58.47));
            else
                return set(static_cast<uint32_t>(58.47f));
        if (isQData(nodep) && isQData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(45.00));
            else
                return set(static_cast<uint32_t>(45.00f));
        return set(defaultLatency(nodep));
    }
    void visit(AstShiftRS* nodep) {
        if (isEData(nodep) && isEData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(36.37));
            else
                return set(static_cast<uint32_t>(36.37f));
        if (isQData(nodep) && isQData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(74.60));
            else
                return set(static_cast<uint32_t>(74.60f));
        if (isEData(nodep) && isQData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(70.92));
            else
                return set(static_cast<uint32_t>(70.92f));
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(249.68));
            else
                return set(static_cast<uint32_t>(
                    (9.07f * nodep->widthWords()) + (0.04f * nodep->lhsp()->widthWords())
                    + (0.04f * nodep->rhsp()->widthWords()) + 164.43f));
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(289.63));
            else
                return set(static_cast<uint32_t>(
                    (8.57f * nodep->widthWords()) + (-0.35f * nodep->lhsp()->widthWords())
                    + (-0.35f * nodep->rhsp()->widthWords()) + 223.60f));
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(250.32));
            else
                return set(static_cast<uint32_t>(
                    (10.09f * nodep->widthWords()) + (0.21f * nodep->lhsp()->widthWords())
                    + (0.21f * nodep->rhsp()->widthWords()) + 153.37f));
        if (isQData(nodep) && isQData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(136.32));
            else
                return set(static_cast<uint32_t>(136.32f));
        if (isEData(nodep) && isEData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(56.58));
            else
                return set(static_cast<uint32_t>(56.58f));
        if (isQData(nodep) && isQData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(static_cast<uint32_t>(90.65));
            else
                return set(static_cast<uint32_t>(90.65f));
        return set(defaultLatency(nodep));
    }

    void visit(AstNode* nodep) { set(std::max(defaultLatency(nodep), nodep->instrCount())); }
};
}  // namespace
std::pair<int, bool> IpuCostModelLinReg::tryEstimate(AstNode* nodep, bool useMean) {
    IpuCostModelGen c{nodep, useMean};
    return {c.m_count, c.m_found};
}
#endif
