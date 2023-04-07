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
/// \brief Verilated/Verilator common header for OS portability
///
/// This header is included by user wrappers and defines the Verilated
/// public-facing API.
///
/// User wrapper code does not generally need to include this, instead
/// include verilated.h.
///
/// This header is used by both the Verilator source code (run on the build
/// and host system), and the Verilated output (run on the target system).
///
/// Configuration code needed by only the host system is in
/// config_build.h.in, code needed by Verilated code only is in
/// verilated.h, and code needed by both is here (verilatedos.h).
///
//*************************************************************************

#ifndef VERILATOR_VERILATEDOS_H_
#define VERILATOR_VERILATEDOS_H_

// Current clang-format versions botch #ifdef inclusion, so
// clang-format off
//=========================================================================
// Compiler pragma abstraction

#ifdef __GNUC__
# define VL_ATTR_ALIGNED(alignment) __attribute__((aligned(alignment)))
# define VL_ATTR_ALWINLINE __attribute__((always_inline)) inline
# define VL_ATTR_NOINLINE __attribute__((noinline))
# define VL_ATTR_COLD __attribute__((cold))
# define VL_ATTR_HOT __attribute__((hot))
# define VL_ATTR_NORETURN __attribute__((noreturn))
// clang and gcc-8.0+ support no_sanitize("string") style attribute
# if defined(__clang__) || (__GNUC__ >= 8)
#  define VL_ATTR_NO_SANITIZE_ALIGN __attribute__((no_sanitize("alignment")))
#else  // The entire undefined sanitizer has to be disabled for older gcc
#  define VL_ATTR_NO_SANITIZE_ALIGN __attribute__((no_sanitize_undefined))
#endif
# define VL_ATTR_PRINTF(fmtArgNum) __attribute__((format(printf, (fmtArgNum), (fmtArgNum) + 1)))
# define VL_ATTR_PURE __attribute__((pure))
# define VL_ATTR_UNUSED __attribute__((unused))
# if !defined(_WIN32) && !defined(__MINGW32__)
// All VL_ATTR_WEAK symbols must be marked with the macOS -U linker flag in verilated.mk.in
#  define VL_ATTR_WEAK __attribute__((weak))
# endif
# if defined(__clang__)
#  define VL_ACQUIRE(...) __attribute__((annotate("ACQUIRE"))) __attribute__((acquire_capability(__VA_ARGS__)))
#  define VL_ACQUIRE_SHARED(...) __attribute__((annotate("ACQUIRE_SHARED"))) __attribute__((acquire_shared_capability(__VA_ARGS__)))
#  define VL_RELEASE(...) __attribute__((annotate("RELEASE"))) __attribute__((release_capability(__VA_ARGS__)))
#  define VL_RELEASE_SHARED(...) __attribute__((annotate("RELEASE_SHARED"))) __attribute__((release_shared_capability(__VA_ARGS__)))
#  define VL_TRY_ACQUIRE(...) __attribute__((try_acquire_capability(__VA_ARGS__)))
#  define VL_TRY_ACQUIRE_SHARED(...) __attribute__((try_acquire_shared_capability(__VA_ARGS__)))
#  define VL_CAPABILITY(x) __attribute__((capability(x)))
#  define VL_REQUIRES(x) __attribute__((annotate("REQUIRES"))) __attribute__((requires_capability(x)))
#  define VL_GUARDED_BY(x) __attribute__((annotate("GUARDED_BY"))) __attribute__((guarded_by(x)))
#  define VL_EXCLUDES(x) __attribute__((annotate("EXCLUDES"))) __attribute__((locks_excluded(x)))
#  define VL_SCOPED_CAPABILITY __attribute__((scoped_lockable))
# endif
# define VL_LIKELY(x) __builtin_expect(!!(x), 1)  // Prefer over C++20 [[likely]]
# define VL_UNLIKELY(x) __builtin_expect(!!(x), 0)  // Prefer over C++20 [[unlikely]]
# define VL_UNREACHABLE __builtin_unreachable()  // C++23 std::unreachable()
# define VL_PREFETCH_RD(p) __builtin_prefetch((p), 0)
# define VL_PREFETCH_RW(p) __builtin_prefetch((p), 1)
#endif

