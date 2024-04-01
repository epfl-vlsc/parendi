// verilated_openmp_context.cpp

#include VPROGRAM_HEADER /*defined by the Makefile*/

#include "verilated.h"

#include <omp.h>

void runCodelet(CodeletBase* codeletInstance, int iam, MemoryContents* memA, MemoryContents* memB) {
    while (true) {
        codeletInstance->compute(memA, memB);  // f(memA) -> memB
#pragma omp barrier
        MemoryContents* tmp = memA;
        memA = memB;
        memB = tmp;
    }
}

void dispatchWorkerThread(int numCodelets, MemoryContents* memA, MemoryContents* memB) {
    int iam = omp_get_thread_num();
    if (iam < 0 || iam >= numCodelets) { perror("Bad thread number"); }
    runCodelet(CodeletInstances[iam], iam, memA, memB);
}

int main(int argc, char* argv[]) {
    VerilatedContext contextp;
    contextp.commandArgs(argc, argv);

    OpenMPRoot* root = &RootObject;
    root->setNumCores(root->NUM_CORES);
    root->setCodelets(CodeletInstances);
    root->setinitCodelets(CodeletInitInstances);

    MemoryContents* memA = &MemIn;
    MemoryContents* memB = &MemOut;

    // Initialize the world. MemOut (==memB) is written.
    for (int i = 0; CodeletInitInstances[i] != nullptr; i++) {
        CodeletInitInstances[i]->compute(memA, memB);
    }

    // Count the codelets
    int numCodelets = 0;
    for (int i = 0; CodeletInstances[i] != nullptr; i++) { numCodelets += 1; }

    // Run the computation. Start with initialized memory (MemB).
#pragma omp parallel num_threads(numCodelets)
    dispatchWorkerThread(numCodelets, memB, memA);

    return 0;
}



void vl_finish(const char* filename, int linenum, const char* hier) VL_MT_UNSAFE {
    if (false && hier) {}
    VL_PRINTF(  // Not VL_PRINTF_MT, already on main thread
        "- %s:%d: Verilog $finish\n", filename, linenum);
    Verilated::runFlushCallbacks();
    Verilated::runExitCallbacks();
    std::exit(0);
}