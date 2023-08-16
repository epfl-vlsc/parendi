#include <ipu_intrinsics>

#include <poplar/Vertex.hpp>

#include <print.h>
#include <cassert>
using namespace poplar;

#if (VL_IPU_TRACE_BUFFER_SIZE <= 2) || (VL_IPU_TRACE_BUFFER_SIZE % 2)
#error "expected VL_IPU_TRACE_BUFFER_SIZE >= 32 and it should also be even"
#endif
#if ((VL_IPU_TRACE_BUFFER_SIZE - 1) & VL_IPU_TRACE_BUFFER_SIZE)
#error "Expected VL_IPU_TRACE_BUFFER_SIZE to be power of 2"
#endif

class VlTimeStamp : public SupervisorVertex {
public:
    Output<Vector<uint64_t>> buffer;
    InOut<Vector<uint32_t>> totCount;
    __attribute__((target("supervisor"))) void compute() {
        uint32_t l = -1, u = -1, l2 = -1;
        uint32_t cnt = totCount[0];
        do {
            l = __builtin_ipu_get_scount_l();
            u = __builtin_ipu_get_scount_u();
            l2 = __builtin_ipu_get_scount_l();
        } while (l2 < l);
        uint64_t ts = (static_cast<uint64_t>(u) << 32ull) | static_cast<uint64_t>(l);
        buffer[cnt % VL_IPU_TRACE_BUFFER_SIZE] = ts;
        cnt = cnt + 1;
        totCount[0] = cnt;
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