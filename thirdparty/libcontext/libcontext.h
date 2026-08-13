/*
 *
 *   libcontext - a slightly more portable version of boost::context
 *
 *   Copyright Martin Husemann 2013.
 *   Copyright Oliver Kowalke 2009.
 *   Copyright Sergue E. Leontiev 2013.
 *   Copyright Thomas Sailer 2013.
 *   Minor modifications by Tomasz Wlostowski 2016.
 *
 *  Distributed under the Boost Software License, Version 1.0.
 *     (See accompanying file LICENSE_1_0.txt or copy at
 *           http://www.boost.org/LICENSE_1_0.txt)
 *
 */

#ifndef __LIBCONTEXT_H
#define __LIBCONTEXT_H

#include <cstddef>
#include <cstdint>
#include <cstdio>


#if defined(__EMSCRIPTEN__)
    // WebAssembly/Emscripten. Two backends live in libcontext.cpp's wasm32
    // section: the historical Asyncify/fiber one, and (PCBJAM_JSPI) one where
    // each coroutine is a WebAssembly.promising activation. Under JSPI the
    // engine manages the native stacks and the backend allocates the C spill
    // region itself, so the caller must NOT allocate a stack.
    #if defined(PCBJAM_JSPI)
        #define LIBCONTEXT_HAS_OWN_STACK
    #else
        #undef LIBCONTEXT_HAS_OWN_STACK
    #endif
    #define LIBCONTEXT_COMPILER_gcc
    #define LIBCONTEXT_PLATFORM_wasm32
    #define LIBCONTEXT_CALL_CONVENTION

#elif defined(__GNUC__) || defined(__APPLE__) || defined(__FreeBSD__)

    #undef LIBCONTEXT_HAS_OWN_STACK

    #define LIBCONTEXT_COMPILER_gcc

    #if defined(__linux__) || defined(__FreeBSD__)
    #if defined(__x86_64__) || defined(__amd64__)
        #define LIBCONTEXT_PLATFORM_linux_x86_64
        #define LIBCONTEXT_CALL_CONVENTION
    #elif __i386__
        #define LIBCONTEXT_PLATFORM_linux_i386
        #define LIBCONTEXT_CALL_CONVENTION
    #elif __arm__
        #define LIBCONTEXT_PLATFORM_linux_arm32
        #define LIBCONTEXT_CALL_CONVENTION
    #elif __aarch64__
        #define LIBCONTEXT_PLATFORM_linux_arm64
        #define LIBCONTEXT_CALL_CONVENTION
    #elif (__loongarch__ && _LOONGARCH_SIM == _ABILP64)
        #define LIBCONTEXT_PLATFORM_linux_loong64
        #define LIBCONTEXT_CALL_CONVENTION
    #elif (__mips__ && _MIPS_SIM == _ABI64)
        #define LIBCONTEXT_PLATFORM_linux_mips_n64
        #define LIBCONTEXT_CALL_CONVENTION
    #elif __powerpc__
        #ifdef _ARCH_PPC64
            #define LIBCONTEXT_PLATFORM_linux_ppc64
            #define LIBCONTEXT_CALL_CONVENTION
        #elif defined _ARCH_PPC
            #define LIBCONTEXT_PLATFORM_linux_ppc32
            #define LIBCONTEXT_CALL_CONVENTION
        #endif
    #elif defined(__riscv)
        #if __riscv_xlen == 64
            #define LIBCONTEXT_PLATFORM_linux_riscv64
            #define LIBCONTEXT_CALL_CONVENTION
        #endif
    #endif

    #elif defined(__MINGW32__) || defined(__MINGW64__)
    #if defined(__x86_64__)
        #define LIBCONTEXT_COMPILER_gcc
            #define LIBCONTEXT_PLATFORM_windows_x86_64
        #define LIBCONTEXT_CALL_CONVENTION
    #endif

    #if defined(__i386__)
        #define LIBCONTEXT_COMPILER_gcc
        #define LIBCONTEXT_PLATFORM_windows_i386
        #define LIBCONTEXT_CALL_CONVENTION __cdecl
    #endif
    #elif defined(__APPLE__) && defined(__MACH__)
    #if defined(__i386__)
        #define LIBCONTEXT_PLATFORM_apple_i386
        #define LIBCONTEXT_CALL_CONVENTION
    #elif defined(__x86_64__)
        #define LIBCONTEXT_PLATFORM_apple_x86_64
        #define LIBCONTEXT_CALL_CONVENTION
    #elif __aarch64__
        #define LIBCONTEXT_PLATFORM_apple_arm64
        #define LIBCONTEXT_CALL_CONVENTION
    #endif
    #endif
#elif defined (_MSC_VER )

#define LIBCONTEXT_CALL_CONVENTION __cdecl

#if defined(_WIN64)
	#define LIBCONTEXT_PLATFORM_msvc_x86_64
#elif defined(_WIN32)
	#define LIBCONTEXT_PLATFORM_msvc_i386
#elif defined( _M_ARM64 )
    #define LIBCONTEXT_PLATFORM_msvc_arm64
#endif
#endif

#ifdef __cplusplus
namespace libcontext {
#endif

typedef void* fcontext_t;

#ifdef __cplusplus
extern "C" {
#endif

void LIBCONTEXT_CALL_CONVENTION release_fcontext( fcontext_t ctx );

intptr_t LIBCONTEXT_CALL_CONVENTION jump_fcontext( fcontext_t* ofc, fcontext_t nfc,
        intptr_t vp, bool preserve_fpu = true );
fcontext_t LIBCONTEXT_CALL_CONVENTION make_fcontext( void* sp, size_t size,
        void (* fn)( intptr_t ) );

#if defined(PCBJAM_JSPI)
// JSPI backend only: the coroutine entry (COROUTINE::callerStub) calls this
// right before its FINAL jump back to the caller. It tells the backend that
// the next yield is a completion: instead of suspending the activation forever
// (leaking its engine-managed stack), the backend resolves the caller and lets
// the entry export return, completing the activation.
void LIBCONTEXT_CALL_CONVENTION finish_fcontext( fcontext_t ctx );
#endif

#ifdef __cplusplus
}    // namespace
#endif

#ifdef __cplusplus
}    // extern "C"
#endif

#endif
