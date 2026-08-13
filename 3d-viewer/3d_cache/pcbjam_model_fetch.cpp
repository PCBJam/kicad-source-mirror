/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include <wx/filename.h>

#include "pcbjam_model_fetch.h"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

// Unique export names (pcbjam_3d_*) so this bridge never collides with the
// symbol (pcbjam_libs_*) or footprint (pcbjam_fp_*) bridges in the same binary.
extern "C" EMSCRIPTEN_KEEPALIVE char* pcbjam_3d_alloc( int aBytes )
{
    return (char*) malloc( aBytes );
}

extern "C" EMSCRIPTEN_KEEPALIVE void pcbjam_3d_finish( em_proxying_ctx* aCtx )
{
    emscripten_proxy_finish( aCtx );
}

// Main-thread path, Phase E shape (docs/features/async/22 §5, K3): the request
// no longer Asyncify-parks the stack it stands on — token wait via
// wxWasmYieldUntil, resolution ALWAYS deferred to at least a microtask (the
// early-resolve contract, doc 22 §10 Phase E retry entry). The provider fetches
// the model body (IDB, then R2) without touching native state. Its synchronous
// prepared apply writes MEMFS, allocates the result path, and resolves this
// exact wait in one immediate completion edge.
EM_JS( void, pcbjam_3d_request_start,
       ( int aToken, const char* aOp, const char* aLib, const char* aArg, const char* aKind ), {
    const op = UTF8ToString( aOp ), lib = UTF8ToString( aLib );
    const arg = UTF8ToString( aArg ), kind = UTF8ToString( aKind );
    const scheduler = globalThis.__wxScheduler;
    const finish = ( value ) => {
        if( !scheduler || typeof scheduler.runWaitCompletion !== 'function' )
            return false;

        // The model apply is a dependency of this exact waiter. Queueing it in
        // the physical FIFO can self-deadlock when YieldUntil used its in-place
        // fallback, because readiness stays closed until this token resolves.
        return scheduler.runWaitCompletion( '3D-model wait completion', aToken, () => {
            let res = value;
            if( res && res.__pcbjamPreparedModel3d === true
                && typeof res.apply === 'function' )
                res = res.apply();

            // Only the returned path is meaningful. Keep legacy providers which
            // used a non-string success marker compatible with the old bridge.
            res = res == null ? null : ( typeof res === 'string' ? res : '1' );
            let ptr = 0;
            if( res != null )
            {
                const len = lengthBytesUTF8( res ) + 1;
                ptr = _pcbjam_3d_alloc( len );

                if( ptr )
                    stringToUTF8( res, ptr, len );
            }
            return ptr;
        } );
    };

    const hook = globalThis.kicadLibs;

    if( !hook || !hook.request )
    {
        Promise.resolve().then( () => finish( null ) );
        return;
    }

    Promise.resolve()
        .then( () => hook.request( op, lib, arg, kind ) )
        .then( finish, ( e ) => {
            console.error( 'kicadLibs.request (model3d) failed:', e );
            return finish( null );
        } );
} );

// Token waits live in the wx wasm port (evtloop.cpp).
extern "C" int wxWasmBeginWait( const char* aKind );
extern "C" int wxWasmYieldUntil( int aToken );


struct PCBJAM_3D_REQ
{
    const char* op;
    const char* lib;
    const char* arg;
    const char* kind;
    char*       result;
};

