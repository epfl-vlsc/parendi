# Lab Notes for Parendi
## James Larus, February 26, 2024

### 04-03-2024

- Strip out the Poplar-specific stuff and generate plain code. Can go back later to make it faster.
  - V3BspOpenMPProgram.cpp::OpenMPViewsVisitor changes variable references to VL_ASSIGN_W and adds cast with VL_VIEW.
  - V3BspOpenMPProgram.cpp::OpenMPLegalizeFieldNamesVisitor changes variable name to fieldN.
  - Added V3EmitOpenMPVertex.cpp::emitMemory to produce include file of memory definition (mem_contents).


### 04-04-2024

- Does "VNUser1InUse m_user1Inuse;" have any uses? Eliminate?
- Organizations of memories:

    	class mem_contents {
	      int field0;
	      int field1;
	      int[7] __pad1__;
	    }
	
	    class base {
	      mem_contents memA;
	      mem_contents memB;
	
	      base() {
	        memclr(memA);
	        memclr(memB);
	      }
	
	      run() {
	        memIn = memA;
	        memOut = memB;
	        while (1) {
	          compute(memIn, memOut);
	    #pragma omp barrier
	          memIn = (memIn == memA ? memB : memA);
	          memOut = (memOut == memA ? memB : memA);
	        }
	      }
	    }

	    class core1: base {
	      void compute(mem_contents& memIn, mem_contents& memOut) {
	        memOut.field0 = (int) ...;
	      }
	    }


### 03-03-2024
- Find variable declaration code in Vertex, trace it back to see when variable is first created and add code to compute offsets.
  - Offsets need to be computed for variables that are class variables, but not local (tmp) variables. Need start, size, type. Use assigned name and later dump out .h file assigning offset to name.

- How to transform variable refs (this->field48) into mem references (mem[101])? Could make mem an inherited member field and use #define to change field name: #define field48 mem[101]. So, it goes to this->mem[field101]. With casting, this will work, but it is an ugly use of #define. Also, need casting: ((int*)&this->mem[field101]), so it introduces a level of indirection.
  - Alternative, combine all the declarations into a single object equivalent to mem. So, this->field48 => this->mem.field48. Need to be able to flip between two objects, so another level of indirection? this->mem->field48. Takes two memory dereferences (first clearly belongs in register).

- Where is barrier in Poplar code?


### 02-03-2024
- Reuse IPU classes?
  -Vertex class is the container for code running on a core.
    - Construct to allocate.
    - Parallel dispatch to ::nbaTop or ::compute (::triggerEval?).
    - Change field_N into constant, which is starting location of field in memory. Use literals in code and hope the compiler optimizes them. Need table of offsets so that code out of class gets right place. Maybe dump include file of offsets and continue using symbolic names?
    - Root nodes do not need wiring. Can be constructed to dispatch to threads.

- Use bitcoin benchmark. It has sufficient operations to be interesting and is small enough to compile.

- Options: --openmp flag and --openmp_threads <N>
Cannot reuse --threads flag since it is used in multiple places in compiler.

### 01-03-2024
Got lldb running under vscode. Got past compiler issues. Can generate minimal code that does not compile. Need to strip down Poplar_Context for OpenMP. Need to figure out how to use classes generated for the IPU on x64. Need to undo some deletions, which produced non-working code.


### 29-02-2024
Spent trying to get a minimal version running again. Lots of frustration with C++ and compilers. Hours looking for type error caused by missing parenthesis. Hours figuring out abstract types, singleton, and references. Java is looking good again.


