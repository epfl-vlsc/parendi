// verilated_openmp_context.cpp

#include VPROGRAM_HEADER /*defined by the Makefile*/

#include "verilated.h"


#include <omp.h>
#include "verilated.h"

int main(int argc, char* argv[]) {
    VerilatedContext contextp;
    contextp.commandArgs(argc, argv);

    OpenMPRoot* root = &RootObject;
    root->setNumCores(root->NUM_CORES);
    root->setCodelets(CodeletInstances);
    root->setinitCodelets(CodeletInitInstances);

    MemoryContents& memA = MemIn;
    MemoryContents& memB = MemOut;

    // Initialize the world.
    for (int i = 0; CodeletInitInstances[i] != nullptr; i++) {
        CodeletInitInstances[i]->compute(memA, memB);
    }

    // Run the computation.
#pragma omp parallel for
    for (int i = 0; CodeletInstances[i] != nullptr; i++) {
        auto codeletForThread = CodeletInstances[i];
        while (true) {
            codeletForThread->compute(memA, memB);
#pragma barrier
            codeletForThread->compute(memB, memA);
#pragma barrier
        }
    }
}