// Worker-thread path, main-thread half: runs on the main thread via the proxying
// queue, kicks off the provider's async request, and releases the blocked worker
// (pcbjam_3d_finish) when the promise settles.  The request struct lives on the
// blocked worker's stack; shared wasm memory makes it readable here.
static void pcbjam_3d_request_on_main( em_proxying_ctx* aCtx, void* aArg )
{
    PCBJAM_3D_REQ* req = (PCBJAM_3D_REQ*) aArg;

    EM_ASM( {
        // EM_ASM is a variadic C macro. Avoid unparenthesized JavaScript
        // commas which the preprocessor can mistake for macro separators.
        const op = UTF8ToString( $0 );
        const lib = UTF8ToString( $1 );
        const arg = UTF8ToString( $2 );
        const kind = UTF8ToString( $3 );
        const hook = globalThis.kicadLibs;
        const resultPtr = $4;
        const ctx = $5;
        const enqueue = ( value ) => {
            const site = 'proxied 3D-model completion';
            const deliver = () => {
                let res = value;
                if( res && res.__pcbjamPreparedModel3d === true
                    && typeof res.apply === 'function' )
                    res = res.apply();
                res = res == null ? null : ( typeof res === 'string' ? res : '1' );

                let ptr = 0;
                if( res != null )
                {
                    const len = lengthBytesUTF8( res ) + 1;
                    ptr = _pcbjam_3d_alloc( len );

                    if( ptr )
                        stringToUTF8( res, ptr, len );
                }
                HEAPU32[resultPtr >> 2] = ptr;
                _pcbjam_3d_finish( ctx );
            };

            const scheduler = globalThis.__wxScheduler;
            if( scheduler )
            {
                if( typeof scheduler.enqueueNativeCompletion !== 'function' )
                    return false;
                let retainedBytes = value == null ? 0
                    : ( typeof value === 'string' ? value.length * 2 : 0 );
                if( value && value.__pcbjamPreparedModel3d === true
                    && typeof value.apply === 'function' )
                    retainedBytes = Number.isSafeInteger( value.retainedBytes )
                        && value.retainedBytes >= 0
                        ? value.retainedBytes : Number.MAX_SAFE_INTEGER;
                return scheduler.enqueueNativeCompletion( site, retainedBytes, deliver );
            }
            return globalThis.__wxNativeIntegrityUnknown ? false : ( deliver(), true );
        };
        const done = ( value ) => enqueue( value );

        if( !hook || !hook.request )
        {
            done( null );
            return;
        }

        Promise.resolve()
            .then( () => hook.request( op, lib, arg, kind ) )
            .then( done, ( e ) => {
                console.error( 'kicadLibs.request (model3d) failed:', e );
                return done( null );
            } );
    }, req->op, req->lib, req->arg, req->kind, &req->result, aCtx );
}

// Dispatch on the calling thread: workers proxy to the main thread and
// futex-block. Their fetches overlap and only short native completions serialize;
// main-thread calls use the scheduler wait.
static char* pcbjam_3d_request_dispatch( const char* aOp, const char* aLib, const char* aArg,
                                         const char* aKind )
{
    if( emscripten_is_main_runtime_thread() )
    {
        const int token = wxWasmBeginWait( "3d" );

        if( token <= 0 )
            return nullptr;

        pcbjam_3d_request_start( token, aOp, aLib, aArg, aKind );

        // The result is the malloc'd pointer, carried through the wait as an
        // int32; recover the full unsigned address before widening.
        const int r = wxWasmYieldUntil( token );
        return (char*) (uintptr_t) (uint32_t) r;
    }

    PCBJAM_3D_REQ req{ aOp, aLib, aArg, aKind, nullptr };

    em_proxying_queue* queue = emscripten_proxy_get_system_queue();

    if( !emscripten_proxy_sync_with_ctx( queue, emscripten_main_runtime_thread_id(),
                                         pcbjam_3d_request_on_main, &req ) )
        return nullptr;

    return req.result;
}

#endif // __EMSCRIPTEN__


