
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
        return nodep->widthWords();
    }
    void visit(AstCCast*) override { set(0); }
    void visit(AstVarRefView*) override { set(0); }
    void visit(AstNodeVarRef* nodep) override {
        if (const AstCMethodHard* const callp = VN_CAST(nodep->backp(), CMethodHard)) {
            if (callp->fromp() == nodep) return set(1);
        }
        if (nodep->varp()->isFuncLocal()) {
            return set(nodep->widthWords());
        } else {
            return set(nodep->widthWords() + 1);
        }
    }

    inline void set(float m) { m_count = static_cast<int>(std::round(m)); }
    void visit(AstNodeIf* nodep) override { set(6); }
    void visit(AstNodeCond* nodep) override {
        if (AstNodeAssign* const assignp = VN_CAST(nodep->backp(), NodeAssign)) {
            AstNodeVarRef* const lvp = VN_CAST(assignp->lhsp(), NodeVarRef);
            AstNodeVarRef* const elsep = VN_CAST(nodep->elsep(), NodeVarRef);
            if (lvp && elsep && lvp->varp() == elsep->varp()) {
                set(3);  // can become movz
                return;
            }
        }
        set(6);
    }
    void visit(AstExtendS* nodep) {
        if (isEData(nodep) && isEData(nodep->lhsp()))
            if (useMean())
                return set(4.93);
            else
                return set(4.93f);
        if (isQData(nodep) && isEData(nodep->lhsp()))
            if (useMean())
                return set(16.17);
            else
                return set(16.17f);
        if (isQData(nodep) && isQData(nodep->lhsp()))
            if (useMean())
                return set(6.93);
            else
                return set(6.93f);
        if (isVlWide(nodep) && isEData(nodep->lhsp()))
            if (useMean())
                return set(15.60);
            else
                return set((1.00f * nodep->widthWords()) + 5.00f);
        if (isVlWide(nodep) && isQData(nodep->lhsp()))
            if (useMean())
                return set(21.65);
            else
                return set((0.56f * nodep->widthWords()) + 16.33f);
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()))
            if (useMean())
                return set(24.33);
            else
                return set((0.55f * nodep->widthWords()) + (1.49f * nodep->lhsp()->widthWords())
                           + 6.01f);
        return set(defaultLatency(nodep));
    }
    void visit(AstRedAnd* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(3.07);
            else
                return set(3.07f);
        if (isQData(nodep))
            if (useMean())
                return set(10.17);
            else
                return set(10.17f);
        if (isVlWide(nodep))
            if (useMean())
                return set(20.52);
            else
                return set((1.99f * nodep->lhsp()->widthWords()) + 2.19f);
        return set(defaultLatency(nodep));
    }
    void visit(AstRedOr* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(3.00);
            else
                return set(3.00f);
        if (isQData(nodep))
            if (useMean())
                return set(8.00);
            else
                return set(8.00f);
        if (isVlWide(nodep))
            if (useMean())
                return set(410.17);
            else
                return set((64.71f * nodep->lhsp()->widthWords()) + -35.59f);
        return set(defaultLatency(nodep));
    }
    void visit(AstRedXor* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(636.56);
            else
                return set((71.63f * nodep->lhsp()->widthWords()) + -24.73f);
        if (isEData(nodep))
            if (useMean())
                return set(4.00);
            else
                return set(4.00f);
        if (isQData(nodep))
            if (useMean())
                return set(7.00);
            else
                return set(7.00f);
        return set(defaultLatency(nodep));
    }
    void visit(AstCountOnes* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(18.17);
            else
                return set(18.17f);
        if (isQData(nodep))
            if (useMean())
                return set(53.17);
            else
                return set(53.17f);
        if (isVlWide(nodep))
            if (useMean())
                return set(200.68);
            else
                return set((20.99f * nodep->lhsp()->widthWords()) + -8.49f);
        return set(defaultLatency(nodep));
    }
    void visit(AstAnd* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(33.87);
            else
                return set((4.00f * nodep->lhsp()->widthWords()) + 0.17f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        if (isQData(nodep))
            if (useMean())
                return set(8.17);
            else
                return set(8.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstOr* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(33.87);
            else
                return set((4.00f * nodep->lhsp()->widthWords()) + 0.17f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        if (isQData(nodep))
            if (useMean())
                return set(8.17);
            else
                return set(8.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstXor* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(33.87);
            else
                return set((4.00f * nodep->lhsp()->widthWords()) + 0.17f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        if (isQData(nodep))
            if (useMean())
                return set(8.17);
            else
                return set(8.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstNot* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(25.28);
            else
                return set((3.00f * nodep->lhsp()->widthWords()) + -0.00f);
        if (isEData(nodep))
            if (useMean())
                return set(3.00);
            else
                return set(3.00f);
        if (isQData(nodep))
            if (useMean())
                return set(6.00);
            else
                return set(6.00f);
        return set(defaultLatency(nodep));
    }
    void visit(AstGt* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(56.96);
            else
                return set((6.99f * nodep->rhsp()->widthWords()) + -1.94f);
        if (isQData(nodep))
            if (useMean())
                return set(12.67);
            else
                return set(12.67f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstLt* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(52.72);
            else
                return set((6.00f * nodep->rhsp()->widthWords()) + 2.17f);
        if (isQData(nodep))
            if (useMean())
                return set(12.67);
            else
                return set(12.67f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstEq* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(34.87);
            else
                return set((4.00f * nodep->rhsp()->widthWords()) + 1.17f);
        if (isQData(nodep))
            if (useMean())
                return set(14.00);
            else
                return set(14.00f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstNeq* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(34.87);
            else
                return set((4.00f * nodep->rhsp()->widthWords()) + 1.17f);
        if (isQData(nodep))
            if (useMean())
                return set(14.00);
            else
                return set(14.00f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstGtS* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(9.96);
            else
                return set(9.96f);
        if (isQData(nodep))
            if (useMean())
                return set(18.44);
            else
                return set(18.44f);
        if (isVlWide(nodep))
            if (useMean())
                return set(94.70);
            else
                return set((9.92f * nodep->rhsp()->widthWords()) + 11.15f);
        return set(defaultLatency(nodep));
    }
    void visit(AstGteS* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(10.96);
            else
                return set(10.96f);
        if (isQData(nodep))
            if (useMean())
                return set(20.44);
            else
                return set(20.44f);
        if (isVlWide(nodep))
            if (useMean())
                return set(67.23);
            else
                return set((6.90f * nodep->rhsp()->widthWords()) + 9.06f);
        return set(defaultLatency(nodep));
    }
    void visit(AstLtS* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(9.96);
            else
                return set(9.96f);
        if (isQData(nodep))
            if (useMean())
                return set(18.44);
            else
                return set(18.44f);
        if (isVlWide(nodep))
            if (useMean())
                return set(57.96);
            else
                return set((5.90f * nodep->rhsp()->widthWords()) + 8.26f);
        return set(defaultLatency(nodep));
    }
    void visit(AstLteS* nodep) {
        if (isEData(nodep))
            if (useMean())
                return set(10.96);
            else
                return set(10.96f);
        if (isQData(nodep))
            if (useMean())
                return set(20.44);
            else
                return set(20.44f);
        if (isVlWide(nodep))
            if (useMean())
                return set(86.35);
            else
                return set((8.90f * nodep->rhsp()->widthWords()) + 11.38f);
        return set(defaultLatency(nodep));
    }
    void visit(AstNegate* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(54.98);
            else
                return set((7.00f * nodep->lhsp()->widthWords()) + -4.00f);
        return set(defaultLatency(nodep));
    }
    void visit(AstAdd* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(61.57);
            else
                return set((8.00f * nodep->lhsp()->widthWords()) + -5.83f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        if (isQData(nodep))
            if (useMean())
                return set(10.17);
            else
                return set(10.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstSub* nodep) {
        if (isVlWide(nodep))
            if (useMean())
                return set(73.00);
            else
                return set((9.00f * nodep->lhsp()->widthWords()) + -2.83f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        if (isQData(nodep))
            if (useMean())
                return set(10.17);
            else
                return set(10.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstMul* nodep) {
        if (nodep->isWide())
            return set(2.31 * nodep->widthWords() * nodep->widthWords() * nodep->widthWords()
                       + -10.80 * nodep->widthWords() * nodep->widthWords()
                       + 308.63 * nodep->widthWords() + -853.18);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        if (isQData(nodep))
            if (useMean())
                return set(27.17);
            else
                return set(27.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstMulS* nodep) {
        if (nodep->isWide())
            return set(1.98 * nodep->widthWords() * nodep->widthWords() * nodep->widthWords()
                       + -3.55 * nodep->widthWords() * nodep->widthWords()
                       + 263.54 * nodep->widthWords() + -749.20);
        if (isEData(nodep))
            if (useMean())
                return set(9.96);
            else
                return set(9.96f);
        if (isQData(nodep))
            if (useMean())
                return set(33.76);
            else
                return set(33.76f);
        return set(defaultLatency(nodep));
    }
    void visit(AstShiftL* nodep) {
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(60.79);
            else
                return set((4.36f * nodep->widthWords()) + (-0.08f * nodep->lhsp()->widthWords())
                           + (-0.08f * nodep->rhsp()->widthWords()) + 21.60f);
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(73.55);
            else
                return set((4.06f * nodep->widthWords()) + (-0.09f * nodep->lhsp()->widthWords())
                           + (-0.09f * nodep->rhsp()->widthWords()) + 40.98f);
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(63.07);
            else
                return set((4.18f * nodep->widthWords()) + (0.00f * nodep->lhsp()->widthWords())
                           + (0.00f * nodep->rhsp()->widthWords()) + 24.64f);
        if (isEData(nodep) && isEData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(20.98);
            else
                return set(20.98f);
        if (isEData(nodep) && isEData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(13.51);
            else
                return set(13.51f);
        if (isQData(nodep) && isQData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(28.55);
            else
                return set(28.55f);
        if (isQData(nodep) && isQData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(20.73);
            else
                return set(20.73f);
        if (isQData(nodep) && isQData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(11.17);
            else
                return set(11.17f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstShiftR* nodep) {
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(55.27);
            else
                return set((3.59f * nodep->widthWords()) + (-0.05f * nodep->lhsp()->widthWords())
                           + (-0.05f * nodep->rhsp()->widthWords()) + 22.76f);
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(67.08);
            else
                return set((3.63f * nodep->widthWords()) + (-0.10f * nodep->lhsp()->widthWords())
                           + (-0.10f * nodep->rhsp()->widthWords()) + 38.36f);
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(58.33);
            else
                return set((3.62f * nodep->widthWords()) + (-0.05f * nodep->lhsp()->widthWords())
                           + (-0.05f * nodep->rhsp()->widthWords()) + 26.21f);
        if (isEData(nodep) && isEData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(20.98);
            else
                return set(20.98f);
        if (isEData(nodep) && isEData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(13.51);
            else
                return set(13.51f);
        if (isQData(nodep) && isQData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(28.55);
            else
                return set(28.55f);
        if (isQData(nodep) && isQData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(20.73);
            else
                return set(20.73f);
        if (isQData(nodep) && isQData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(11.17);
            else
                return set(11.17f);
        if (isEData(nodep))
            if (useMean())
                return set(4.17);
            else
                return set(4.17f);
        return set(defaultLatency(nodep));
    }
    void visit(AstShiftRS* nodep) {
        if (isEData(nodep) && isEData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(10.05);
            else
                return set(10.05f);
        if (isQData(nodep) && isQData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(25.72);
            else
                return set(25.72f);
        if (isEData(nodep) && isQData(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(17.53);
            else
                return set(17.53f);
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isEData(nodep->rhsp()))
            if (useMean())
                return set(73.77);
            else
                return set((4.18f * nodep->widthWords()) + (-0.13f * nodep->lhsp()->widthWords())
                           + (-0.13f * nodep->rhsp()->widthWords()) + 37.20f);
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(86.01);
            else
                return set((3.79f * nodep->widthWords()) + (-0.15f * nodep->lhsp()->widthWords())
                           + (-0.15f * nodep->rhsp()->widthWords()) + 56.70f);
        if (isVlWide(nodep) && isVlWide(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(75.20);
            else
                return set((4.03f * nodep->widthWords()) + (0.00f * nodep->lhsp()->widthWords())
                           + (0.00f * nodep->rhsp()->widthWords()) + 38.17f);
        if (isQData(nodep) && isQData(nodep->lhsp()) && isVlWide(nodep->rhsp()))
            if (useMean())
                return set(39.50);
            else
                return set(39.50f);
        if (isEData(nodep) && isEData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(15.28);
            else
                return set(15.28f);
        if (isQData(nodep) && isQData(nodep->lhsp()) && isQData(nodep->rhsp()))
            if (useMean())
                return set(26.37);
            else
                return set(26.37f);
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
