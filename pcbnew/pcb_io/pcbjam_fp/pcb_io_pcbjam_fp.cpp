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

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>

#include <nlohmann/json.hpp>

#include <ki_exception.h>
#include <footprint.h>
#include <thread_pool.h>
#include <trace_helpers.h>
#include <wx/log.h>

#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <pcb_io/pcbjam_fp/pcb_io_pcbjam_fp.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

// Unique export names (pcbjam_fp_*) so this footprint bridge never collides with
// the symbol bridge's pcbjam_libs_* exports if both libraries ever land in one
// binary (eeschema vs pcbnew are separate today, but this keeps it safe).
extern "C" EMSCRIPTEN_KEEPALIVE char* pcbjam_fp_libs_alloc( int aBytes )
{
    return (char*) malloc( aBytes );
}

extern "C" EMSCRIPTEN_KEEPALIVE void pcbjam_fp_libs_finish( em_proxying_ctx* aCtx )
{
    emscripten_proxy_finish( aCtx );
}

// Main-thread path: suspends the calling C++ stack via Asyncify until the
// provider's promise resolves.  Returns a malloc'd UTF-8 string (caller frees)
// or 0 on failure.  globalThis here is the window, where the standalone installs
// the provider.  The 4th arg carries the item kind ("footprint").
EM_ASYNC_JS( char*, pcbjam_fp_libs_request_js,
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

        // "Copy as-is": the fat list (list/bodies) returns the raw item bytes as a
        // Uint8Array — memcpy straight into the wasm heap, no string/JSON detour.
        // s-expr never contains a NUL byte, so NUL-terminate and the C++ side reads
        // it as a std::string exactly like the string ops (get/save).
        if( res instanceof Uint8Array )
        {
            const p = _pcbjam_fp_libs_alloc( res.length + 1 );
            HEAPU8.set( res, p );
            HEAPU8[p + res.length] = 0;
            return p;
        }

        const len = lengthBytesUTF8( res ) + 1;
        const ptr = _pcbjam_fp_libs_alloc( len );
        stringToUTF8( res, ptr, len );
        return ptr;
    }
    catch( e )
    {
        console.error( 'kicadLibs.request (footprint) failed:', e );
        return 0;
    }
} );


struct PCBJAM_FP_LIBS_REQ
{
    const char* op;
    const char* lib;
    const char* arg;
    const char* kind;
    char*       result;
};

// Worker-thread path, main-thread half: runs on the main thread via the proxying
// queue, kicks off the provider's async request, and releases the blocked worker
// (pcbjam_fp_libs_finish) when the promise settles.  The request struct lives on
// the blocked worker's stack; shared wasm memory makes it readable here.
static void pcbjam_fp_libs_request_on_main( em_proxying_ctx* aCtx, void* aArg )
{
    PCBJAM_FP_LIBS_REQ* req = (PCBJAM_FP_LIBS_REQ*) aArg;

    EM_ASM( {
        const hook = globalThis.kicadLibs;
        const resultPtr = $4;
        const ctx = $5;

        const done = ( ptr ) => {
            HEAPU32[resultPtr >> 2] = ptr;
            _pcbjam_fp_libs_finish( ctx );
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

                // "Copy as-is": raw item bytes (fat list) → memcpy; NUL-terminate
                // so the C++ side reads a std::string like the string ops.
                if( res instanceof Uint8Array )
                {
                    const p = _pcbjam_fp_libs_alloc( res.length + 1 );
                    HEAPU8.set( res, p );
                    HEAPU8[p + res.length] = 0;
                    done( p );
                    return;
                }

                const len = lengthBytesUTF8( res ) + 1;
                const ptr = _pcbjam_fp_libs_alloc( len );
                stringToUTF8( res, ptr, len );
                done( ptr );
            } )
            .catch( ( e ) => {
                console.error( 'kicadLibs.request (footprint) failed:', e );
                done( 0 );
            } );
    }, req->op, req->lib, req->arg, req->kind, &req->result, aCtx );
}


// Serialize worker-thread proxied requests. The footprint chooser enumerates
// every library concurrently (one thread-pool task per lib), so multiple pthreads
// would proxy into the main thread at once. Concurrent C reentry into the
// Asyncify-suspended runtime corrupts its state ("table index out of bounds").
// A global lock held across the whole proxy+fetch round-trip serializes them; it
// parks extra worker threads (not the main thread), so the UI stays live. (Same
// reasoning as the symbol bridge; this is a distinct lock for the pcbnew binary.)
static std::mutex g_pcbjamFpProxyMutex;

