#include <ipu_intrinsics>

#include <poplar/Vertex.hpp>

#include <print.h>
#include <cassert>
using namespace poplar;
#ifndef VL_IPU_TRACE_BUFFER_SIZE
#define VL_IPU_TRACE_BUFFER_SIZE 512
#endif

#if (VL_IPU_TRACE_BUFFER_SIZE <= 2) || (VL_IPU_TRACE_BUFFER_SIZE % 2)
#error "expected VL_IPU_TRACE_BUFFER_SIZE >= 32 and it should also be even"
#endif

class VlTimeStamp : public SupervisorVertex {
public:
    Output<Vector<uint64_t>> buffer;
    Output<Vector<uint32_t>> overflow;
    InOut<Vector<uint32_t>> count;

    __attribute__((target("supervisor"))) void compute() {
        uint32_t l = -1, u = -1, l2 = -1;
        uint32_t cnt = count[0];

        do {
            l = __builtin_ipu_get_scount_l();
            u = __builtin_ipu_get_scount_u();
            l2 = __builtin_ipu_get_scount_l();
        } while (l2 < l);
        uint64_t ts = (static_cast<uint64_t>(u) << 32ull) | static_cast<uint64_t>(l);
        buffer[cnt] = ts;
        cnt = cnt + 1;
        bool ovf = (cnt == VL_IPU_TRACE_BUFFER_SIZE);
        count[0] = ovf ? 0 : cnt;
        overflow[0] = ovf;
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