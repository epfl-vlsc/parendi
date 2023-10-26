
#!/usr/bin/env python3
import numpy as np
import argparse
from pathlib import Path
import subprocess
import multiprocessing
import typing
import pandas as pd

REPEATS = 64
SUPERVISOR = True


class VlFunc:
    def __init__(self, name: str, func: str = None):
        self.name = name
        if func == None:
            self.func = f"{name}(obits, rbits, lbits, op, lp, rp)"
        else:
            self.func = func
    def toString(self):
        return self.func


class VertexGenerator:

    def __init__(self, vlOp: VlFunc,
                obits: int = 32,
                lbits: int = 32, rbits: int = 32,
                supervisor: bool = True, repeats: int = 32):
        assert((repeats - 1) & repeats) == 0, "repeats should be power of 2"
        self.vlOp = vlOp
        self.supervisor = supervisor
        self.repeats = repeats
        self.obits = obits
        self.lbits = lbits
        self.rbits = rbits
        def getType(nbits: int):
                if nbits <= 32:
                    return "IData"
                elif nbits <= 64:
                    return "IData"
                else :
                    return f"VlWide<VL_WORDS_I({nbits})>"
        self.otype = getType(obits)
        self.rtype = getType(rbits)
        self.ltype = getType(lbits)
        self.path = None

    def name(self):
        return "VTX_" + self.vlOp.name

    def emit(self):

        vertexType = "SupervisorVertex" if self.supervisor else "Vertex"
        vtxAttr = '__attribute__((target("supervisor"))) ' if self.supervisor else ""

        text = f"""
#include "vlpoplar/verilated.h"
#include <ipu_intrinsics>
#include <poplar/Vertex.hpp>
using namespace poplar;

struct {self.name()} : public {vertexType} {{
    VecIn in1, in2; Vec out, cycles;
    inline uint64_t timeNow() const {{
        uint32_t l = -1, u = -1, l2 = -1;
        do {{
            l = __builtin_ipu_get_scount_l();
            u = __builtin_ipu_get_scount_u();
            l2 = __builtin_ipu_get_scount_l();
        }} while (l2 < l);
        const uint64_t ts = (static_cast<uint64_t>(u) << 32ull) | static_cast<uint64_t>(l2);
        return ts;
    }}
    {vtxAttr} void compute() {{
        const uint64_t start = timeNow();
        constexpr int obits = {self.obits};
        constexpr int lbits = {self.lbits};
        constexpr int rbits = {self.rbits};
        constexpr int words = VL_WORDS_I(obits);
        #pragma clang loop unroll(full)
        for (int i = 0; i < {self.repeats}; i++) {{
            const {self.ltype}& lp = reinterpret_cast<const {self.ltype}*>(in1.data())[i];
            const {self.rtype}& rp = reinterpret_cast<const {self.rtype}*>(in2.data())[i];
            {self.otype}& op = reinterpret_cast<{self.otype}*>(out.data())[i];
            {self.vlOp.func};
        }}
        const uint64_t end = timeNow();
        const uint64_t d = (end - start) / {self.repeats};
        cycles[0] = d & 0xfffffffful;
        cycles[1] = (d >> 32ul) & 0xfffffffful;
    }}

}};
"""
        return text


    def build(self) -> Path:

        cppFile = Path("funcs") / f"{self.vlOp.name}.cpp"
        if not cppFile.exists():
            with open(cppFile, 'w') as fp:
                    print(f"Generating code {self.name()}")
                    text = self.emit()
                    fp.write(text)
        mkCmd = ["make", f"funcs/{self.vlOp.name}.gp"]
        print(f"Compiling {self.name()}")
        proc = subprocess.run(mkCmd, check=False, capture_output=True, text=True)
        if (proc.returncode != 0):
            print(proc.stderr)
            raise RuntimeError(f"Compiliation failed for {self.name()}")
        self.path = Path("funcs") / f"{self.vlOp.name}.gp"

        return self.path


    def copy(self, obits: int, lbits: int, rbts: int):
        return VertexGenerator(vlOp=self.vlOp,
                               obits = obits, lbits = self.lbits, rbits = self.rbits,
                               supervisor = self.supervisor, repeats=self.repeats)

