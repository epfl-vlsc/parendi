
#!/usr/bin/env python3
import numpy as np
import argparse
from pathlib import Path
import subprocess
import multiprocessing
import typing
import pandas as pd

REPEATS = 64
MUL_W_REPEATS = 16
SUPERVISOR = False
BUILD_TIMEOUT = 30 # 30 second

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
                    return "QData"
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
        constexpr int obits = {self.obits};
        constexpr int lbits = {self.lbits};
        constexpr int rbits = {self.rbits};
        constexpr int words = VL_WORDS_I(obits);
        const uint64_t start = timeNow();
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
        mkCmd = ["make", f"funcs/{self.vlOp.name}.gp", f"funcs/{self.vlOp.name}.s"]
        print(f"Compiling {self.name()}")
        self.path = None
        try:
            proc = subprocess.run(mkCmd, check=False, capture_output=True, text=True, timeout=BUILD_TIMEOUT)
            if (proc.returncode != 0):
                print(proc.stderr)
                raise RuntimeError(f"Compiliation failed for {self.name()}")
            else:
                print(f"Finished compiling {self.name()}")
            proc = subprocess.run(mkCmd, check=False, capture_output=True, text=True, timeout=BUILD_TIMEOUT)
            self.path = Path("funcs") / f"{self.vlOp.name}.gp"
        except subprocess.TimeoutExpired as e:
                print(str(e))
                print(f"Build time out for {self.name()}!")

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
    argParser.add_argument("--jobs", "-j", type=int, default=1, help="number of jobs")
    argParser.add_argument("--output", "-o", type=Path, default=Path("profile.txt"), help="output file")
    argParser.add_argument("--num", "-n", type=int, default=50, help="Number of random cases")
    argParser.add_argument("--append", "-a", action="store_true", default=False, help="Append to the output")
    args = argParser.parse_args()

    Jobs = args.jobs
    Outfile = args.output
    NumRand = args.num

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
            "Supervisor": pd.Series(dtype=bool)
        }
    )
    if args.append:
        df = pd.read_table(Outfile, delim_whitespace=True)

    print("Generating random cases")

    def gW(): return randGen.integers(65, 513)
    def gQ(): return randGen.integers(33, 65)
    def gI(): return randGen.integers(1, 33)
    def gOne(): return 1
    def gList(c1, c2 = gOne, c3 = gOne): return list(set([(c1(), c2(), c3()) for _ in range(NumRand)]))

    WidthWWW = gList(gW, gW, gW)
    WidthIW2 = list(set([(32, x, x)for (x,_,_) in WidthWWW]))
    WidthW3 = list(set([(x, x, x) for (x, _, _) in WidthWWW]))

    WidthWQQ = gList(gW, gQ, gQ)
    WidthWQI = gList(gW, gQ, gI)
    WidthWWI = gList(gW, gW, gI)
    WidthWWQ = gList(gW, gW, gQ)

    WidthQQQ = gList(gQ, gQ, gQ)
    WidthIQ2 = list(set([(32, x, x)for (x,_,_) in WidthQQQ]))
    WidthQ3 = list(set([(x, x, x) for (x, _, _) in WidthQQQ]))
    WidthQQW = gList(gQ, gQ, gW)
    WidthQII = gList(gQ, gI, gI)
    WidthQQI = gList(gQ, gQ, gI)

    WidthIII = gList(gI, gI, gI)
    WidthII2 = list(set([(32, x, x)for (x,_,_) in WidthIII]))
    WidthI3 = list(set([(x, x, x) for (x, _, _) in WidthIII]))
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

    WidthII = gList(gI, gI)
    WidthQI = gList(gQ, gI)
    WidthQQ = gList(gQ, gQ)
    WidthWI = gList(gW, gI)
    WidthWQ = gList(gW, gQ)
    WidthWW = gList(gW, gW)
    WidthIQ = gList(gI, gQ)
    WidthIW = gList(gI, gW)
    WidthI = gList(gI)
    WidthQ = gList(gQ)
    WidthW = gList(gW)


    print("Done")
    def saveData(name: str, obits: int, lbits: int, rbits: int, cycles: int):
        df.loc[len(df)] = [name, obits, lbits, rbits, cycles, SUPERVISOR]
        df.to_string(Outfile)

    def runCases(fn, widthCases, funcName: str):


        def fnWrapped(q, args):
            res = None
            if len(args) == 1:
                res = fn(args[0])
            elif len(args) == 2:
                res = fn(args[0], args[1])
            elif len(args) == 3:
                res = fn(args[0], args[1], args[2])
            else:
                res = fn(args[0], args[1], args[2], args[3])
            q.put(res)
        ls = []
        j = 0
        while j < len(widthCases):
            lastJ = min(j + Jobs, len(widthCases))
            jobCases = widthCases[j : lastJ]
            j = j + Jobs
            if len(jobCases) == 0:
                break
            ctx = multiprocessing.get_context("spawn")
            queues = [ctx.Queue() for _ in jobCases]
            procs = [multiprocessing.Process(target=fnWrapped, args=(queues[i], jobCases[i])) for i in range(len(jobCases))]
            for p in procs:
                p.start()
            pls = [q.get() for q in queues]
            ls = ls + list(filter(lambda g: g.path != None, pls))
            # for p in procs:
            #     p.join()


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
        make_EXTENDS("II", WidthII)
        make_EXTENDS("QI", WidthQI)
        make_EXTENDS("QQ", WidthQQ)
        make_EXTENDS("WI", WidthWI)
        make_EXTENDS("WQ", WidthWQ)
        make_EXTENDS("WW", WidthWW)


    # REDOR, REDAND, REDXOR
    def make_REDUCE(op: str, suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_RED{op}_{suffix}(lp)"
            if suffix[0] == "I" or suffix[0] == "W" or suffix[0] == "Q":
                funcImpl = f"op = VL_RED{op}_{suffix}(lbits, lp)"
            if op == "OR" and suffix != "W":
                funcImpl = f"op = VL_RED{op}_{suffix}(lp)"
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
        make_REDUCE("OR", "I", Width1I)
        make_REDUCE("OR", "Q", Width1Q)
        make_REDUCE("OR", "W", Width1W)

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
            funcImpl = f"op = VL_COUNTONES_{suffix}(lp)"
            if suffix.startswith("W"):
                funcImpl = f"op = VL_COUNTONES_{suffix}(VL_WORDS_I(lbits), lp)"
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
        make_COUNTONES("I", WidthII)
        make_COUNTONES("Q", WidthIQ)
        make_COUNTONES("W", WidthIW)

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
        make_BITWISE("AND", "W", WidthW3)
    if shouldProfile("OR"):
        make_BITWISE("OR", "W", WidthW3)
    if shouldProfile("XOR"):
        make_BITWISE("XOR", "W", WidthW3)
        # make_BITWISE("CHANGEXOR", "W", WidthW3)
    if shouldProfile("NOT"):
        make_BITWISE("NOT", "W", WidthW3)


    # GTS, GTES, LTS, LTES
    def make_COMPARE_SIGNED(op: str, suffix: str, cases):
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

    def make_COMPARE_UNSIGNED(op: str, suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_{op}_{suffix}(VL_WORDS_I(lbits), lp, rp)"
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
        make_COMPARE_SIGNED("GTS", "III", WidthII2)
        make_COMPARE_SIGNED("GTS", "IQQ", WidthIQ2)
        make_COMPARE_SIGNED("GTS", "IWW", WidthIW2)

    if shouldProfile("GT"):
        make_COMPARE_UNSIGNED("GT", "W", WidthIW2)

    if shouldProfile("GTES"):
        make_COMPARE_SIGNED("GTES", "III", WidthII2)
        make_COMPARE_SIGNED("GTES", "IQQ", WidthIQ2)
        make_COMPARE_SIGNED("GTES", "IWW", WidthIW2)

    if shouldProfile("GTE"):
        make_COMPARE_UNSIGNED("GTE", "W", WidthIW2)

    if shouldProfile("LTS"):
        make_COMPARE_SIGNED("LTS", "III", WidthII2)
        make_COMPARE_SIGNED("LTS", "IQQ", WidthIQ2)
        make_COMPARE_SIGNED("LTS", "IWW", WidthIW2)

    if shouldProfile("LT"):
        make_COMPARE_UNSIGNED("LT", "W", WidthIW2)

    if shouldProfile("LTES"):
        make_COMPARE_SIGNED("LTES", "III", WidthII2)
        make_COMPARE_SIGNED("LTES", "IQQ", WidthIQ2)
        make_COMPARE_SIGNED("LTES", "IWW", WidthIW2)

    if shouldProfile("LTE"):
        make_COMPARE_UNSIGNED("LTE", "W", WidthIW2)

    if shouldProfile("EQ"):
        make_COMPARE_UNSIGNED("EQ", "W", WidthIW2)

    if shouldProfile("NEQ"):
        make_COMPARE_UNSIGNED("NEQ", "W", WidthIW2)


    def make_COMPARE_UNSIGNED(op: str, suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_{op}_{suffix}(VL_WORDS_I(lbits), lp, rp)"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_{op}_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR, repeats=REPEATS)
            gen.build()
            return gen

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
        make_NEGATE("W", WidthW3)


    # ADD and SUB, MUL_W
    def make_ARITH(op: str, suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"VL_{op}_{suffix}(words, op, lp, rp)"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_{op}_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR,
                                    repeats=MUL_W_REPEATS if (suffix.startswith("W") and op == "MUL")
                                               else REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_{op}_{suffix}")

    if shouldProfile("ADD"):
        make_ARITH("ADD", "W", WidthW3)
    if shouldProfile("SUB"):
        make_ARITH("SUB", "W", WidthW3)
    if shouldProfile("MUL"):
        make_ARITH("MUL", "W", WidthW3)


    def make_MULS(suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = VL_MULS_{suffix}(lbits, lp, rp)"
            if suffix.startswith("W"):
                funcImpl = f"VL_MULS_{suffix}(lbits, op, lp, rp)"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_MULS_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR,
                                    repeats=MUL_W_REPEATS if suffix.startswith("W") else REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_MULS_{suffix}")

    if shouldProfile("MULS"):
        make_MULS("III", WidthI3)
        make_MULS("QQQ", WidthQ3)
        make_MULS("WWW", WidthW3)



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




    def make_NATIVE_BINOP(op: str, name: str,  suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = lp {op} rp"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_NATIVE_{name}_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR, repeats=REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_NATIVE_{name}_{suffix}")

    def make_NATIVE_UNIOP(op: str, name: str,  suffix: str, cases):
        def generateCode(obits: int, lbits: int, rbits: int):
            funcImpl = f"op = {op} lp"
            gen =  VertexGenerator(
                VlFunc(
                        name = f"VL_NATIVE_{name}_{suffix}_{obits}_{lbits}_{rbits}",
                        func = funcImpl
                    ), obits=obits, lbits=lbits, rbits=rbits,
                                    supervisor=SUPERVISOR, repeats=REPEATS)
            gen.build()
            return gen
        runCases(generateCode, cases, f"VL_NATIVE_{name}_{suffix}")


    if shouldProfile("NATIVE"):
        l32 = [(32, 32, 32)]
        l64 = [(64, 64, 64)]
        lI64 = [(32, 64, 64)]
        make_NATIVE_BINOP("+", "ADD", "I", l32)
        make_NATIVE_BINOP("+", "ADD", "Q", l64)

        make_NATIVE_BINOP("-", "SUB", "I", l32)
        make_NATIVE_BINOP("-", "SUB", "Q", l64)


        make_NATIVE_BINOP("*", "MUL", "I", l32)
        make_NATIVE_BINOP("*", "MUL", "Q", l64)

        make_NATIVE_BINOP("<<", "SHIFTL", "I", l32)
        make_NATIVE_BINOP(">>", "SHIFTR", "I", l32)

        make_NATIVE_BINOP("==", "EQ", "I", l32)
        make_NATIVE_BINOP("==", "EQ", "Q", lI64)
        make_NATIVE_BINOP(">", "GT", "I", l32)
        make_NATIVE_BINOP("<", "LT", "I", l32)
        make_NATIVE_BINOP(">=", "GTE", "I", l32)
        make_NATIVE_BINOP("<=", "LTE", "I", l32)
        make_NATIVE_BINOP(">", "GT", "Q", lI64)
        make_NATIVE_BINOP("<", "LT", "Q", lI64)
        make_NATIVE_BINOP(">=", "GTE", "Q", lI64)
        make_NATIVE_BINOP("<=", "LTE", "Q", lI64)

        make_NATIVE_BINOP("!=", "NEQ", "I", l32)
        make_NATIVE_BINOP("!=", "NEQ", "Q", lI64)



        make_NATIVE_BINOP("&", "AND", "I", l32)
        make_NATIVE_BINOP("&", "AND", "Q", l64)

        make_NATIVE_BINOP("|", "OR", "I", l32)
        make_NATIVE_BINOP("|", "OR", "Q", l64)

        make_NATIVE_BINOP("^", "XOR", "I", l32)
        make_NATIVE_BINOP("^", "XOR", "Q", l64)

        make_NATIVE_UNIOP("~", "NOT", "I", l32)
        make_NATIVE_UNIOP("~", "NOT", "Q", l64)