// Dispatch on the calling thread.  Library loads come in on KiCad thread-pool
// pthreads; there we proxy to the main thread and futex-block until the fetch
// settles — legal on a worker.  Calls already on the main thread use the Asyncify
// suspension instead (blocking the main thread is not an option, and proxy-to-
// self would deadlock).
static char* pcbjam_fp_libs_request_dispatch( const char* aOp, const char* aLib, const char* aArg,
                                              const char* aKind )
{
    if( emscripten_is_main_runtime_thread() )
        return pcbjam_fp_libs_request_js( aOp, aLib, aArg, aKind );

    PCBJAM_FP_LIBS_REQ req{ aOp, aLib, aArg, aKind, nullptr };

    em_proxying_queue* queue = emscripten_proxy_get_system_queue();

    std::lock_guard<std::mutex> serialize( g_pcbjamFpProxyMutex );

    if( !emscripten_proxy_sync_with_ctx( queue, emscripten_main_runtime_thread_id(),
                                         pcbjam_fp_libs_request_on_main, &req ) )
        return nullptr;

    return req.result;
}
#endif


PCB_IO_PCBJAM_FP::PCB_IO_PCBJAM_FP() :
        PCB_IO( wxS( "pcbjam footprint library" ) )
{
}


PCB_IO_PCBJAM_FP::~PCB_IO_PCBJAM_FP()
{
    for( auto& [key, footprint] : m_cache )
        delete footprint;
}


bool PCB_IO_PCBJAM_FP::CanReadLibrary( const wxString& aFileName ) const
{
    // Single mount root for every pcbjam lib.  PCB_IO_MGR::GuessPluginTypeFromLibPath
    // probes this too (used as a legacy-format gate on the save path), so the type
    // is recognised from the URI without touching pcb_io_mgr's selection logic.
    return aFileName.StartsWith( wxS( "/mnt/pcbjam/" ) );
}


std::optional<std::string> PCB_IO_PCBJAM_FP::requestOpt( const std::string& aOp,
                                                         const wxString& aLibraryPath,
                                                         const wxString& aArg )
{
#ifdef __EMSCRIPTEN__
    char* res = pcbjam_fp_libs_request_dispatch( aOp.c_str(), aLibraryPath.utf8_str().data(),
                                                 aArg.utf8_str().data(), "footprint" );

    if( !res )
        return std::nullopt;

    std::string out( res );
    free( res );
    return out;
#else
    return std::nullopt;
#endif
}


std::string PCB_IO_PCBJAM_FP::request( const std::string& aOp, const wxString& aLibraryPath,
                                       const wxString& aArg )
{
    if( std::optional<std::string> out = requestOpt( aOp, aLibraryPath, aArg ) )
        return *out;

    m_lastError = wxString::Format( _( "pcbjam footprint library provider failed: %s %s" ),
                                    wxString( aOp ), aLibraryPath );
    THROW_IO_ERROR( m_lastError );
}


void PCB_IO_PCBJAM_FP::FootprintEnumerate( wxArrayString& aFootprintNames,
                                           const wxString& aLibraryPath, bool aBestEfforts,
                                           const std::map<std::string, UTF8>* aProperties )
{
    // Fat-load the whole library in ONE crossing on first enumerate and cache
    // every body, so the adapter's subsequent per-name FootprintLoad calls all
    // hit the cache (the per-item N gets used to live in the adapter loop, not in
    // this plugin). Repeat enumerates answer from m_libNames with no crossing
    // (docs/features/libs/0011-fast-lib-load).
    if( !m_loadedLibs.count( aLibraryPath ) )
    {
        try
        {
            fatLoad( aLibraryPath );
        }
        catch( const IO_ERROR& )
        {
            if( aBestEfforts )
                return;

            throw;
        }
        catch( const nlohmann::json::exception& e )
        {
            if( aBestEfforts )
                return;

            m_lastError = wxString::Format( _( "pcbjam footprint list for '%s' is invalid: %s" ),
                                            aLibraryPath, wxString( e.what() ) );
            THROW_IO_ERROR( m_lastError );
        }
    }

    if( auto it = m_libNames.find( aLibraryPath ); it != m_libNames.end() )
    {
        for( const wxString& name : it->second )
            aFootprintNames.Add( name );
    }
}


// File-local: parse one (footprint …) document into a fresh master. PURE — it
// touches no member or global state and makes no provider/bridge call — so it is
// safe to run on a thread-pool worker (the parallel-parse path in fatLoad). Each
// worker builds its own wxString from the UTF-8 body (NOT a shared wxString:
// wxString's copy-on-write refcount is not thread-safe), and constructs its own
// PCB_IO_KICAD_SEXPR. A malformed body yields nullptr — one bad footprint must not
// abort the library.
static FOOTPRINT* parseFpDoc( const std::string& aBody )
{
    try
    {
        PCB_IO_KICAD_SEXPR pi;
        BOARD_ITEM*        item = pi.Parse( wxString::FromUTF8( aBody.c_str(), aBody.size() ) );

        if( FOOTPRINT* footprint = dynamic_cast<FOOTPRINT*>( item ) )
            return footprint;

        delete item;
    }
    catch( ... )
    {
        // Runs on a worker — no exception may escape into the pool future (it would
        // be rethrown by wait() on the app thread and bypass the aBestEfforts /
        // IO_ERROR handling in FootprintEnumerate). One bad body is simply dropped.
    }

    return nullptr;
}


