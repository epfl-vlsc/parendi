import numpy as np
from sklearn.linear_model import LinearRegression
import pandas as pd
import argparse
import typing
from enum import Enum


DEFAULT_LATENCY = 1




class CostFit:
    def mkFormula(cond: str, slope, intercept, mean = 0.0):
        def mkCond(l: str, v:str):
            if l == "I":
                return f"isEData({v})"
            elif l == "Q":
                return f"isQData({v})"
            elif l == "W":
                return f"isVlWide({v})"
            else:
                raise NotImplementedError
        condBits = list(filter(lambda x: x != "_", [x for x in cond]))
        assert len(slope) == len(condBits), "Invalid number of variables"
        vars = ["nodep", "nodep->lhsp()", "nodep->rhsp()"]
        vars = [v for _, v in filter(lambda xy : xy[0] != "_", zip([x for x in cond], vars))]
        condCode = zip(condBits, ["nodep", "nodep->lhsp()", "nodep->rhsp()"])
        condCode = [f"{mkCond(c, v)}" for c, v in condCode]
        condCode = " && ".join(condCode)
        condCode = f"if ({condCode})"

        formula = filter(lambda sv: sv[0] != 0, zip(slope, vars))
        formula = [f"({c:0.2f}f * {n}->widthWords())" for c, n in formula]
        formula = " + ".join(formula)
        text = formula
        if len(formula):
            text = formula + f" + {intercept:0.2f}f"
        else :
            text = f"{intercept:0.2f}f"
        useMean = ""
        if (mean != 0.0):
            useMean = f"if (useMean()) return set({mean:0.2f}); else "
        text = f"{condCode} {useMean} return set({text});"
        return text

    def __init__(self, df: pd.DataFrame):
        self.df = df
        self.df['OWords'] = [int(np.ceil(x / 32)) for x in self.df['OBits']]
        self.df['LWords'] = [int(np.ceil(x / 32)) for x in self.df['LBits']]
        self.df['RWords'] = [int(np.ceil(x / 32)) for x in self.df['RBits']]
        self.codeGen = {}
        self.anyCode = {}
        # print(self.df)

    def saveResult(self, node: str, text: str):
        if not (node in self.codeGen):
            self.codeGen[node] = []
        self.codeGen[node].append(text)

    def lin(self, node: str, name: str, numOperands: int, widthInfo: str):
        data = self.df.loc[self.df["Func"] == name]
        ys = data['Cycles'].tolist()
        ys = [y if s else y / 6 for y, s in  zip(ys, data['Supervisor'])]
        xsAll = [data['OWords'].tolist(), data['LWords'].tolist(), data['RWords'].tolist()]
        mean = np.mean(ys)
        print(f"{name} has {len(ys)} samples with mean {mean:0.2f}")


        if widthInfo == "" or widthInfo == "___":
            code = CostFit.mkFormula(widthInfo, [], np.mean(ys))
            self.saveResult(node, code)
            return

        assert len(widthInfo) == 3, f"invalid width pattern {widthInfo}"

        xs = []
        for (c, x) in zip(widthInfo, xsAll) :
            if c != "_":
                xs.append(x)
        xs = np.transpose(xs)

        reg = LinearRegression().fit(xs, ys)
        code = CostFit.mkFormula(widthInfo, reg.coef_, reg.intercept_, mean)
        self.saveResult(node, code)

    def cubeWide(self, node: str, name: str):
        data = self.df.loc[self.df["Func"] == name]
        ys = data['Cycles'].tolist()
        ys = [y if s else y / 6 for y, s in  zip(ys, data['Supervisor'])]
        xs = data['OWords'].tolist()
        c3, c2, c1, c0 = np.polyfit(xs, ys, deg=3)
        text = f"if(nodep->isWide()) return set({c3:0.2f} * nodep->widthWords() * nodep->widthWords() * nodep->widthWords() + {c2:0.2f} * nodep->widthWords() * nodep->widthWords() + {c1: 0.2f} * nodep->widthWords() + {c0:0.2f});"
        self.saveResult(node, text)

    def fallback(self, node: str, cost: int):
        if not node in self.anyCode:
            self.anyCode[node] = f"return set({cost});"
    def emit(self):

        text = ""
        for k, vs in self.codeGen.items():
            text += f"void visit(Ast{k}* nodep){{\n"
            for v in vs:
                text += f"\t{v}\n"
            if k in self.anyCode:
                text += f"\t{self.anyCode[k]}\n"
            else:
                text += f"\treturn set(defaultLatency(nodep));\n"
            text +=f"}}\n"

        return text



