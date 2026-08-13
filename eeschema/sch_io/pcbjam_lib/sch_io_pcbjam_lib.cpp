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
#include <optional>

#include <nlohmann/json.hpp>

#include <ki_exception.h>
#include <lib_symbol.h>
#include <richio.h>
#include <thread_pool.h>
#include <trace_helpers.h>
#include <wx/log.h>

#include <sch_file_versions.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_lib_cache.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_parser.h>
#include <sch_io/pcbjam_lib/sch_io_pcbjam_lib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

extern "C" EMSCRIPTEN_KEEPALIVE char* pcbjam_libs_alloc( int aBytes )
{
    return (char*) malloc( aBytes );
}

extern "C" EMSCRIPTEN_KEEPALIVE void pcbjam_libs_finish( em_proxying_ctx* aCtx )
{
    emscripten_proxy_finish( aCtx );
}

// Main-thread path, Phase E shape (docs/features/async/22 §5): the request no
// longer Asyncify-parks the stack it stands on. It opens a wait token, starts
// the JS request, and waits via wxWasmYieldUntil — which parks the owning
// scheduler context when the frame stands on one, and falls back to the
// in-place park (the pre-Phase-E behaviour) when it does not.
//
// Every resolution is deferred to at least a microtask, NEVER delivered
// synchronously from this call: the C++ caller has not parked yet, and a
// synchronous resolveWait would spend the wake before the park exists (the
// exact strand that killed Phase E attempt 1 on pages with no provider).
// wxWasmYieldUntil's early-resolve peek is the second half of that contract.
EM_JS( void, pcbjam_libs_request_start,
       ( int aToken, const char* aOp, const char* aLib, const char* aArg, const char* aKind ), {
    const op = UTF8ToString( aOp ), lib = UTF8ToString( aLib );
    const arg = UTF8ToString( aArg ), kind = UTF8ToString( aKind );
    const scheduler = globalThis.__wxScheduler;
    const normalize = ( value ) => {
        if( value == null || typeof value === 'string' || value instanceof Uint8Array )
            return value;
        try { return String( value ); }
        catch( e ) {
            console.error( 'kicadLibs.request returned an invalid result:', e );
            return null;
        }
    };
    const finish = ( value ) => {
        const res = normalize( value );

        if( !scheduler || typeof scheduler.runWaitCompletion !== 'function' )
            return false;

        // This completion owns the exact token whose caller is parked. Putting
        // it in the physical-entry FIFO can self-deadlock when YieldUntil used
        // its in-place fallback: readiness stays closed until this very token
        // resolves. Prepare the caller's result and publish it as one exact edge.
        return scheduler.runWaitCompletion(
            'symbol-library wait completion', aToken, () => {
                let ptr = 0;
                if( res instanceof Uint8Array )
                {
                    ptr = _pcbjam_libs_alloc( res.length + 1 );

                    if( ptr )
                    {
                        HEAPU8.set( res, ptr );
                        HEAPU8[ptr + res.length] = 0;
                    }
                }
                else if( res != null )
                {
                    const len = lengthBytesUTF8( res ) + 1;
                    ptr = _pcbjam_libs_alloc( len );

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
        // This is pure JavaScript/service work. It deliberately stays outside
        // native admission so independent fetches can overlap.
        .then( () => hook.request( op, lib, arg, kind ) )
        .then( finish, ( e ) => {
            console.error( 'kicadLibs.request failed:', e );
            return finish( null );
        } );
} );

// Token waits live in the wx wasm port (evtloop.cpp); this is the first
// KiCad-side client of that registry. WEAK: the sym_convert diet compiles this
// TU without the wx wasm port — there the registry is absent, and so is any
// /mnt/pcbjam provider, so a null resolves to "request unavailable" (nullopt).
extern "C" int wxWasmBeginWait( const char* aKind ) __attribute__(( weak ));
extern "C" int wxWasmYieldUntil( int aToken ) __attribute__(( weak ));


struct PCBJAM_LIBS_REQ
{
    const char* op;
    const char* lib;
    const char* arg;
    const char* kind;
    char*       result;
};

// Worker-thread path, main-thread half: runs on the main thread via the
// proxying queue, kicks off the provider's async request, and releases the
// blocked worker (emscripten_proxy_finish) when the promise settles.  The
// request struct lives on the blocked worker's stack; shared wasm memory
// makes it readable/writable here.
static void pcbjam_libs_request_on_main( em_proxying_ctx* aCtx, void* aArg )
{
    PCBJAM_LIBS_REQ* req = (PCBJAM_LIBS_REQ*) aArg;

    EM_ASM( {
        // Copy the worker-owned strings while this leaf proxy callback is on
        // the stack. The callback then returns immediately; only its proxying
        // context and result slot remain retained until completion.
        // The C preprocessor parses EM_ASM as a variadic macro. Keep each
        // declaration separate so an unparenthesized JavaScript comma cannot
        // split the macro arguments.
        const op = UTF8ToString( $0 );
        const lib = UTF8ToString( $1 );
        const arg = UTF8ToString( $2 );
        const kind = UTF8ToString( $3 );
        const hook = globalThis.kicadLibs;
        const resultPtr = $4;
        const ctx = $5;
        const normalize = ( value ) => {
            if( value == null || typeof value === 'string' || value instanceof Uint8Array )
                return value;
            try { return String( value ); }
            catch( e ) {
                console.error( 'kicadLibs.request returned an invalid result:', e );
                return null;
            }
        };
        const enqueue = ( res ) => {
            const site = 'proxied symbol-library completion';
            const deliver = () => {
                let ptr = 0;
                if( res instanceof Uint8Array )
                {
                    ptr = _pcbjam_libs_alloc( res.length + 1 );

                    if( ptr )
                    {
                        HEAPU8.set( res, ptr );
                        HEAPU8[ptr + res.length] = 0;
                    }
                }
                else if( res != null )
                {
                    const len = lengthBytesUTF8( res ) + 1;
                    ptr = _pcbjam_libs_alloc( len );

                    if( ptr )
                        stringToUTF8( res, ptr, len );
                }
                HEAPU32[resultPtr >> 2] = ptr;
                _pcbjam_libs_finish( ctx );
            };

            const scheduler = globalThis.__wxScheduler;
            if( scheduler )
            {
                if( typeof scheduler.enqueueNativeCompletion !== 'function' )
                    return false;
                const retainedBytes = res instanceof Uint8Array
                    ? res.buffer.byteLength : ( typeof res === 'string' ? res.length * 2 : 0 );
                return scheduler.enqueueNativeCompletion( site, retainedBytes, deliver );
            }

            // Scheduler-less fallback is for the converter diet.
            return globalThis.__wxNativeIntegrityUnknown ? false : ( deliver(), true );
        };
        const done = ( value ) => enqueue( normalize( value ) );

        if( !hook || !hook.request )
        {
            done( null );
            return;
        }

        Promise.resolve()
            .then( () => hook.request( op, lib, arg, kind ) )
            .then( done, ( e ) => {
                console.error( 'kicadLibs.request failed:', e );
                return done( null );
            } );
    }, req->op, req->lib, req->arg, req->kind, &req->result, aCtx );
}

// Dispatch on the calling thread.  Library loads come in on KiCad thread-pool
// pthreads. Each worker independently proxies a leaf request starter to the
// main thread and futex-blocks until its own fetch settles. Fetches overlap;
// their short heap-copy/finish steps pass through the native-entry arbiter.
// Calls already on the main thread use the scheduler wait instead (blocking
// the main thread is not an option, and proxy-to-self would deadlock).
static char* pcbjam_libs_request_dispatch( const char* aOp, const char* aLib, const char* aArg,
                                           const char* aKind )
{
    if( emscripten_is_main_runtime_thread() )
    {
        if( !wxWasmBeginWait || !wxWasmYieldUntil )
            return nullptr;   // converter diet: no wait registry, no provider

        const int token = wxWasmBeginWait( "lib" );

        if( token <= 0 )
            return nullptr;

        pcbjam_libs_request_start( token, aOp, aLib, aArg, aKind );

        // The result is the malloc'd pointer, carried through the wait as an
        // int32; recover the full unsigned address before widening.
        const int r = wxWasmYieldUntil( token );
        return (char*) (uintptr_t) (uint32_t) r;
    }

    PCBJAM_LIBS_REQ req{ aOp, aLib, aArg, aKind, nullptr };

    em_proxying_queue* queue = emscripten_proxy_get_system_queue();

    if( !emscripten_proxy_sync_with_ctx( queue, emscripten_main_runtime_thread_id(),
                                         pcbjam_libs_request_on_main, &req ) )
        return nullptr;

    return req.result;
}
#endif


SCH_IO_PCBJAM_LIB::SCH_IO_PCBJAM_LIB() :
        SCH_IO( wxS( "pcbjam library" ) )
{
}


SCH_IO_PCBJAM_LIB::~SCH_IO_PCBJAM_LIB()
{
    for( auto& [key, symbol] : m_cache )
        delete symbol;
}


bool SCH_IO_PCBJAM_LIB::CanReadLibrary( const wxString& aFileName ) const
{
    // Single mount root for every pcbjam lib.  This is also how SCH_IO_MGR::
    // GuessPluginTypeFromLibPath routes a save to this plugin (it probes each
    // plugin's CanReadLibrary), so the type is selected from the URI without
    // touching sch_io_mgr.
    return aFileName.StartsWith( wxS( "/mnt/pcbjam/" ) );
}


std::optional<std::string> SCH_IO_PCBJAM_LIB::requestOpt( const std::string& aOp,
                                                          const wxString& aLibraryPath,
                                                          const wxString& aArg )
{
#ifdef __EMSCRIPTEN__
    // The bridge carries the item kind as a 4th arg (the JS provider defaults it
    // to "symbol", so this is also forward-compatible); this plugin is symbols.
    char* res = pcbjam_libs_request_dispatch( aOp.c_str(), aLibraryPath.utf8_str().data(),
                                              aArg.utf8_str().data(), "symbol" );

    if( !res )
        return std::nullopt;

    std::string out( res );
    free( res );
    return out;
#else
    return std::nullopt;
#endif
}


std::string SCH_IO_PCBJAM_LIB::request( const std::string& aOp, const wxString& aLibraryPath,
                                        const wxString& aArg )
{
    if( std::optional<std::string> out = requestOpt( aOp, aLibraryPath, aArg ) )
        return *out;

    m_lastError = wxString::Format( _( "pcbjam library provider failed: %s %s" ),
                                    wxString( aOp ), aLibraryPath );
    THROW_IO_ERROR( m_lastError );
}


void SCH_IO_PCBJAM_LIB::EnumerateSymbolLib( wxArrayString& aSymbolNameList,
                                            const wxString& aLibraryPath,
                                            const std::map<std::string, UTF8>* aProperties )
{
    std::string body = request( "list", aLibraryPath, wxEmptyString );

    try
    {
        nlohmann::json js = nlohmann::json::parse( body );

        for( const auto& name : js.at( "symbols" ) )
            aSymbolNameList.Add( wxString::FromUTF8( name.get<std::string>() ) );
    }
    catch( const nlohmann::json::exception& e )
    {
        m_lastError = wxString::Format( _( "pcbjam library list for '%s' is invalid: %s" ),
                                        aLibraryPath, wxString( e.what() ) );
        THROW_IO_ERROR( m_lastError );
    }
}


void SCH_IO_PCBJAM_LIB::EnumerateSymbolLib( std::vector<LIB_SYMBOL*>& aSymbolList,
                                            const wxString& aLibraryPath,
                                            const std::map<std::string, UTF8>* aProperties )
{
    // Fat-load the whole library in ONE provider crossing on first enumerate;
    // repeat enumerates (tree Sync refreshes) rebuild from m_cache with no
    // crossing.  This collapses the old "1 list + N get" into a single "list",
    // while preserving the cache's "already loaded => 0 fetches" property
    // (docs/features/libs/0011-fast-lib-load).
    if( !m_loadedLibs.count( aLibraryPath ) )
        fatLoad( aLibraryPath );

    auto it = m_libNames.find( aLibraryPath );

    if( it == m_libNames.end() )
        return;

    // Hand back the cache-owned masters directly (NON-owning, like upstream
    // SCH_IO_KICAD_SEXPR::EnumerateSymbolLib and the footprint adapter's
    // GetFootprints): the LIB_TREE_NODE_ITEM consumer copies out only the
    // metadata it needs and never retains the pointer, so the previous
    // `new LIB_SYMBOL( *master )` per item deep-cloned every symbol's geometry
    // on EVERY chooser open (~20k clones for the full set, the warm-path
    // 5-8s) and then leaked them (the contract is non-owning). m_cache owns
    // these and frees them in the dtor; SaveSymbol invalidates per-lib.
    for( const wxString& name : it->second )
    {
        if( LIB_SYMBOL* master = loadOne( aLibraryPath, name ) )
            aSymbolList.emplace_back( master );
    }
}


// File-local: parse one kicad_symbol_lib document into a fresh symbol map. PURE —
// it touches no member or global state and makes no provider/bridge call — so it
// is safe to run on a thread-pool worker (the parallel-parse path in fatLoad). The
// lib path arrives as UTF-8 std::string (NOT a shared wxString: wxString's
// copy-on-write refcount is not thread-safe, so each worker builds its own). A
// malformed body yields an empty map — one bad symbol must not abort the library.
static LIB_SYMBOL_MAP parseLibDoc( const std::string& aBody, const std::string& aLibPathUtf8 )
{
    LIB_SYMBOL_MAP map;

    try
    {
        wxString                  source = wxString::FromUTF8( aLibPathUtf8 );
        STRING_LINE_READER        reader( aBody, source );
        SCH_IO_KICAD_SEXPR_PARSER parser( &reader );
        parser.ParseLib( map );
    }
    catch( ... )
    {
        // Runs on a worker — no exception may escape into the pool future (it
        // would be rethrown by wait() on the app thread and bypass GetSymbols's
        // IO_ERROR handler). One bad body is simply dropped.
        for( auto& [name, symbol] : map )
            delete symbol;

        map.clear();
    }

    return map;
}


void SCH_IO_PCBJAM_LIB::mergeLibDoc( const wxString& aLibraryPath, LIB_SYMBOL_MAP& aMap,
                                     std::vector<LIB_SYMBOL*>& aFresh )
{
    // Cache-if-absent (not replace): a parent re-seen in a later body keeps the
    // already-cached master, so pointers held by earlier derived symbols never
    // dangle; the parsed duplicate is freed.
    for( auto& [name, symbol] : aMap )
    {
        wxString key = aLibraryPath + wxS( "|" ) + name;

        if( m_cache.find( key ) != m_cache.end() )
        {
            delete symbol;
            continue;
        }

        m_cache[key] = symbol;
        aFresh.push_back( symbol );
    }
}


void SCH_IO_PCBJAM_LIB::linkExtends( const wxString&                 aLibraryPath,
                                     const std::vector<LIB_SYMBOL*>& aFresh )
{
    // Resolve `extends` (derived-symbol) parents. ParseLib only records the parent
    // NAME (SetParentName); the standard SCH_IO_KICAD_SEXPR_LIB_CACHE links the
    // pointer in a second pass (updateParentSymbolLinks). We bypass that cache, so
    // do it here — otherwise IsDerived() stays false and LIB_SYMBOL::Flatten()
    // (used by the chooser preview) drops the parent's body geometry, rendering
    // the preview as a zoomed-out dot. The parent is normally bundled in the same
    // body but may have parsed in a different (parallel) body, so resolve against
    // the full m_cache after every body of the library has merged.
    for( LIB_SYMBOL* symbol : aFresh )
    {
        if( symbol->GetParentName().IsEmpty() || symbol->GetParent().lock() )
            continue;

        wxString parentKey = aLibraryPath + wxS( "|" ) + symbol->GetParentName();

        if( auto pit = m_cache.find( parentKey ); pit != m_cache.end() )
            symbol->SetParent( pit->second );
    }
}


void SCH_IO_PCBJAM_LIB::cacheLibDocument( const wxString& aLibraryPath, const std::string& aBody )
{
    LIB_SYMBOL_MAP           map = parseLibDoc( aBody, std::string( aLibraryPath.utf8_str() ) );
    std::vector<LIB_SYMBOL*> fresh;

    mergeLibDoc( aLibraryPath, map, fresh );
    linkExtends( aLibraryPath, fresh );
}


void SCH_IO_PCBJAM_LIB::fatLoad( const wxString& aLibraryPath )
{
    using clock = std::chrono::steady_clock;
    auto msSince = []( clock::time_point a, clock::time_point b )
                   { return std::chrono::duration<double, std::milli>( b - a ).count(); };

    // One crossing for the whole library: "list" with arg "bodies" returns every
    // symbol's body.  request() (not requestOpt) throws if the provider yields
    // null, so a transient failure leaves the lib un-flagged and retries on the
    // next expand; an empty "symbols" array is a legitimately empty library.
    clock::time_point t0 = clock::now();
    std::string       body = request( "list", aLibraryPath, wxS( "bodies" ) );
    clock::time_point tFetch = clock::now();

    // Framed fat-list payload ("copy as-is"): a one-line JSON header
    //   {"symbols":[{"name":..,"len":<utf8 byte length>}, ...]}
    // then a single '\n', then every body's raw s-expr bytes concatenated with NO
    // JSON escaping (the provider memcpy's them across the bridge). So we parse only
    // the small header and slice the body region by byte length — none of the
    // ~450MB gets un-escaped (the old per-body JSON parse was ~22% of the cold
    // load). The header JSON is single-line, so the first '\n' is its terminator.
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

    try
    {
        nlohmann::json header = nlohmann::json::parse( body.substr( 0, nl ) );
        const auto&    arr = header.at( "symbols" );

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
    }
    catch( const nlohmann::json::exception& e )
    {
        m_lastError = wxString::Format( _( "pcbjam fat list header for '%s' is invalid: %s" ),
                                        aLibraryPath, wxString( e.what() ) );
        THROW_IO_ERROR( m_lastError );
    }

    clock::time_point tJson = clock::now();

    // Parallel s-expr parse: each worker slices its bodies out of `body` (read-only
    // shared, so concurrent reads are safe) and parses each to a LOCAL map.
    // parseLibDoc is pure (no member/global state, no bridge call), so this is safe
    // off the app thread; the wait() yields via the build's main-thread
    // nanosleep->yield shim, mirroring the 3D raytracer's submit_blocks/wait. This
    // spreads the dominant per-symbol s-expr parse over all cores; fetch (Asyncify,
    // above) and the cache merge (below) stay on the calling app thread; the lib
    // path is passed as UTF-8 std::string so no shared wxString crosses threads.
    const size_t                count = offs.size();
    std::vector<LIB_SYMBOL_MAP> maps( count );

    if( count > 0 )
    {
        const std::string libPathUtf8( aLibraryPath.utf8_str() );
        thread_pool&      tp = GetKiCadThreadPool();

        tp.submit_blocks( size_t( 0 ), count,
                [&body, &maps, &offs, &lens, &libPathUtf8]( size_t start, size_t end )
                {
                    for( size_t i = start; i < end; ++i )
                    {
                        size_t lo = std::min( offs[i], body.size() );
                        size_t hi = std::min( offs[i] + lens[i], body.size() );
                        maps[i] = parseLibDoc( std::string( body.data() + lo, hi - lo ),
                                               libPathUtf8 );
                    }
                } )
                .wait();
    }

    clock::time_point tParse = clock::now();

    // Merge every parsed body into m_cache on the app thread (cache-if-absent),
    // then resolve `extends` parents once the whole library is present.
    std::vector<LIB_SYMBOL*> fresh;

    for( LIB_SYMBOL_MAP& map : maps )
        mergeLibDoc( aLibraryPath, map, fresh );

    linkExtends( aLibraryPath, fresh );

    // Cold-path breakdown (KICAD_TRACE=KI_TRACE_SYM_CHOOSER): the fetch crossing
    // vs the (now tiny) header parse vs the parallel s-expr parse wall-time vs the
    // merge. fatLoad runs once per lib (cached after), so this is cold-only.
    KI_TRACE( wxT( "KI_TRACE_SYM_CHOOSER" ),
              wxT( "fatLoad lib=%s symbols=%zu bytes=%zu fetch_ms=%.1f header_ms=%.1f parse_ms=%.1f merge_ms=%.1f total_ms=%.1f\n" ),
              aLibraryPath, count, body.size(), msSince( t0, tFetch ), msSince( tFetch, tJson ),
              msSince( tJson, tParse ), msSince( tParse, clock::now() ), msSince( t0, clock::now() ) );

    m_libNames[aLibraryPath] = std::move( names );
    m_loadedLibs.insert( aLibraryPath );
}


LIB_SYMBOL* SCH_IO_PCBJAM_LIB::LoadSymbol( const wxString& aLibraryPath, const wxString& aAliasName,
                                           const std::map<std::string, UTF8>* aProperties )
{
    if( LIB_SYMBOL* master = loadOne( aLibraryPath, aAliasName ) )
        return new LIB_SYMBOL( *master );

    return nullptr;
}


LIB_SYMBOL* SCH_IO_PCBJAM_LIB::loadOne( const wxString& aLibraryPath, const wxString& aName )
{
    wxString cacheKey = aLibraryPath + wxS( "|" ) + aName;

    if( auto it = m_cache.find( cacheKey ); it != m_cache.end() )
        return it->second;

    // A missing symbol comes back as nullopt (not an exception): the symbol-save
    // flow probes LoadSymbol to test existence before writing, and must see a
    // clean "not found" rather than a thrown IO_ERROR.
    std::optional<std::string> got = requestOpt( "get", aLibraryPath, aName );

    if( !got )
        return nullptr;

    // Parse + cache the returned document (the requested symbol plus any bundled
    // `extends` parents); shared with the fat-load path.
    cacheLibDocument( aLibraryPath, *got );

    if( auto it = m_cache.find( cacheKey ); it != m_cache.end() )
        return it->second;

    m_lastError = wxString::Format( _( "Symbol '%s' not found in pcbjam library '%s'" ),
                                    aName, aLibraryPath );
    wxLogTrace( wxS( "PCBJAM_LIB" ), m_lastError );
    return nullptr;
}


void SCH_IO_PCBJAM_LIB::SaveSymbol( const wxString& aLibraryPath, const LIB_SYMBOL* aSymbol,
                                    const std::map<std::string, UTF8>* aProperties )
{
    wxCHECK_RET( aSymbol, wxS( "null symbol passed to SCH_IO_PCBJAM_LIB::SaveSymbol" ) );

    // The cache serializer mutates the symbol (font embedding), so format a copy.
    // Wrap the single (symbol …) block in a kicad_symbol_lib header at the fork's
    // native version — this is exactly what the on-device parser reads back, so
    // user-saved bodies round-trip without the origin-data version shim.
    LIB_SYMBOL       copy( *aSymbol );
    STRING_FORMATTER formatter;

    formatter.Print( "(kicad_symbol_lib (version %d) (generator \"pcbjam\") "
                     "(generator_version \"1.0\")\n",
                     SEXPR_SYMBOL_LIB_FILE_VERSION );
    SCH_IO_KICAD_SEXPR_LIB_CACHE::SaveSymbol( &copy, formatter );
    formatter.Print( ")\n" );

    // The bridge carries one string arg, so pass {name, body} as JSON.
    nlohmann::json payload;
    payload["name"] = std::string( aSymbol->GetName().utf8_str() );
    payload["body"] = formatter.GetString();
    std::string payloadStr = payload.dump();

    // Throws IO_ERROR if the provider rejects the write (the library manager
    // catches it and reports the save as failed).
    request( "save", aLibraryPath,
             wxString::FromUTF8( payloadStr.c_str(), payloadStr.size() ) );

    // Drop any stale cached master so the next load reflects the saved body.
    wxString key = aLibraryPath + wxS( "|" ) + aSymbol->GetName();

    if( auto it = m_cache.find( key ); it != m_cache.end() )
    {
        delete it->second;
        m_cache.erase( it );
    }

    // Invalidate the fat-load guard so the next enumerate refetches this lib —
    // picking up a newly-created item, the edited body, or a removal (and the
    // mirror overlay for an edited origin item). Saves are user-paced, so the
    // one extra "list" is negligible.
    m_loadedLibs.erase( aLibraryPath );
    m_libNames.erase( aLibraryPath );
}


void SCH_IO_PCBJAM_LIB::SaveLibrary( const wxString& aFileName,
                                     const std::map<std::string, UTF8>* aProperties )
{
    // No-op: each SaveSymbol already persisted its item through the bridge, and
    // there is no aggregate library file to flush.  Overridden purely so the
    // base class's NOT_IMPLEMENTED throw doesn't fail the save.
}