void PCB_IO_PCBJAM_FP::mergeFpDoc( const wxString& aLibraryPath, const wxString& aName,
                                   FOOTPRINT* aFootprint )
{
    if( !aFootprint )
        return;

    wxString cacheKey = aLibraryPath + wxS( "|" ) + aName;

    // Cache-if-absent: a footprint already loaded by an earlier "get" keeps its
    // master (callers hold clones, not the master, but staying consistent with the
    // symbol plugin and avoiding redundant parses); the duplicate is freed.
    if( m_cache.find( cacheKey ) != m_cache.end() )
    {
        delete aFootprint;
        return;
    }

    m_cache[cacheKey] = aFootprint;
}


void PCB_IO_PCBJAM_FP::cacheFootprint( const wxString& aLibraryPath, const wxString& aName,
                                       const std::string& aBody )
{
    // Single-get path (loadOne): parse on the calling thread, then cache-if-absent.
    mergeFpDoc( aLibraryPath, aName, parseFpDoc( aBody ) );
}


void PCB_IO_PCBJAM_FP::fatLoad( const wxString& aLibraryPath )
{
    using clock = std::chrono::steady_clock;
    auto msSince = []( clock::time_point a, clock::time_point b )
                   { return std::chrono::duration<double, std::milli>( b - a ).count(); };

    // One crossing for the whole library: "list" with arg "bodies" returns every
    // footprint's body. request() throws if the provider yields null, so a
    // transient failure leaves the lib un-flagged and retries on the next
    // enumerate; an empty "footprints" array is a legitimately empty library.
    clock::time_point t0 = clock::now();
    std::string       body = request( "list", aLibraryPath, wxS( "bodies" ) );
    clock::time_point tFetch = clock::now();

    // Framed fat-list payload ("copy as-is", mirrors the symbol plugin): a one-line
    // JSON header
    //   {"footprints":[{"name":..,"len":<utf8 byte length>}, ...]}
    // then a single '\n', then every body's raw s-expr bytes concatenated with NO
    // JSON escaping (the provider memcpy's them across the bridge). So we parse only
    // the small header and slice the body region by byte length — none of the s-expr
    // gets un-escaped. The header JSON is single-line, so the first '\n' terminates it.
    std::vector<size_t>   offs;
    std::vector<size_t>   lens;
    std::vector<wxString> names;

    size_t nl = body.find( '\n' );

    if( nl == std::string::npos )
    {
        m_lastError = wxString::Format( _( "pcbjam fat list for '%s' is malformed (no header)" ),
                                        aLibraryPath );
        THROW_IO_ERROR( m_lastError );
    }

    // nlohmann::json::exception propagates to FootprintEnumerate (aBestEfforts).
    nlohmann::json header = nlohmann::json::parse( body.substr( 0, nl ) );
    const auto&    arr = header.at( "footprints" );

    offs.reserve( arr.size() );
    lens.reserve( arr.size() );
    names.reserve( arr.size() );

    size_t off = nl + 1;

    for( const auto& item : arr )
    {
        size_t len = item.at( "len" ).get<size_t>();
        offs.push_back( off );
        lens.push_back( len );
        off += len;
        names.emplace_back( wxString::FromUTF8( item.at( "name" ).get<std::string>() ) );
    }

    clock::time_point tHeader = clock::now();

    // Parallel s-expr parse: each worker slices its bodies out of `body` (read-only
    // shared, so concurrent reads are safe) and parses each to a LOCAL master.
    // parseFpDoc is pure (no member/global state, no bridge call), so this is safe
    // off the app thread; the wait() yields via the build's main-thread
    // nanosleep->yield shim, mirroring the 3D raytracer's submit_blocks/wait and the
    // symbol plugin's fatLoad. Fetch (Asyncify, above) and the cache merge (below)
    // stay on the calling app thread.
    const size_t            count = offs.size();
    std::vector<FOOTPRINT*> parsed( count, nullptr );

    if( count > 0 )
    {
        thread_pool& tp = GetKiCadThreadPool();

        tp.submit_blocks( size_t( 0 ), count,
                [&body, &parsed, &offs, &lens]( size_t start, size_t end )
                {
                    for( size_t i = start; i < end; ++i )
                    {
                        size_t lo = std::min( offs[i], body.size() );
                        size_t hi = std::min( offs[i] + lens[i], body.size() );
                        parsed[i] = parseFpDoc( std::string( body.data() + lo, hi - lo ) );
                    }
                } )
                .wait();
    }

    clock::time_point tParse = clock::now();

    // Merge every parsed master into m_cache on the app thread (cache-if-absent).
    for( size_t i = 0; i < count; ++i )
        mergeFpDoc( aLibraryPath, names[i], parsed[i] );

    // Cold-path breakdown (KICAD_TRACE=KI_TRACE_FP_CHOOSER): fetch crossing vs the
    // (tiny) header parse vs the parallel s-expr parse wall-time vs the merge.
    // fatLoad runs once per lib (cached after), so this is cold-only.
    KI_TRACE( wxT( "KI_TRACE_FP_CHOOSER" ),
              wxT( "fatLoad lib=%s footprints=%zu bytes=%zu fetch_ms=%.1f header_ms=%.1f parse_ms=%.1f merge_ms=%.1f total_ms=%.1f\n" ),
              aLibraryPath, count, body.size(), msSince( t0, tFetch ), msSince( tFetch, tHeader ),
              msSince( tHeader, tParse ), msSince( tParse, clock::now() ), msSince( t0, clock::now() ) );

    m_libNames[aLibraryPath] = std::move( names );
    m_loadedLibs.insert( aLibraryPath );
}