// Defaults for unsupported compiler features
#ifndef VL_ATTR_ALIGNED
# define VL_ATTR_ALIGNED(alignment)  ///< Attribute to align structure to byte alignment
#endif
#ifndef VL_ATTR_ALWINLINE
# define VL_ATTR_ALWINLINE  ///< Attribute to inline, even when not optimizing
#endif
#ifndef VL_ATTR_NOINLINE
# define VL_ATTR_NOINLINE  ///< Attribute to never inline, even when optimizing
#endif
#ifndef VL_ATTR_COLD
# define VL_ATTR_COLD  ///< Attribute that function is rarely executed
#endif
#ifndef VL_ATTR_HOT
# define VL_ATTR_HOT  ///< Attribute that function is highly executed
#endif
#ifndef VL_ATTR_NORETURN
# define VL_ATTR_NORETURN  ///< Attribute that function does not ever return
#endif
#ifndef VL_ATTR_NO_SANITIZE_ALIGN
# define VL_ATTR_NO_SANITIZE_ALIGN  ///< Attribute that function contains intended unaligned access
#endif
#ifndef VL_ATTR_PRINTF
# define VL_ATTR_PRINTF(fmtArgNum)  ///< Attribute for function with printf format checking
#endif
#ifndef VL_ATTR_PURE
# define VL_ATTR_PURE  ///< Attribute that function is pure (and thus also VL_MT_SAFE)
#endif
#ifndef VL_ATTR_UNUSED
# define VL_ATTR_UNUSED  ///< Attribute that function that may be never used
#endif
#ifndef VL_ATTR_WEAK
# define VL_ATTR_WEAK  ///< Attribute that function external that is optionally defined
#endif
#ifndef VL_CAPABILITY
# define VL_ACQUIRE(...)  ///< Function acquires a capability/lock (-fthread-safety)
# define VL_ACQUIRE_SHARED(...)  ///< Function acquires a shared capability/lock (-fthread-safety)
# define VL_RELEASE(...)  ///< Function releases a capability/lock (-fthread-safety)
# define VL_RELEASE_SHARED(...)  ///< Function releases a shared capability/lock (-fthread-safety)
# define VL_TRY_ACQUIRE(...)  ///< Function returns bool if acquired a capability (-fthread-safety)
# define VL_TRY_ACQUIRE_SHARED(...)  ///< Function returns bool if acquired shared (-fthread-safety)
# define VL_REQUIRES(x)  ///< Function requires a capability inbound (-fthread-safety)
# define VL_EXCLUDES(x)  ///< Function requires not having a capability inbound (-fthread-safety)
# define VL_CAPABILITY(x)  ///< Name of capability/lock (-fthread-safety)
# define VL_GUARDED_BY(x)  ///< Name of mutex protecting this variable (-fthread-safety)
# define VL_SCOPED_CAPABILITY  ///< Scoped threaded capability/lock (-fthread-safety)
#endif
#ifndef VL_LIKELY
# define VL_LIKELY(x) (!!(x))  ///< Return boolean expression that is more often true
# define VL_UNLIKELY(x) (!!(x))  ///< Return boolean expression that is more often false
#endif
/// Boolean expression never hit by users (branch coverage disabled)
# define VL_UNCOVERABLE(x) VL_UNLIKELY(x)
#ifndef VL_UNREACHABLE
# define VL_UNREACHABLE  ///< Statement that may never be reached (for coverage etc)
#endif
#ifndef VL_PREFETCH_RD
# define VL_PREFETCH_RD(p)  ///< Prefetch pointer argument with read intent
#endif
#ifndef VL_PREFETCH_RW
# define VL_PREFETCH_RW(p)  ///< Prefetch pointer argument with read/write intent
#endif


#ifndef VL_NO_LEGACY
# define VL_FUNC __func__  // Deprecated
# define VL_THREAD  // Deprecated
# define VL_THREAD_LOCAL thread_local  // Deprecated
# define VL_STATIC_OR_THREAD static  // Deprecated
#endif

