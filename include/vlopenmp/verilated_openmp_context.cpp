// verilated_openmp_context.cpp

#include VPROGRAM_HEADER /*defined by the Makefile*/

#include <vlopenmp/verilated_openmp.h>

#include <omp.h>


int main(int argc, char* argv[]) {
    VlOpenMPContext ctx;
    OpenMPRoot* root = &RootObject;
    root->setCtx(&ctx);
    root->setNumCores(root->NUM_CORES);
    root->setCodelets(CodeletInstances);
    root->setinitCodelets(CodeletInitInstances);
}