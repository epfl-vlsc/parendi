import numpy as np
from sklearn.linear_model import LinearRegression
import pandas as pd
import argparse
import typing
from enum import Enum


DEFAULT_LATENCY = 14




class CostFit:
    def mkFormula(cond: str, slope, intercept, mean = 0.0):
        def mkCond(l: str, v:str):
            if l == "I":
                return f"isEData({v})"
            elif l == "Q":
                return f"isQData({v})"
            elif l == "W":
                return f"isWlWide({v})"
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
            useMean = f"if (useMean()) return set(static_cast<uint32_t>({mean:0.2f})); else "
        text = f"{condCode} {useMean} return set(static_cast<uint32_t>({text}));"
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

    def lin(self, node: str, name: str, widthInfo: str = ""):
        data = self.df.loc[self.df["Func"] == name]
        ys = data['Cycles'].tolist()
        mean = np.mean(ys)
        print(f"{name} has {len(ys)} samples with mean {mean:0.2f}")


        if widthInfo == "" or widthInfo == "___":
            code = CostFit.mkFormula(widthInfo, [], np.mean(ys))
            self.saveResult(node, code)
            return

        assert len(widthInfo) == 3, f"invalid width pattern {widthInfo}"

        xsAll = [data['OWords'].tolist(), data['LWords'].tolist(), data['RWords'].tolist()]
        xs = []
        for (c, x) in zip(widthInfo, xsAll) :
            if c != "_":
                xs.append(x)
        xs = np.transpose(xs)

        reg = LinearRegression().fit(xs, ys)
        code = CostFit.mkFormula(widthInfo, reg.coef_, reg.intercept_, mean)
        self.saveResult(node, code)

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

    fitter.lin("ExtendS", "VL_EXTENDS_II", "II_")
    fitter.lin("ExtendS", "VL_EXTENDS_QI", "QI_")
    fitter.lin("ExtendS", "VL_EXTENDS_QQ", "QQ_")
    fitter.lin("ExtendS", "VL_EXTENDS_WI", "WI_")
    fitter.lin("ExtendS", "VL_EXTENDS_WQ", "WQ_")
    fitter.lin("ExtendS", "VL_EXTENDS_WW", "WW_")

    fitter.lin("RedAnd", "VL_REDAND_II", "_I_")
    fitter.lin("RedAnd", "VL_REDAND_IQ", "_Q_")
    fitter.lin("RedAnd", "VL_REDAND_IW", "_W_")


    fitter.lin("RedOr", "VL_REDOR_I", "_I_")
    fitter.lin("RedOr", "VL_REDOR_Q", "_Q_")
    fitter.lin("RedOr", "VL_REDOR_W", "_W_")


    fitter.lin("RedXor", "VL_REDXOR_W", "_W_")
    fitter.lin("RedXor", "VL_REDXOR_32", "_I_")
    fitter.lin("RedXor", "VL_REDXOR_64", "_Q_")

    fitter.lin("CountOnes", "VL_COUNTONES_I", "_I_")
    fitter.lin("CountOnes", "VL_COUNTONES_Q", "_Q_")
    fitter.lin("CountOnes", "VL_COUNTONES_W", "_W_")

    fitter.lin("And", "VL_AND_W", "_W_")
    fitter.lin("Or", "VL_OR_W", "_W_")
    fitter.lin("Xor", "VL_XOR_W", "_W_")
    fitter.lin("Not", "VL_NOT_W", "_W_")

    fitter.lin("Gts", "VL_GTS_III", "__I")
    fitter.lin("Gts", "VL_GTS_IQQ", "__Q")
    fitter.lin("Gts", "VL_GTS_IWW", "__W")

    fitter.lin("Gtes", "VL_GTES_III", "__I")
    fitter.lin("Gtes", "VL_GTES_IQQ", "__Q")
    fitter.lin("Gtes", "VL_GTES_IWW", "__W")

    fitter.lin("Lts", "VL_LTS_III", "__I")
    fitter.lin("Lts", "VL_LTS_IQQ", "__Q")
    fitter.lin("Lts", "VL_LTS_IWW", "__W")

    fitter.lin("Ltes", "VL_LTES_III", "__I")
    fitter.lin("Ltes", "VL_LTES_IQQ", "__Q")
    fitter.lin("Ltes", "VL_LTES_IWW", "__W")

    fitter.lin("Negate", "VL_NEGATE_W", "_W_")

    fitter.lin("Add", "VL_ADD_W", "_W_")
    fitter.lin("Add", "VL_NATIVE_ADD_I", "_I_")
    fitter.lin("Add", "VL_NATIVE_ADD_Q", "_Q_")
    fitter.lin("Sub", "VL_SUB_W", "_W_")
    fitter.lin("Sub", "VL_NATIVE_SUB_I", "_I_")
    fitter.lin("Sub", "VL_NATIVE_SUB_Q", "_Q_")
    fitter.lin("Mul", "VL_MUL_W", "_W_")
    fitter.lin("Mul", "VL_NATIVE_MUL_I", "_I_")
    fitter.lin("Mul", "VL_NATIVE_MUL_Q", "_Q_")


    fitter.lin("ShiftL", "VL_SHIFTL_WWI", "WWI")
    fitter.lin("ShiftL", "VL_SHIFTL_WWW", "WWW")
    fitter.lin("ShiftL", "VL_SHIFTL_WWQ", "WWQ")
    fitter.lin("ShiftL", "VL_SHIFTL_IIW", "IIW")
    fitter.lin("ShiftL", "VL_SHIFTL_IIQ", "IIQ")
    fitter.lin("ShiftL", "VL_SHIFTL_QQW", "QQW")
    fitter.lin("ShiftL", "VL_SHIFTL_QQQ", "QQQ")
    fitter.lin("ShiftL", "VL_SHIFTL_QQI", "QQI")


    fitter.lin("ShiftR", "VL_SHIFTR_WWI", "WWI")
    fitter.lin("ShiftR", "VL_SHIFTR_WWW", "WWW")
    fitter.lin("ShiftR", "VL_SHIFTR_WWQ", "WWQ")
    fitter.lin("ShiftR", "VL_SHIFTR_IIW", "IIW")
    fitter.lin("ShiftR", "VL_SHIFTR_IIQ", "IIQ")
    fitter.lin("ShiftR", "VL_SHIFTR_QQW", "QQW")
    fitter.lin("ShiftR", "VL_SHIFTR_QQQ", "QQQ")
    fitter.lin("ShiftR", "VL_SHIFTR_QQI", "QQI")


    fitter.lin("ShiftRS", "VL_SHIFTRS_III", "III")
    fitter.lin("ShiftRS", "VL_SHIFTRS_QQI", "QQI")
    fitter.lin("ShiftRS", "VL_SHIFTRS_IQI", "IQI")
    fitter.lin("ShiftRS", "VL_SHIFTRS_WWI", "WWI")
    fitter.lin("ShiftRS", "VL_SHIFTRS_WWW", "WWW")
    fitter.lin("ShiftRS", "VL_SHIFTRS_WWQ", "WWQ")
    fitter.lin("ShiftRS", "VL_SHIFTRS_QQW", "QQW")
    fitter.lin("ShiftRS", "VL_SHIFTRS_IIQ", "IIQ")
    fitter.lin("ShiftRS", "VL_SHIFTRS_QQQ", "QQQ")


    # fitter.line("A")
    COST_MODEL_NAME = "IpuCostModelLinReg"
    with open(f"V3Bsp{COST_MODEL_NAME}.h", 'w') as fp:
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
    inline std::pair<int, bool> tryEstimate(AstNode* nodep);
    inline int estimate(AstNode* nodep) {{
        return tryEstimate(nodep).first;
    }}
}};
namespace {{
class IpuCostModelGen final : public VNVisitor {{
public:
    int m_count = 0;
    bool m_found = true;
    explicit IpuCostModelGen(AstNode* nodep) {{
        iterate(nodep);
    }}
private:
    inline int defaultLatency() {{ m_found = false; return {DEFAULT_LATENCY}; }}
    inline void set(int m) {{ m_count = m; }}
{fitter.emit()}
    void visit(AstNode* nodep) {{
        set(std::max(defaultLatency(), nodep->instrCount()));
    }}

}};
}}
std::pair<int, bool> {COST_MODEL_NAME}::tryEstimate(AstNode* nodep) {{
    IpuCostModelLinReg c{{nodep}};
    return {{c.m_count, c.m_found}};
}}
#endif
""")