// Comment tag that Function is pure (and thus also VL_MT_SAFE)
#if defined(__clang__)
# define VL_PURE __attribute__((annotate("PURE")))
#else
# define VL_PURE
#endif
// Comment tag that function is threadsafe
#if defined(__clang__)
# define VL_MT_SAFE __attribute__((annotate("MT_SAFE")))
#else
# define VL_MT_SAFE
#endif
// Comment tag that function is threadsafe, only
// during normal operation (post-init)
#if defined(__clang__)
# define VL_MT_SAFE_POSTINIT __attribute__((annotate("MT_SAFE_POSTINIT")))
#else
# define VL_MT_SAFE_POSTINIT
#endif
// Attribute that function is clang threadsafe and uses given mutex
#if defined(__clang__)
# define VL_MT_SAFE_EXCLUDES(mutex) __attribute__((annotate("MT_SAFE_EXCLUDES"))) VL_EXCLUDES(mutex)
#else
# define VL_MT_SAFE_EXCLUDES(mutex) VL_EXCLUDES(mutex)
#endif
// Comment tag that function is not threadsafe
#if defined(__clang__)
# define VL_MT_UNSAFE __attribute__((annotate("MT_UNSAFE")))
#else
# define VL_MT_UNSAFE
#endif
// Comment tag that function is not threadsafe
// protected to make sure single-caller
#if defined(__clang__)
# define VL_MT_UNSAFE_ONE __attribute__((annotate("MT_UNSAFE_ONE")))
#else
# define VL_MT_UNSAFE_ONE
#endif
// Comment tag that function is entry point of parallelization
#if defined(__clang__)
# define VL_MT_START __attribute__((annotate("MT_START")))
#else
# define VL_MT_START
#endif

#ifndef VL_NO_LEGACY
# define VL_ULL(c) (c##ULL)  // Add appropriate suffix to 64-bit constant (deprecated)
#endif

// Convert argument to IData
// This is not necessarily the same as "#UL", depending on what the IData typedef is.
#define VL_UL(c) (static_cast<IData>(c##UL))

#if defined(VL_CPPCHECK) || defined(__clang_analyzer__) || __cplusplus < 201103L
# define VL_DANGLING(var)
#else
/// After e.g. delete, set variable to nullptr to indicate must not use later
# define VL_DANGLING(var) \
    do { \
        *const_cast<const void**>(reinterpret_cast<const void* const*>(&var)) = nullptr; \
    } while (false)
#endif

/// Perform an e.g. delete, then set variable to nullptr to indicate must not use later.
/// Unlike VL_DO_CLEAR the setting of the variable is only for debug reasons.
#define VL_DO_DANGLING(stmt, var) \
    do { \
        do { \
            stmt; \
        } while (false); \
        VL_DANGLING(var); \
    } while (false)

/// Perform an e.g. delete, then set variable to nullptr as a requirement
#define VL_DO_CLEAR(stmt, stmt2) \
    do { \
        do { \
            stmt; \
        } while (false); \
        do { \
            stmt2; \
        } while (false); \
    } while (false)

#ifdef _MSC_VER
# if _MSC_VER < 1929
#  error "Verilator requires at least Visual Studio 2019 version 16.11.2"
# endif
#endif

//=========================================================================
// C++-2011

#if __cplusplus >= 201103L || defined(__GXX_EXPERIMENTAL_CXX0X__) || defined(VL_CPPCHECK) || defined(_MSC_VER)
#else
# error "Verilator requires a C++11 or newer compiler"
#endif



//=========================================================================
// C++-2017

#if __cplusplus >= 201703L
# define VL_CONSTEXPR_CXX17 constexpr
#else
# define VL_CONSTEXPR_CXX17
#endif


//=========================================================================
// Optimization

#ifndef VL_INLINE_OPT
# define VL_INLINE_OPT  VL_ATTR_ALWINLINE
#endif



//=========================================================================
// Integer size macros

#define VL_BYTESIZE 8  ///< Bits in a CData / byte
#define VL_SHORTSIZE 16  ///< Bits in a SData / short
#define VL_IDATASIZE 32  ///< Bits in an IData / word
#define VL_QUADSIZE 64  ///< Bits in a QData / quadword
#define VL_EDATASIZE 32  ///< Bits in an EData (WData entry)
#define VL_EDATASIZE_LOG2 5  ///< log2(VL_EDATASIZE)
#define VL_CACHE_LINE_BYTES 64  ///< Bytes in a cache line (for alignment)


/// Return number of bytes argument-number of bits needs (1 bit=1 byte)
#define VL_BYTES_I(nbits) (((nbits) + (VL_BYTESIZE - 1)) / VL_BYTESIZE)
/// Return Words/EDatas in argument-number of bits needs (1 bit=1 word)
#define VL_WORDS_I(nbits) (((nbits) + (VL_EDATASIZE - 1)) / VL_EDATASIZE)
// Number of Words/EDatas a quad requires
#define VL_WQ_WORDS_E VL_WORDS_I(VL_QUADSIZE)

//=========================================================================
// Class definition helpers

// Comment tag to indicate a base class, e.g. cannot label "class final"
#define VL_NOT_FINAL

// Declare a class as uncopyable; put after a private:
#define VL_UNCOPYABLE(Type) \
    Type(const Type& other) = delete; \
    Type& operator=(const Type&) = delete

