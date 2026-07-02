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

// Main-thread path: suspends the calling C++ stack via Asyncify until the
// provider's promise resolves.  The provider fetches the model body (IDB, then
// R2) and writes it into the MEMFS model root itself (FS.writeFile) — the
// response is just an ack string, so no binary framing crosses the bridge.
EM_ASYNC_JS( char*, pcbjam_3d_request_js,
             ( const char* aOp, const char* aLib, const char* aArg, const char* aKind ), {
    const hook = globalThis.kicadLibs;

    if( !hook || !hook.request )
        return 0;

    try
    {
        const res = await hook.request( UTF8ToString( aOp ), UTF8ToString( aLib ),
                                        UTF8ToString( aArg ), UTF8ToString( aKind ) );

        if( res == null )
            return 0;

        const str = typeof res === 'string' ? res : '1';
        const len = lengthBytesUTF8( str ) + 1;
        const ptr = _pcbjam_3d_alloc( len );
        stringToUTF8( str, ptr, len );
        return ptr;
    }
    catch( e )
    {
        console.error( 'kicadLibs.request (model3d) failed:', e );
        return 0;
    }
} );


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
        const hook = globalThis.kicadLibs;
        const resultPtr = $4;
        const ctx = $5;

        const done = ( ptr ) => {
            HEAPU32[resultPtr >> 2] = ptr;
            _pcbjam_3d_finish( ctx );
        };

        if( !hook || !hook.request )
        {
            done( 0 );
            return;
        }

        hook.request( UTF8ToString( $0 ), UTF8ToString( $1 ), UTF8ToString( $2 ),
                      UTF8ToString( $3 ) )
            .then( ( res ) => {
                if( res == null )
                {
                    done( 0 );
                    return;
                }

                const str = typeof res === 'string' ? res : '1';
                const len = lengthBytesUTF8( str ) + 1;
                const ptr = _pcbjam_3d_alloc( len );
                stringToUTF8( str, ptr, len );
                done( ptr );
            } )
            .catch( ( e ) => {
                console.error( 'kicadLibs.request (model3d) failed:', e );
                done( 0 );
            } );
    }, req->op, req->lib, req->arg, req->kind, &req->result, aCtx );
}


// Serialize worker-thread proxied requests — concurrent C reentry into the
// Asyncify-suspended runtime corrupts its state (same reasoning as the symbol
// and footprint bridges; a distinct lock for this bridge).
static std::mutex g_pcbjam3dProxyMutex;

// Dispatch on the calling thread: workers proxy to the main thread and
// futex-block; main-thread calls use the Asyncify suspension.
static char* pcbjam_3d_request_dispatch( const char* aOp, const char* aLib, const char* aArg,
                                         const char* aKind )
{
    if( emscripten_is_main_runtime_thread() )
        return pcbjam_3d_request_js( aOp, aLib, aArg, aKind );

    PCBJAM_3D_REQ req{ aOp, aLib, aArg, aKind, nullptr };

    em_proxying_queue* queue = emscripten_proxy_get_system_queue();

    std::lock_guard<std::mutex> serialize( g_pcbjam3dProxyMutex );

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
static wxString pcbjamNormalizeModelRef( const wxString& aModelRef )
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


wxString PCBJAM_3D::EnsureModelFile( const wxString& aModelRef )
{
#ifdef __EMSCRIPTEN__
    const wxString rel = pcbjamNormalizeModelRef( aModelRef );

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
