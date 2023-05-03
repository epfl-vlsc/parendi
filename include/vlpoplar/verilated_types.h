// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
//
// Code available from: https://verilator.org
//
// Copyright 2003-2023 by Wilson Snyder. This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
///
/// \file
/// \brief Verilated common data type containers
///
/// verilated.h should be included instead of this file.
///
/// Those macro/function/variable starting or ending in _ are internal,
/// however many of the other function/macros here are also internal.
///
//*************************************************************************

#ifndef VERILATOR_VERILATED_TYPES_H_
#define VERILATOR_VERILATED_TYPES_H_

#ifndef VERILATOR_VERILATED_H_INTERNAL_
#error "verilated_types.h should only be included by verilated.h"
#endif

#include <ipu_intrinsics>



//===================================================================
/// Verilog wide packed bit container.
/// Similar to std::array<WData, N>, but lighter weight, only methods needed
/// by Verilator, to help compile time.
///
/// A 'struct' as we want this to be an aggregate type that allows
/// static aggregate initialization. Consider data members private.
///
/// For example a Verilog "bit [94:0]" will become a VlWide<3> because 3*32
/// bits are needed to hold the 95 bits. The MSB (bit 96) must always be
/// zero in memory, but during intermediate operations in the Verilated
/// internals is unpredictable.

static int _vl_cmp_w(int words, WDataInP const lwp, WDataInP const rwp) VL_PURE;

template <std::size_t T_Words>
struct VlWide final {
    // MEMBERS
    // This should be the only data member, otherwise generated static initializers need updating
    EData m_storage[T_Words];  // Contents of the packed array

    // CONSTRUCTORS
    // Default constructors and destructor are used. Note however that C++20 requires that
    // aggregate types do not have a user declared constructor, not even an explicitly defaulted
    // one.

    // OPERATOR METHODS
    // Default copy assignment operators are used.
    operator WDataOutP() VL_PURE { return &m_storage[0]; }  // This also allows []
    operator WDataInP() const VL_PURE { return &m_storage[0]; }  // This also allows []
    bool operator!=(const VlWide<T_Words>& that) const VL_PURE {
        for (size_t i = 0; i < T_Words; ++i) {
            if (m_storage[i] != that.m_storage[i]) return true;
        }
        return false;
    }

    // METHODS
    const EData& at(size_t index) const { return m_storage[index]; }
    EData& at(size_t index) { return m_storage[index]; }
    WData* data() { return &m_storage[0]; }
    const WData* data() const { return &m_storage[0]; }
    bool operator<(const VlWide<T_Words>& rhs) const {
        return _vl_cmp_w(T_Words, data(), rhs.data()) < 0;
    }
};

// Convert a C array to std::array reference by pointer magic, without copy.
// Data type (second argument) is so the function template can automatically generate.
template <std::size_t T_Words>
VlWide<T_Words>& VL_CVT_W_A(const WDataInP inp, const VlWide<T_Words>&) {
    return *((VlWide<T_Words>*)inp);
}

//===================================================================
/// Verilog unpacked array container
/// For when a standard C++[] array is not sufficient, e.g. an
/// array under a queue, or methods operating on the array.
///
/// A 'struct' as we want this to be an aggregate type that allows
/// static aggregate initialization. Consider data members private.
///
/// This class may get exposed to a Verilated Model's top I/O, if the top
/// IO has an unpacked array.

template <class T_Value, std::size_t T_Depth>
struct VlUnpacked final {
    // MEMBERS
    // This should be the only data member, otherwise generated static initializers need updating
    T_Value m_storage[T_Depth];  // Contents of the unpacked array

    // CONSTRUCTORS
    // Default constructors and destructor are used. Note however that C++20 requires that
    // aggregate types do not have a user declared constructor, not even an explicitly defaulted
    // one.

    // OPERATOR METHODS
    // Default copy assignment operators are used.

    // METHODS
    // Raw access
    WData* data() { return &m_storage[0]; }
    const WData* data() const { return &m_storage[0]; }

    T_Value& operator[](size_t index) { return m_storage[index]; }
    const T_Value& operator[](size_t index) const { return m_storage[index]; }

    // *this != that, which might be used for change detection/trigger computation, but avoid
    // operator overloading in VlUnpacked for safety in other contexts.
    bool neq(const VlUnpacked<T_Value, T_Depth>& that) const { return neq(*this, that); }
    // Similar to 'neq' above, *this = that used for change detection
    void assign(const VlUnpacked<T_Value, T_Depth>& that) { *this = that; }

private:
    template <typename T_Val, std::size_t T_Dep>
    static bool neq(const VlUnpacked<T_Val, T_Dep>& a, const VlUnpacked<T_Val, T_Dep>& b) {
        for (size_t i = 0; i < T_Dep; ++i) {
            // Recursive 'neq', in case T_Val is also a VlUnpacked<_, _>
            if (neq(a.m_storage[i], b.m_storage[i])) return true;
        }
        return false;
    }

    template <typename T_Other>  //
    static bool neq(const T_Other& a, const T_Other& b) {
        // Base case (T_Other is not VlUnpacked<_, _>), fall back on !=
        return a != b;
    }
};





#endif  // Guard