//=========================================================================
// Verilated function size macros

#define VL_MULS_MAX_WORDS 16  ///< Max size in words of MULS operation

#ifndef VL_VALUE_STRING_MAX_WORDS
    #define VL_VALUE_STRING_MAX_WORDS 64  ///< Max size in words of String conversion operation
#endif

#define VL_VALUE_STRING_MAX_CHARS (VL_VALUE_STRING_MAX_WORDS * VL_EDATASIZE / VL_BYTESIZE)

//=========================================================================
// Base macros

#define VL_SIZEBITS_I (VL_IDATASIZE - 1)  ///< Bit mask for bits in a word
#define VL_SIZEBITS_Q (VL_QUADSIZE - 1)  ///< Bit mask for bits in a quad
#define VL_SIZEBITS_E (VL_EDATASIZE - 1)  ///< Bit mask for bits in a quad

/// Return mask for words with 1's where relevant bits are (0=all bits)
#define VL_MASK_I(nbits) (((nbits) & VL_SIZEBITS_I) ? ((1U << ((nbits) & VL_SIZEBITS_I)) - 1) : ~0)
/// Return mask for quads with 1's where relevant bits are (0=all bits)
#define VL_MASK_Q(nbits) \
    (((nbits) & VL_SIZEBITS_Q) ? ((1ULL << ((nbits) & VL_SIZEBITS_Q)) - 1ULL) : ~0ULL)
/// Return mask for EData with 1's where relevant bits are (0=all bits)
#define VL_MASK_E(nbits) VL_MASK_I(nbits)

#define VL_EUL(n) VL_UL(n)  // Make constant number EData sized

#define VL_BITWORD_I(bit) ((bit) / VL_IDATASIZE)  ///< Word number for sv DPI vectors
#define VL_BITWORD_E(bit) ((bit) >> VL_EDATASIZE_LOG2)  ///< Word number for a wide quantity
#define VL_BITBIT_I(bit) ((bit) & VL_SIZEBITS_I)  ///< Bit number for a bit in a long
#define VL_BITBIT_Q(bit) ((bit) & VL_SIZEBITS_Q)  ///< Bit number for a bit in a quad
#define VL_BITBIT_E(bit) ((bit) & VL_SIZEBITS_E)  ///< Bit number for a bit in an EData

// Return true if data[bit] set; not 0/1 return, but 0/non-zero return.
#define VL_BITISSET_I(data, bit) ((data) & (VL_UL(1) << VL_BITBIT_I(bit)))
#define VL_BITISSET_Q(data, bit) ((data) & (1ULL << VL_BITBIT_Q(bit)))
#define VL_BITISSET_E(data, bit) ((data) & (VL_EUL(1) << VL_BITBIT_E(bit)))
#define VL_BITISSET_W(data, bit) ((data)[VL_BITWORD_E(bit)] & (VL_EUL(1) << VL_BITBIT_E(bit)))

//=========================================================================
// Floating point
// #defines, to avoid requiring math.h on all compile runs

# define VL_TRUNC(n) std::trunc(n)
# define VL_ROUND(n) std::round(n)



// clang-format on

//=========================================================================
// Stringify macros

#define VL_STRINGIFY(x) VL_STRINGIFY2(x)
#define VL_STRINGIFY2(x) #x

//=========================================================================
// Offset of field in type

// Address zero can cause compiler problems
#define VL_OFFSETOF(type, field) \
    (reinterpret_cast<size_t>(&(reinterpret_cast<type*>(0x10000000)->field)) - 0x10000000)

//=========================================================================
// Conversions

#include <utility>

namespace vlstd {

template <typename T>
struct reverse_wrapper {
    const T& m_v;

    explicit reverse_wrapper(const T& a_v)
        : m_v(a_v) {}
    auto begin() -> decltype(m_v.rbegin()) { return m_v.rbegin(); }
    auto end() -> decltype(m_v.rend()) { return m_v.rend(); }
};

// C++20's std::ranges::reverse_view
template <typename T>
reverse_wrapper<T> reverse_view(const T& v) {
    return reverse_wrapper<T>(v);
}

// C++17's std::as_const
template <class T>
T const& as_const(T& v) {
    return v;
}

// C++14's std::exchange
template <class T, class U = T>
T exchange(T& obj, U&& new_value) {
    T old_value = std::move(obj);
    obj = std::forward<U>(new_value);
    return old_value;
}

};  // namespace vlstd

//=========================================================================

#endif  // Guard
