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

#include <chrono>
#include <mutex>
#include <optional>

#include <nlohmann/json.hpp>

#include <ki_exception.h>
#include <lib_symbol.h>
#include <richio.h>
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

// Main-thread path: suspends the calling C++ stack via Asyncify until the
// provider's promise resolves.  Returns a malloc'd UTF-8 string (caller
// frees) or 0 on failure.  globalThis here is the window, where the
// standalone installs the provider.
EM_ASYNC_JS( char*, pcbjam_libs_request_js,
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

        const len = lengthBytesUTF8( res ) + 1;
        const ptr = _pcbjam_libs_alloc( len );
        stringToUTF8( res, ptr, len );
        return ptr;
    }
    catch( e )
    {
        console.error( 'kicadLibs.request failed:', e );
        return 0;
    }
} );


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
        const hook = globalThis.kicadLibs;
        const resultPtr = $4;
        const ctx = $5;

        const done = ( ptr ) => {
            HEAPU32[resultPtr >> 2] = ptr;
            _pcbjam_libs_finish( ctx );
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

                const len = lengthBytesUTF8( res ) + 1;
                const ptr = _pcbjam_libs_alloc( len );
                stringToUTF8( res, ptr, len );
                done( ptr );
            } )
            .catch( ( e ) => {
                console.error( 'kicadLibs.request failed:', e );
                done( 0 );
            } );
    }, req->op, req->lib, req->arg, req->kind, &req->result, aCtx );
}


// Serialize worker-thread proxied requests. The symbol chooser enumerates every
// library concurrently (SYMBOL_LIBRARY_ADAPTER::AsyncLoad submits one thread-pool
// task per lib), so multiple pthreads would proxy into the main thread at once.
// Each proxied task runs a tiny C function on the main thread (pcbjam_libs_
// request_on_main) — and the main thread is typically Asyncify-suspended in the
// chooser's modal pump at that moment. Concurrent C reentry into a suspended
// runtime corrupts the Asyncify/function-table state (observed as "table index
// out of bounds"). Sequential reentry is fine (proven with a single lib), so a
// global lock held across the whole proxy+fetch round-trip serializes them. The
// lock parks extra worker threads (not the main thread), so the UI stays live.
static std::mutex g_pcbjamProxyMutex;

// Dispatch on the calling thread.  Library loads come in on KiCad thread-pool
// pthreads (SYMBOL_LIBRARY_ADAPTER::AsyncLoad); there we proxy to the main
// thread and futex-block until the fetch settles — legal on a worker.  Calls
// already on the main thread use the Asyncify suspension instead (blocking
// the main thread is not an option, and proxy-to-self would deadlock).
static char* pcbjam_libs_request_dispatch( const char* aOp, const char* aLib, const char* aArg,
                                           const char* aKind )
{
    if( emscripten_is_main_runtime_thread() )
        return pcbjam_libs_request_js( aOp, aLib, aArg, aKind );

    PCBJAM_LIBS_REQ req{ aOp, aLib, aArg, aKind, nullptr };

    em_proxying_queue* queue = emscripten_proxy_get_system_queue();

    // Held across the blocking proxy call (which returns only after the JS fetch
    // resolves and calls emscripten_proxy_finish) → one in-flight request at a time.
    std::lock_guard<std::mutex> serialize( g_pcbjamProxyMutex );

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


void SCH_IO_PCBJAM_LIB::cacheLibDocument( const wxString& aLibraryPath, const std::string& aBody )
{
    STRING_LINE_READER        reader( aBody, aLibraryPath );
    SCH_IO_KICAD_SEXPR_PARSER parser( &reader );
    LIB_SYMBOL_MAP            map;

    parser.ParseLib( map );

    // The document may carry `extends` parents alongside the requested symbol;
    // cache everything it contained.  Cache-if-absent (not replace): a parent
    // re-seen in a later body keeps the already-cached master, so pointers held
    // by earlier derived symbols never dangle; the parsed duplicate is freed.
    std::vector<LIB_SYMBOL*> fresh;

    for( auto& [name, symbol] : map )
    {
        wxString key = aLibraryPath + wxS( "|" ) + name;

        if( m_cache.find( key ) != m_cache.end() )
        {
            delete symbol;
            continue;
        }

        m_cache[key] = symbol;
        fresh.push_back( symbol );
    }

    // Resolve `extends` (derived-symbol) parents. ParseLib only records the
    // parent NAME (SetParentName); the standard SCH_IO_KICAD_SEXPR_LIB_CACHE
    // links the parent pointer in a second pass (updateParentSymbolLinks). We
    // bypass that cache, so do it here — otherwise IsDerived() stays false and
    // LIB_SYMBOL::Flatten() (used by the symbol-chooser preview) drops the
    // parent's body geometry, rendering the preview as a zoomed-out dot. The
    // parent is normally bundled in the same body; fall back to the cache for a
    // parent loaded by an earlier request.
    for( LIB_SYMBOL* symbol : fresh )
    {
        if( symbol->GetParentName().IsEmpty() || symbol->GetParent().lock() )
            continue;

        wxString parentKey = aLibraryPath + wxS( "|" ) + symbol->GetParentName();

        if( auto pit = m_cache.find( parentKey ); pit != m_cache.end() )
            symbol->SetParent( pit->second );
    }
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

    std::vector<wxString> names;
    double                jsonMs = 0.0;
    double                sexprMs = 0.0;

    try
    {
        clock::time_point tj0 = clock::now();
        nlohmann::json    js = nlohmann::json::parse( body );
        jsonMs = msSince( tj0, clock::now() );

        for( const auto& item : js.at( "symbols" ) )
        {
            clock::time_point ts0 = clock::now();
            cacheLibDocument( aLibraryPath, item.at( "body" ).get<std::string>() );
            sexprMs += msSince( ts0, clock::now() );
            names.emplace_back( wxString::FromUTF8( item.at( "name" ).get<std::string>() ) );
        }
    }
    catch( const nlohmann::json::exception& e )
    {
        m_lastError = wxString::Format( _( "pcbjam fat list for '%s' is invalid: %s" ),
                                        aLibraryPath, wxString( e.what() ) );
        THROW_IO_ERROR( m_lastError );
    }

    // Cold-path breakdown (KICAD_TRACE=KI_TRACE_SYM_CHOOSER): the fetch crossing
    // (IDB read + marshal across the JS bridge) vs the JSON envelope parse vs the
    // per-symbol s-expr parse, so the first-open cost is attributable. fatLoad
    // runs once per lib (cached after), so these numbers are cold-only.
    KI_TRACE( wxT( "KI_TRACE_SYM_CHOOSER" ),
              wxT( "fatLoad lib=%s symbols=%zu bytes=%zu fetch_ms=%.1f json_ms=%.1f sexpr_ms=%.1f total_ms=%.1f\n" ),
              aLibraryPath, names.size(), body.size(), msSince( t0, tFetch ), jsonMs, sexprMs,
              msSince( t0, clock::now() ) );

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
