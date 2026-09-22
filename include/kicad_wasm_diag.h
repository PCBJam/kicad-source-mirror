/*
 * WASM debug-logging macros (kicad-wasm fork only — not upstream KiCad).
 *
 * Compile-time gated diagnostic logging for the Emscripten/WASM port. Each
 * category is enabled by a -D define set from the build flag:
 *
 *     ./docker/build.sh --debug --diag=gal,coroutine,ctor   (or --diag=all)
 *
 * which maps to -DKICAD_DIAG_GAL=1 / -DKICAD_DIAG_COROUTINE=1 / -DKICAD_DIAG_CTOR=1.
 *
 * When a category is disabled the macro compiles to nothing (zero overhead).
 * All output goes to STDOUT so it shows up as [KICAD_OUT] logs in the test
 * logs, never as errors. Call sites keep their own "[DIAG_X] ...\n" tag in the
 * format string.
 */

#ifndef KICAD_WASM_DIAG_H
#define KICAD_WASM_DIAG_H

#include <cstdio>

#if defined( KICAD_DIAG_GAL )
#define KI_DIAG_GAL( ... )                                                                         \
    do                                                                                             \
    {                                                                                              \
        std::printf( __VA_ARGS__ );                                                                \
        std::fflush( stdout );                                                                     \
    } while( 0 )
#else
#define KI_DIAG_GAL( ... )                                                                         \
    do                                                                                             \
    {                                                                                              \
    } while( 0 )
#endif

#if defined( KICAD_DIAG_COROUTINE )
#define KI_DIAG_COROUTINE( ... )                                                                   \
    do                                                                                             \
    {                                                                                              \
        std::printf( __VA_ARGS__ );                                                                \
        std::fflush( stdout );                                                                     \
    } while( 0 )
#else
#define KI_DIAG_COROUTINE( ... )                                                                   \
    do                                                                                             \
    {                                                                                              \
    } while( 0 )
#endif

#if defined( KICAD_DIAG_CTOR )
#define KI_DIAG_CTOR( ... )                                                                        \
    do                                                                                             \
    {                                                                                              \
        std::printf( __VA_ARGS__ );                                                                \
        std::fflush( stdout );                                                                     \
    } while( 0 )
#else
#define KI_DIAG_CTOR( ... )                                                                        \
    do                                                                                             \
    {                                                                                              \
    } while( 0 )
#endif

#endif // KICAD_WASM_DIAG_H