// Normalize a footprint model reference to the provider's relative form:
// "${KICAD*_3DMODEL_DIR}/<lib>.3dshapes/<name>.<ext>" (any variable vintage,
// brace or paren syntax) → "<lib>.3dshapes/<name>.<ext>".  Bare relative refs
// pass through as-is.  Absolute paths, ${KIPRJMOD}-style project refs and
// kicad_embed:// URIs return empty — they are not served by the model libs.
wxString PCBJAM_3D::NormalizeModelRef( const wxString& aModelRef )
{
    wxString ref = aModelRef;
    ref.Trim( true ).Trim( false );

    if( ref.empty() )
        return wxEmptyString;

    if( ref.StartsWith( wxT( "${" ) ) || ref.StartsWith( wxT( "$(" ) ) )
    {
        wxChar closing = ref[1] == '{' ? '}' : ')';
        int    end = ref.Find( closing );

        if( end == wxNOT_FOUND )
            return wxEmptyString;

        const wxString var = ref.Mid( 2, end - 2 );

        // Any vintage of the model-dir var, plus the pre-v6 legacy alias.
        if( !var.Contains( wxT( "3DMODEL_DIR" ) ) && var != wxT( "KISYS3DMOD" ) )
            return wxEmptyString;

        ref = ref.Mid( end + 1 );

        while( !ref.empty() && ( ref[0] == '/' || ref[0] == '\\' ) )
            ref = ref.Mid( 1 );

        return ref;
    }

    if( ref.StartsWith( wxT( "/" ) ) || ref.Contains( wxT( "://" ) ) )
        return wxEmptyString;

    return ref;
}


wxString PCBJAM_3D::FindStagedModel( const wxString& aModelRef )
{
    const wxString rel = NormalizeModelRef( aModelRef );

    if( rel.empty() )
        return wxEmptyString;

    const wxString root = wxString::FromUTF8( MODELS_MEMFS_ROOT ) + wxT( "/" );

    // The exact ref, then the same-stem format fallbacks (mirror the JS
    // models-bridge FALLBACK_EXTS: the staged file's own extension picks the
    // parser, so a .wrl ref answered by its .step sibling loads through OCC).
    std::vector<wxString> candidates{ rel };

    const int dot = rel.Find( '.', /* fromEnd */ true );

    if( dot != wxNOT_FOUND )
    {
        const wxString ext = rel.Mid( dot ).Lower();
        const wxString stem = rel.Mid( 0, dot );

        if( ext == wxT( ".wrl" ) || ext == wxT( ".wrz" ) )
        {
            candidates.push_back( stem + wxT( ".step" ) );
            candidates.push_back( stem + wxT( ".stp" ) );
        }
        else if( ext == wxT( ".step" ) || ext == wxT( ".stp" ) )
        {
            candidates.push_back( stem + wxT( ".wrl" ) );
        }
    }

    for( const wxString& candidate : candidates )
    {
        const wxString path = root + candidate;

        if( wxFileName::FileExists( path ) )
            return path;
    }

    return wxEmptyString;
}


wxString PCBJAM_3D::EnsureModelFile( const wxString& aModelRef )
{
#ifdef __EMSCRIPTEN__
    const wxString rel = NormalizeModelRef( aModelRef );

    if( rel.empty() )
        return wxEmptyString;

    // Memoize both outcomes: a ref the provider can't serve must not re-cross
    // the bridge on every scene rebuild.
    static std::mutex                     memoMutex;
    static std::map<wxString, wxString>   resolved;

    {
        std::lock_guard<std::mutex> lock( memoMutex );
        auto it = resolved.find( rel );

        if( it != resolved.end() )
            return it->second;
    }

    wxString path;
    char* res = pcbjam_3d_request_dispatch( "ensure", "", rel.utf8_str().data(), "model3d" );

    if( res )
    {
        // The provider answers with the absolute MEMFS path it wrote ("1" was
        // the pre-path ack; treat it as "not a path" for compatibility).
        path = wxString::FromUTF8( res );
        free( res );

        if( !path.StartsWith( wxT( "/" ) ) )
            path.clear();
    }

    {
        std::lock_guard<std::mutex> lock( memoMutex );
        resolved[rel] = path;
    }

    // Once per ref (memoized above) — the only field-visible signal of the
    // lazy-delivery outcome.
    std::printf( "[pcbjam-3d] ensure %s -> %s\n", (const char*) rel.utf8_str(),
                 path.empty() ? "(unserved)" : (const char*) path.utf8_str() );

    return path;
#else
    (void) aModelRef;
    return wxEmptyString;
#endif
}
