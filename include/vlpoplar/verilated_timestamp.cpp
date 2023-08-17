#include <cassert>
#include <ipu_intrinsics>
#include <print.h>

#include <poplar/Vertex.hpp>
using namespace poplar;
#ifndef VL_IPU_TRACE_BUFFER_SIZE
#error "VL_IPU_TRACE_BUFFER_SIZE is not defined!"
#endif
#if (VL_IPU_TRACE_BUFFER_SIZE <= 2) || (VL_IPU_TRACE_BUFFER_SIZE % 2)
#error "expected VL_IPU_TRACE_BUFFER_SIZE >= 2 and it should also be even"
#endif
#if ((VL_IPU_TRACE_BUFFER_SIZE - 1) & VL_IPU_TRACE_BUFFER_SIZE)
#error "Expected VL_IPU_TRACE_BUFFER_SIZE to be power of 2"
#endif

class VlTimeStamp : public SupervisorVertex {
public:

    using VecType = Vector<uint32_t, VectorLayout::COMPACT_PTR, alignof(uint64_t)>;
    InOut<VecType> buffer;
    InOut<VecType> totCount;
    Output<VecType> overflow;
    __attribute__((target("supervisor"))) void compute() {
        uint32_t l = -1, u = -1, l2 = -1;
        uint32_t cnt = totCount[0];
        do {
            l = __builtin_ipu_get_scount_l();
            u = __builtin_ipu_get_scount_u();
            l2 = __builtin_ipu_get_scount_l();
        } while (l2 < l);
        const uint64_t ts = (static_cast<uint64_t>(u) << 32ull) | static_cast<uint64_t>(l2);
        // if (ts == 0) {
        //     printf("Error\n");
        // } else {
            // printf("TS %lu\n", ts);
        // }
        const uint32_t index = cnt % VL_IPU_TRACE_BUFFER_SIZE;
        const uint32_t index2 = index << 1;
        buffer[index2] = l2;
        buffer[index2 + 1] = u;
        overflow[0] = (index == VL_IPU_TRACE_BUFFER_SIZE - 1);
        totCount[0] = cnt + 1;
    }
};

class VlLogicalOrInPlace : public SupervisorVertex {
public:
    InOut<Vector<uint32_t>> a;
    Input<Vector<uint32_t>> b;
    __attribute__((target("supervisor"))) void compute() {
        uint32_t al = a[0];
        uint32_t bl = b[0];
        a[0] = (al || bl);
    }
};