if __name__ == "__main__":

    dfFileName = "profile.txt"



    fitter = CostFit(pd.read_table(dfFileName, delim_whitespace=True))

    fitter.lin("ExtendS", "VL_EXTENDS_II", 1, "II_")
    fitter.lin("ExtendS", "VL_EXTENDS_QI", 1, "QI_")
    fitter.lin("ExtendS", "VL_EXTENDS_QQ", 1, "QQ_")
    fitter.lin("ExtendS", "VL_EXTENDS_WI", 1, "WI_")
    fitter.lin("ExtendS", "VL_EXTENDS_WQ", 1, "WQ_")
    fitter.lin("ExtendS", "VL_EXTENDS_WW", 1, "WW_")

    fitter.lin("RedAnd", "VL_REDAND_II", 1, "_I_")
    fitter.lin("RedAnd", "VL_REDAND_IQ", 1, "_Q_")
    fitter.lin("RedAnd", "VL_REDAND_IW", 1, "_W_")


    fitter.lin("RedOr", "VL_REDOR_I", 1, "_I_")
    fitter.lin("RedOr", "VL_REDOR_Q", 1, "_Q_")
    fitter.lin("RedOr", "VL_REDOR_W", 1, "_W_")


    fitter.lin("RedXor", "VL_REDXOR_W", 1, "_W_")
    fitter.lin("RedXor", "VL_REDXOR_32", 1, "_I_")
    fitter.lin("RedXor", "VL_REDXOR_64", 1, "_Q_")

    fitter.lin("CountOnes", "VL_COUNTONES_I", 1, "_I_")
    fitter.lin("CountOnes", "VL_COUNTONES_Q", 1, "_Q_")
    fitter.lin("CountOnes", "VL_COUNTONES_W", 1, "_W_")

    fitter.lin("And", "VL_AND_W", 2, "_W_")
    fitter.lin("And", "VL_NATIVE_AND_I", 2, "_I_")
    fitter.lin("And", "VL_NATIVE_AND_Q", 2, "_Q_")
    fitter.lin("Or", "VL_OR_W", 2, "_W_")
    fitter.lin("Or", "VL_NATIVE_OR_I", 2, "_I_")
    fitter.lin("Or", "VL_NATIVE_OR_Q", 2, "_Q_")
    fitter.lin("Xor", "VL_XOR_W", 2, "_W_")
    fitter.lin("Xor", "VL_NATIVE_XOR_I", 2, "_I_")
    fitter.lin("Xor", "VL_NATIVE_XOR_Q", 2, "_Q_")
    fitter.lin("Not", "VL_NOT_W", 1, "_W_")
    fitter.lin("Not", "VL_NATIVE_NOT_I", 1, "_I_")
    fitter.lin("Not", "VL_NATIVE_NOT_Q", 1, "_Q_")


    fitter.lin("Gt", "VL_GT_W", 2, "__W")
    fitter.lin("Gt", "VL_NATIVE_GT_Q", 2, "__Q")
    fitter.lin("Gt", "VL_NATIVE_GT_I", 2, "__I")

    fitter.lin("Lt", "VL_LT_W", 2, "__W")
    fitter.lin("Lt", "VL_NATIVE_LT_Q", 2, "__Q")
    fitter.lin("Lt", "VL_NATIVE_LT_I", 2, "__I")

    fitter.lin("Eq", "VL_EQ_W", 2, "__W")
    fitter.lin("Eq", "VL_NATIVE_EQ_Q", 2, "__Q")
    fitter.lin("Eq", "VL_NATIVE_EQ_I", 2, "__I")

    fitter.lin("Neq", "VL_NEQ_W", 2, "__W")
    fitter.lin("Neq", "VL_NATIVE_NEQ_Q", 2, "__Q")
    fitter.lin("Neq", "VL_NATIVE_NEQ_I", 2, "__I")


    fitter.lin("GtS", "VL_GTS_III", 2, "__I")
    fitter.lin("GtS", "VL_GTS_IQQ", 2, "__Q")
    fitter.lin("GtS", "VL_GTS_IWW", 2, "__W")


    fitter.lin("GteS", "VL_GTES_III", 2, "__I")
    fitter.lin("GteS", "VL_GTES_IQQ", 2, "__Q")
    fitter.lin("GteS", "VL_GTES_IWW", 2, "__W")

    fitter.lin("LtS", "VL_LTS_III", 2, "__I")
    fitter.lin("LtS", "VL_LTS_IQQ", 2, "__Q")
    fitter.lin("LtS", "VL_LTS_IWW", 2, "__W")

    fitter.lin("LteS", "VL_LTES_III", 2, "__I")
    fitter.lin("LteS", "VL_LTES_IQQ", 2, "__Q")
    fitter.lin("LteS", "VL_LTES_IWW", 2, "__W")

    fitter.lin("Negate", "VL_NEGATE_W", 1, "_W_")

    fitter.lin("Add", "VL_ADD_W", 2, "_W_")
    fitter.lin("Add", "VL_NATIVE_ADD_I", 2, "_I_")
    fitter.lin("Add", "VL_NATIVE_ADD_Q", 2, "_Q_")
    fitter.lin("Sub", "VL_SUB_W", 2, "_W_")
    fitter.lin("Sub", "VL_NATIVE_SUB_I", 2, "_I_")
    fitter.lin("Sub", "VL_NATIVE_SUB_Q", 2, "_Q_")
    # fitter.lin("Mul", "VL_MUL_W", 2, "_W_")
    fitter.cubeWide("Mul", "VL_MUL_W")
    fitter.lin("Mul", "VL_NATIVE_MUL_I", 2, "_I_")
    fitter.lin("Mul", "VL_NATIVE_MUL_Q", 2, "_Q_")

    fitter.cubeWide("MulS", "VL_MULS_WWW")
    fitter.lin("MulS", "VL_MULS_III", 2, "_I_")
    fitter.lin("MulS", "VL_MULS_QQQ", 2, "_Q_")


    fitter.lin("ShiftL", "VL_SHIFTL_WWI", 2, "WWI")
    fitter.lin("ShiftL", "VL_SHIFTL_WWW", 2, "WWW")
    fitter.lin("ShiftL", "VL_SHIFTL_WWQ", 2, "WWQ")
    fitter.lin("ShiftL", "VL_SHIFTL_IIW", 2, "IIW")
    fitter.lin("ShiftL", "VL_SHIFTL_IIQ", 2, "IIQ")
    fitter.lin("ShiftL", "VL_SHIFTL_QQW", 2, "QQW")
    fitter.lin("ShiftL", "VL_SHIFTL_QQQ", 2, "QQQ")
    fitter.lin("ShiftL", "VL_SHIFTL_QQI", 2, "QQI")
    fitter.lin("ShiftL", "VL_NATIVE_SHIFTL_I", 2, "_I_")

    fitter.lin("ShiftR", "VL_SHIFTR_WWI", 2, "WWI")
    fitter.lin("ShiftR", "VL_SHIFTR_WWW", 2, "WWW")
    fitter.lin("ShiftR", "VL_SHIFTR_WWQ", 2, "WWQ")
    fitter.lin("ShiftR", "VL_SHIFTR_IIW", 2, "IIW")
    fitter.lin("ShiftR", "VL_SHIFTR_IIQ", 2, "IIQ")
    fitter.lin("ShiftR", "VL_SHIFTR_QQW", 2, "QQW")
    fitter.lin("ShiftR", "VL_SHIFTR_QQQ", 2, "QQQ")
    fitter.lin("ShiftR", "VL_SHIFTR_QQI", 2, "QQI")
    fitter.lin("ShiftR", "VL_NATIVE_SHIFTR_I", 2, "_I_")


    fitter.lin("ShiftRS", "VL_SHIFTRS_III", 2, "III")
    fitter.lin("ShiftRS", "VL_SHIFTRS_QQI", 2, "QQI")
    fitter.lin("ShiftRS", "VL_SHIFTRS_IQI", 2, "IQI")
    fitter.lin("ShiftRS", "VL_SHIFTRS_WWI", 2, "WWI")
    fitter.lin("ShiftRS", "VL_SHIFTRS_WWW", 2, "WWW")
    fitter.lin("ShiftRS", "VL_SHIFTRS_WWQ", 2, "WWQ")
    fitter.lin("ShiftRS", "VL_SHIFTRS_QQW", 2, "QQW")
    fitter.lin("ShiftRS", "VL_SHIFTRS_IIQ", 2, "IIQ")
    fitter.lin("ShiftRS", "VL_SHIFTRS_QQQ", 2, "QQQ")



    # fitter.line("A")
    COST_MODEL_NAME = "IpuCostModelLinReg"
    with open(f"../src/V3Bsp{COST_MODEL_NAME}.h", 'w') as fp:
        fp.write(f"""
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


class {COST_MODEL_NAME} final {{
public:
    static inline std::pair<int, bool> tryEstimate(AstNode* nodep, bool useMean = false);
    static inline int estimate(AstNode* nodep, bool useMean = false) {{
        return tryEstimate(nodep, useMean).first;
    }}
}};
namespace {{
class IpuCostModelGen final : public VNVisitor {{
public:
    int m_count = 0;
    bool m_found = true;
    const bool m_useMean;
    explicit IpuCostModelGen(AstNode* nodep, bool useMean = false) : m_useMean{{useMean}} {{
        iterate(nodep);
    }}
private:
    inline bool isQData(AstNode* nodep) const {{ return nodep->isQuad(); }}
    inline bool isVlWide(AstNode* nodep) const {{ return nodep->isWide(); }}
    inline bool isEData(AstNode* nodep) const {{ return nodep->widthWords() == 1; }}
    inline bool useMean() const {{ return m_useMean; }}
    inline int defaultLatency(AstNode* nodep) {{
        m_found = false;
        return nodep->widthWords();
    }}
    void visit(AstCCast*) override {{ set(0); }}
    void visit(AstVarRefView*) override {{ set(0); }}
    void visit(AstNodeVarRef* nodep) override {{
        if (const AstCMethodHard* const callp = VN_CAST(nodep->backp(), CMethodHard)) {{
            if (callp->fromp() == nodep) return set(1);
        }}
        if (nodep->varp()->isFuncLocal()) {{
            return set(nodep->widthWords());
        }} else {{
            return set(nodep->widthWords() + 1);
        }}
    }}

    inline void set(float m) {{ m_count = static_cast<int>(std::round(m)); }}
    void visit(AstNodeIf* nodep) override {{ set(6); }}
    void visit(AstNodeCond* nodep) override {{
        if (AstNodeAssign* const assignp = VN_CAST(nodep->backp(), NodeAssign)) {{
            AstNodeVarRef* const lvp = VN_CAST(assignp->lhsp(), NodeVarRef);
            AstNodeVarRef* const elsep = VN_CAST(nodep->elsep(), NodeVarRef);
            if (lvp && elsep && lvp->varp() == elsep->varp()) {{
                set(3);  // can become movz
                return;
            }}
        }}
        set(6);
    }}
{fitter.emit()}
    void visit(AstNode* nodep) {{
        set(std::max(defaultLatency(nodep), nodep->instrCount()));
    }}

}};
}}
std::pair<int, bool> {COST_MODEL_NAME}::tryEstimate(AstNode* nodep, bool useMean) {{
    IpuCostModelGen c{{nodep, useMean}};
    return {{c.m_count, c.m_found}};
}}
#endif
""")