### 28-02-2024
Outline of OpenMP program.
https://www.openmp.org/resources/
(See openmpmodel/test_openmp.cpp for running version.)

    //%compiler: clang
    //%cflags: -fopenmp

    #include <stdio.h>
    #include <stdlib.h>
    #include <omp.h>

    bool Done = false; // Terminate execution

    int main(int argc, char *argv[]){
        // Argument parsing: inputs, etc.
        // Degree of parallelism (NUM_THREADS) is compile-time constant
        // Allocate and initialize two memory regions MemA and MemB
        #pragma omp parallel num_threads(NUM_THREADS), shared(Done)
        dispatchWorkerThread(memA, memB)
        return 0;
    }

    void dispatchWorkerThread(memA, memB) {
        iam = omp_get_thread_num();
        switch (iam) {
          case 0: workerThread0(memA, memB); break;
          ...
          case P-1: workerThreadP-1(memA, memB); break;
          default: error("Bad thread number");
        }
    }

    void workerThreadI(memA, memB){
      memIn = memA
      memOut = memB
      while (!Done) {
        // Body of thread I (fibers)
        #pragma omp barrier
        memIn = (memIn == memA ? memB : memA);
        memOut = (memOut == memA ? memB : memA);
      }
    }


### 27-02-2024
- IPU --> CPU, Poplar --> OpenMP

- Because x64 has global address space, try to simplify IPU/Poplar design in a number of ways.

Basic architecture is that each core will run one thread. Each thread will execute a sequence of fibers, each of which compute one step of part of the computation (a cone). At the end of a sequence, the threads will rendezvous using a barrier and then proceed to the next step. The inputs and outputs will be double-buffered so that only one barrier is necessary: step1 - input A, output B; step 2 - input B, output A; ....

How to store variables in global address space? 
1. Single block of memory (2 copies). Each fiber gets cache-aligned region to store its values so that offsets are consistent across all threads.
2. Global variables (2 copies). Reference variable by name, but make access conditional on A/B flag: (flag ? varA : varB). Or could have two copies of function body, one using varA and the other varB.
First is simpler (just set global point at start of thread), but requires pointer casting. Since it is close to IPU/Polar tensors, implementation should be easy, beyond using a single region instead of multiple tensors. Second introduces control flow (or conditional move), which has significant complexity (can conditional be used as target of assignment?) and r/t cost.

Outputs from a fiber must reside in their own cache lines, which are not shared with other fibers, to avoid write ping-ponging between thread. Fibers that execute in the same thread can (and probably should) share cache lines to reduce memory usage and improve transfers. This implies that fiber scheduling should take into account which outputs are used together, so they can run on the same thread and share cache lines.

A fiber can flush its cache lines when it finishes (CLFLUSH - x64, ? - arm) or perhaps more efficiently, or invalidate the dcache on entry to the barrier. Goal is to speed up the coherence protocol when fibers access data in next step. Needs experimental evaluation.

The IPU implementation interleaves instructions from various fibers so their 6-cycle execution latency can be filled with independent instruction streams. It is not clear that this optimization would be greatly beneficial on x64/arm with dynamic scheduling. Perhaps disable it and experiment later.

A thread can prefetch cache lines of data for yet-to-be-executed fibers, to reduce cache miss stalls. Need to balance benefit against cost of additional instruction execution and possible cache conflicts evicting useful data.

Library: OpenMP Uses a relaxed consistency memory model that requires explicit flushes (between threads). Uses code annotations and gcc/llvm compiler support.


### 26-02-2024
- Numerous files. Suppress the gcc/llvm warnings, most of which were missing "override" declarations or nested if-stmts without braces.
- V3EmitPoplarVertex.cpp::EmitPoplarVertex[171]. Contained VL_MT_SAFE on constructor body, which is not valid syntax. Deleted VL_MT_SAFE.
- V3BspResyncGraph.h::ResyncGraph[105]. Has dynamic_cast of ResyncVertex, which is not yet defined. dynamic_cast requires full type, so it fails with incomplete type error. Comment out UASSERT.
- V3BspGraph.cpp::DisjointSets[449]. Explicit Allocator had wrong type (should be pair), which triggered assert in MapType[455].
- CMakeLists.txt[21]. Need to set C++ version on macOS. CMake defaults to c++11, which does not work with Mahyar's code using more modern features. Set it to c++17.