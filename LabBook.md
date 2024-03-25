# Lab Notes for Parendi
## James Larus, February 26, 2024

### 26-02-2024
- V3EmitPoplarVertex.cpp::EmitPoplarVertex[171]. Contained VL_MT_SAFE on constructor body, which is not valid syntax. Deleted VL_MT_SAFE.
- V3BspResyncGraph.h::ResyncGraph[105]. Has dynamic_cast of ResyncVertex, which is not yet defined. dynamic_cast requires full type, so it fails with incomplete type error. Comment out UASSERT.
- V3BspGraph.cpp::DisjointSets[449]. Explicit Allocator had wrong type (should be pair), which triggered assert in MapType[455].
- CMakeLists.txt[21]. Need to set C++ version on macOS. CMake defaults to c++11, which does not work with Mahyar's code using more modern features. Set it to c++17.