if __name__ == "__main__":


    pool = multiprocessing.Pool(1)
    randGen = np.random.default_rng(78123961)

    argParser = argparse.ArgumentParser("Profile operations on the IPU")
    argParser.add_argument("--funcs", "-f", nargs="+", default=[], help="functions to profile")

    args = argParser.parse_args()

    def shouldProfile(s: str):
        if (len(args.funcs) == 0) or (s in args.funcs):
            return True
        else:
            return False

    df = pd.DataFrame(
        {
            "Func" : pd.Series(dtype=str),
            "OBits": pd.Series(dtype=int),
            "LBits": pd.Series(dtype=int),
            "RBits": pd.Series(dtype=int),
            "Cycles": pd.Series(dtype=int),
        }
    )

    print("Generating random cases")

    def gW(): return randGen.integers(65, 513)
    def gQ(): return randGen.integers(33, 65)
    def gI(): return randGen.integers(1, 33)
    def gOne(): return 1
    def gList(c1, c2, c3, l = 10): return list(set([(c1(), c2(), c3()) for _ in range(l)]))

    WidthWWW = gList(gW, gW, gW)
    WidthW = list(set([(x, x, x) for (x, _, _) in WidthWWW]))

    WidthWQQ = gList(gW, gQ, gQ)
    WidthWQI = gList(gW, gQ, gI)
    WidthWWI = gList(gW, gW, gI)
    WidthWWQ = gList(gW, gW, gQ)

    WidthQQQ = gList(gQ, gQ, gQ)
    WidthQQW = gList(gQ, gQ, gW)
    WidthQII = gList(gQ, gI, gI)
    WidthQQI = gList(gQ, gQ, gI)

    WidthIII = gList(gI, gI, gI)
    WidthIQI = gList(gI, gQ, gI)
    WidthIWI = gList(gI, gW, gI)
    WidthWII = gList(gW, gI, gI)
    WidthWQQ = gList(gW, gQ, gQ)
    WidthIIW = gList(gI, gI, gW)
    WidthIIQ = gList(gI, gI, gQ)
    WidthIQQ = gList(gI, gQ, gQ)
    WidthIWW = gList(gI, gW, gW)

    Width1I = gList(gOne, gI, gOne)
    Width1Q = gList(gOne, gQ, gOne)
    Width1W = gList(gOne, gW, gOne)


    print("Done")
    def saveData(name: str, obits: int, lbits: int, rbits: int, cycles: int):
        df.loc[len(df)] = [name, obits, lbits, rbits, cycles]
        df.to_string("data.txt")

    def runCases(fn, widthCases, funcName: str):

        # ls = pool.starmap(fn, widthCases)
        arity = len(widthCases[0])
        ls = []
        if arity == 3:
            ls = [fn(x, y, z) for (x, y, z) in widthCases]
        elif arity == 2:
            ls = [fn(x, y) for (x, y) in widthCases]
        else:
            ls = [fn(x, y, z, w) for (x, y, z, w) in widthCases]
        print(f"Profiling {funcName}")
        cmd = ["./runner", "-r", str(REPEATS), "-v"] + \
                       [g.name() for g in ls] + \
                        ["-f"] +  [str(g.path) for g in ls]
        print(" ".join(cmd))
        proc = subprocess.run(cmd,
                        text=True, capture_output=True, check=False)
        if (proc.returncode != 0):
            print(proc.stderr)
            exit(-1)
        lines = proc.stdout.strip().split('\n')
        print('\n'.join(lines))
        res = [int(ln.strip().split(":")[1]) for ln in lines]

        for (cycles, gen) in zip(res, ls):
            saveData(funcName, gen.obits, gen.lbits, gen.lbits, cycles)

        # numWords = [int(gen.obits / 32) for gen in ls]
        # (m, c) = np.polyfit(numWords, res, 1)
        # print(f"y = {m:0.3f} * n + {c:0.3f}")
        return res


    ####### VL FUNCTIONS

    # EXTENDS

    def make_EXTENDS(suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_EXTENDS_{suffix}(obits, lbits, lp)"
            if suffix.startswith("W"):
                funcImpl = f"VL_EXTENDS_{suffix}(obits, lbits, op, lp)"
            gen = VertexGenerator(
                VlFunc(
                    name= f"VL_EXTENDS_{suffix}_{obits}_{lbits}",
                    func = funcImpl
                ),
                obits=obits, lbits=lbits, rbits=rbits,
                    supervisor=SUPERVISOR, repeats=REPEATS
            )
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_EXTENDS_{suffix}")

    if shouldProfile("EXTENDS"):
        make_EXTENDS("II", WidthIII)
        make_EXTENDS("QI", WidthQII)
        make_EXTENDS("QQ", WidthQQQ)
        make_EXTENDS("WI", WidthWII)
        make_EXTENDS("WQ", WidthWQQ)
        make_EXTENDS("WW", WidthWWW)


    # REDOR, REDAND, REDXOR
    def make_REDUCE(op: str, suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_RED{op}_{suffix}(lbits, lp)"
            gen = VertexGenerator(
                VlFunc(
                    name= f"VL_RED{op}_{suffix}_{obits}_{lbits}",
                    func = funcImpl
                ),
                obits=obits, lbits=lbits, rbits=rbits,
                    supervisor=SUPERVISOR, repeats=REPEATS
            )
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_RED{op}_{suffix}")

    if shouldProfile("REDAND"):
        make_REDUCE("AND", "II", Width1I)
        make_REDUCE("AND", "IQ", Width1Q)
        make_REDUCE("AND", "IW", Width1W)

    if shouldProfile("REDOR"):
        make_REDUCE("OR", "II", Width1I)
        make_REDUCE("OR", "IQ", Width1Q)
        make_REDUCE("OR", "IW", Width1W)

    if shouldProfile("REDXOR"):
        make_REDUCE("XOR", "W", Width1W)
        make_REDUCE("XOR", "2", Width1I)
        make_REDUCE("XOR", "4", Width1I)
        make_REDUCE("XOR", "8", Width1I)
        make_REDUCE("XOR", "16", Width1I)
        make_REDUCE("XOR", "32", Width1I)
        make_REDUCE("XOR", "64", Width1Q)




    # COUNTONES
    def make_COUNTONES(suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_COUNTONES_{suffix}(lbits, lp)"
            gen = VertexGenerator(
                VlFunc(
                    name= f"VL_COUNTONES_{suffix}_{obits}_{lbits}",
                    func = funcImpl
                ),
                obits=obits, lbits=lbits, rbits=rbits,
                    supervisor=SUPERVISOR, repeats=REPEATS
            )
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_COUNTONES_{suffix}")

    if shouldProfile("COUNTONES"):
        make_COUNTONES("I", WidthIII)
        make_COUNTONES("Q", WidthIQI)
        make_COUNTONES("W", WidthIWI)

    # TODO COUNTBITS

    # TODO ONEHOT, ONEHOT0

    # TODO CLOG2


    # AND, OR, XOR
    def make_BITWISE(op: str, suffix: str, cases):

        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"VL_{op}_{suffix}(words, op, lp, rp)"
            if op == "NOT":
                funcImpl = f"VL_{op}_{suffix}(words, op, lp)"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_{op}_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR, repeats=REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_{op}_{suffix}")

    if shouldProfile("AND"):
        make_BITWISE("AND", "W", WidthW)
    if shouldProfile("OR"):
        make_BITWISE("OR", "W", WidthW)
    if shouldProfile("XOR"):
        make_BITWISE("XOR", "W", WidthW)
        make_BITWISE("CAHGENXOR", "W", WidthW)
    if shouldProfile("NOT"):
        make_BITWISE("NOT", "W", WidthW)


    # GTS, GTES, LTS, LTES
    def make_COMPARE(op: str, suffix: str, cases):

        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_{op}_{suffix}(lbits, lp, rp)"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_{op}_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR, repeats=REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_{op}_{suffix}")

    if shouldProfile("GTS"):
        make_COMPARE("GTS", "III", WidthIII)
        make_COMPARE("GTS", "IQQ", WidthIQQ)

    if shouldProfile("GTES"):
        make_COMPARE("GTES", "III", WidthIII)
        make_COMPARE("GTES", "IQQ", WidthIQQ)

    if shouldProfile("LTS"):
        make_COMPARE("LTS", "III", WidthIII)
        make_COMPARE("LTS", "IQQ", WidthIQQ)

    if shouldProfile("LTES"):
        make_COMPARE("LTES", "III", WidthIII)
        make_COMPARE("LTES", "IQQ", WidthIQQ)

    if shouldProfile("LTS"):
        make_COMPARE("LTS", "IWW", WidthIWW)
        make_COMPARE("LTES", "IWW", WidthIWW)

    if shouldProfile("GTES"):
        make_COMPARE("GTS", "IWW", WidthIWW)
        make_COMPARE("GTES", "IWW", WidthIWW)

    # NEGATE
    def make_NEGATE(suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"VL_NEGATE_{suffix}(words, op, lp)"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_NEGATE_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR, repeats=REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_NEGATE_{suffix}")
    if shouldProfile("NEGATE"):
        make_NEGATE("W", WidthW)


    # ADD and SUB, MUL_W
    def make_ARITH(op: str, suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"VL_{op}_{suffix}(words, op, lp, rp)"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_{op}_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR, repeats=REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_{op}_{suffix}")

    if shouldProfile("ADD"):
        make_ARITH("ADD", "W", WidthW)
    if shouldProfile("SUB"):
        make_ARITH("SUB", "W", WidthW)
    if shouldProfile("MUL"):
        make_ARITH("MUL", "W", WidthW)


    def make_MUL(suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_MUL_{suffix}(lbits, lp, rp)"
            if suffix.startswith("W"):
                funcImpl = f"VL_MUL_{suffix}(lbits, op, lp, rp)"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_MUL_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR, repeats=REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_MUL_{suffix}")
    if shouldProfile("MUL"):
        make_MUL("MUL", "III", WidthIII)
        make_MUL("MUL", "QQQ", WidthQQQ)
        make_MUL("MUL", "WWW", WidthWWW)



    # SHIFTL, SHIFTR, and SHIFTRS

    def make_BINOP(op: str, suffix: str, cases):

        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_{op}_{suffix}(obits, lbits, rbits, lp, rp)"
            if suffix.startswith("W"):
                funcImpl = f"VL_{op}_{suffix}(obits, lbits, rbits, op, lp, rp)"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_{op}_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR, repeats=REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_{op}_{suffix}")

    if shouldProfile("SHIFTL"):
        make_BINOP("SHIFTL", "WWI", WidthWWI)
        make_BINOP("SHIFTL", "WWW", WidthWWW)
        make_BINOP("SHIFTL", "WWQ", WidthWWQ)
        make_BINOP("SHIFTL", "IIW", WidthIIW)
        make_BINOP("SHIFTL", "IIQ", WidthIIQ)
        make_BINOP("SHIFTL", "QQW", WidthQQW)
        make_BINOP("SHIFTL", "QQQ", WidthQQQ)
        make_BINOP("SHIFTL", "QQI", WidthQQI)

    if shouldProfile("SHIFTR"):
        make_BINOP("SHIFTR", "WWI", WidthWWI)
        make_BINOP("SHIFTR", "WWW", WidthWWW)
        make_BINOP("SHIFTR", "WWQ", WidthWWQ)
        make_BINOP("SHIFTR", "IIW", WidthIIW)
        make_BINOP("SHIFTR", "IIQ", WidthIIQ)
        make_BINOP("SHIFTR", "QQW", WidthQQW)
        make_BINOP("SHIFTR", "QQQ", WidthQQQ)
        make_BINOP("SHIFTR", "QQI", WidthQQI)

    if shouldProfile("SHIFTRS"):
        make_BINOP("SHIFTRS", "III", WidthIII)
        make_BINOP("SHIFTRS", "QQI", WidthQQI)
        make_BINOP("SHIFTRS", "IQI", WidthIQI)
        make_BINOP("SHIFTRS", "WWI", WidthWWI)
        make_BINOP("SHIFTRS", "WWW", WidthWWW)
        make_BINOP("SHIFTRS", "WWQ", WidthWWQ)
        make_BINOP("SHIFTRS", "QQW", WidthQQW)
        make_BINOP("SHIFTRS", "IIQ", WidthIIQ)
        make_BINOP("SHIFTRS", "QQQ", WidthQQQ)










    # shiftlCasesWide = pool.starmap(mkShiftLWWW, wideBitSizes)
    # shiftrCasesWide = pool.starmap(mkShiftRWWW, wideBitSizes)
    # shiftrsCasesWide = pool.starmap(mkShiftRSWWW, wideBitSizes)

    # runCases(shiftlCasesWide, "VL_SHIFTL_WWW")
    # runCases(shiftrCasesWide, "VL_SHIFTR_WWW")
    # runCases(shiftrsCasesWide, "VL_SHIFTRS_WWW")