FOOTPRINT* PCB_IO_PCBJAM_FP::FootprintLoad( const wxString& aLibraryPath,
                                            const wxString& aFootprintName, bool aKeepUUID,
                                            const std::map<std::string, UTF8>* aProperties )
{
    if( FOOTPRINT* master = loadOne( aLibraryPath, aFootprintName ) )
        return static_cast<FOOTPRINT*>( master->Clone() );

    return nullptr;
}


FOOTPRINT* PCB_IO_PCBJAM_FP::loadOne( const wxString& aLibraryPath, const wxString& aName )
{
    wxString cacheKey = aLibraryPath + wxS( "|" ) + aName;

    if( auto it = m_cache.find( cacheKey ); it != m_cache.end() )
        return it->second;

    // A missing footprint comes back as nullopt (not an exception): the editor's
    // CreateNewFootprint probes FootprintExists (-> FootprintLoad) to find a unique
    // name, and must see a clean "not found" rather than a thrown IO_ERROR.
    std::optional<std::string> got = requestOpt( "get", aLibraryPath, aName );

    if( !got )
        return nullptr;

    // Parse + cache the returned document; shared with the fat-load path.
    cacheFootprint( aLibraryPath, aName, *got );

    if( auto it = m_cache.find( cacheKey ); it != m_cache.end() )
        return it->second;

    m_lastError = wxString::Format( _( "Footprint '%s' not found in pcbjam library '%s'" ),
                                    aName, aLibraryPath );
    wxLogTrace( wxS( "PCBJAM_FP" ), m_lastError );
    return nullptr;
}


void PCB_IO_PCBJAM_FP::FootprintSave( const wxString& aLibraryPath, const FOOTPRINT* aFootprint,
                                      const std::map<std::string, UTF8>* aProperties )
{
    wxCHECK_RET( aFootprint, wxS( "null footprint passed to PCB_IO_PCBJAM_FP::FootprintSave" ) );

    // Serialize the single footprint at the fork's native board/footprint version
    // using the library control flags (omit nets/uuids/path/at, bare lib item name).
    // The emitted (footprint … (version N) (generator "pcbnew") …) is exactly what
    // the on-device parser reads back, so user-saved bodies round-trip with no shim.
    std::string body;

    {
        PCB_IO_KICAD_SEXPR pi( CTL_FOR_LIBRARY );
        pi.Format( static_cast<const BOARD_ITEM*>( aFootprint ) );
        body = pi.GetStringOutput( false );
    }

    nlohmann::json payload;
    payload["name"] = std::string( aFootprint->GetFPID().GetLibItemName().c_str() );
    payload["body"] = body;
    std::string payloadStr = payload.dump();

    // Throws IO_ERROR if the provider rejects the write (the library adapter
    // catches it and reports the save as failed).
    request( "save", aLibraryPath,
             wxString::FromUTF8( payloadStr.c_str(), payloadStr.size() ) );

    // Drop any stale cached master so the next load reflects the saved body.
    wxString key = aLibraryPath + wxS( "|" ) + aFootprint->GetFPID().GetLibItemName().wx_str();

    if( auto it = m_cache.find( key ); it != m_cache.end() )
    {
        delete it->second;
        m_cache.erase( it );
    }

    // Invalidate the fat-load guard so the next enumerate refetches this lib —
    // picking up a newly-created footprint, the edited body, or a removal.
    m_loadedLibs.erase( aLibraryPath );
    m_libNames.erase( aLibraryPath );
}
