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

#include <nlohmann/json.hpp>

#include <ki_exception.h>
#include <lib_symbol.h>
#include <richio.h>
#include <wx/log.h>

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
             ( const char* aOp, const char* aLib, const char* aArg ), {
    const hook = globalThis.kicadLibs;

    if( !hook || !hook.request )
        return 0;

    try
    {
        const res = await hook.request( UTF8ToString( aOp ), UTF8ToString( aLib ),
                                        UTF8ToString( aArg ) );

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
        const resultPtr = $3;
        const ctx = $4;

        const done = ( ptr ) => {
            HEAPU32[resultPtr >> 2] = ptr;
            _pcbjam_libs_finish( ctx );
        };

        if( !hook || !hook.request )
        {
            done( 0 );
            return;
        }

        hook.request( UTF8ToString( $0 ), UTF8ToString( $1 ), UTF8ToString( $2 ) )
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
    }, req->op, req->lib, req->arg, &req->result, aCtx );
}


// Dispatch on the calling thread.  Library loads come in on KiCad thread-pool
// pthreads (SYMBOL_LIBRARY_ADAPTER::AsyncLoad); there we proxy to the main
// thread and futex-block until the fetch settles — legal on a worker.  Calls
// already on the main thread use the Asyncify suspension instead (blocking
// the main thread is not an option, and proxy-to-self would deadlock).
static char* pcbjam_libs_request_dispatch( const char* aOp, const char* aLib, const char* aArg )
{
    if( emscripten_is_main_runtime_thread() )
        return pcbjam_libs_request_js( aOp, aLib, aArg );

    PCBJAM_LIBS_REQ req{ aOp, aLib, aArg, nullptr };

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
    return aFileName.StartsWith( wxS( "/mnt/pcbjam/" ) );
}


std::string SCH_IO_PCBJAM_LIB::request( const std::string& aOp, const wxString& aLibraryPath,
                                        const wxString& aArg )
{
#ifdef __EMSCRIPTEN__
    char* res = pcbjam_libs_request_dispatch( aOp.c_str(), aLibraryPath.utf8_str().data(),
                                              aArg.utf8_str().data() );

    if( !res )
    {
        m_lastError = wxString::Format( _( "pcbjam library provider failed: %s %s %s" ),
                                        wxString( aOp ), aLibraryPath, aArg );
        THROW_IO_ERROR( m_lastError );
    }

    std::string out( res );
    free( res );
    return out;
#else
    m_lastError = _( "pcbjam libraries are only available in the web build" );
    THROW_IO_ERROR( m_lastError );
#endif
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
    wxArrayString names;
    EnumerateSymbolLib( names, aLibraryPath, aProperties );

    for( const wxString& name : names )
    {
        if( LIB_SYMBOL* master = loadOne( aLibraryPath, name ) )
            aSymbolList.emplace_back( new LIB_SYMBOL( *master ) );
    }
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

    std::string body = request( "get", aLibraryPath, aName );

    STRING_LINE_READER        reader( body, aLibraryPath + wxS( ":" ) + aName );
    SCH_IO_KICAD_SEXPR_PARSER parser( &reader );
    LIB_SYMBOL_MAP            map;

    parser.ParseLib( map );

    LIB_SYMBOL* found = nullptr;

    // The document may carry `extends` parents alongside the requested
    // symbol; cache everything it contained.
    for( auto& [name, symbol] : map )
    {
        wxString key = aLibraryPath + wxS( "|" ) + name;

        if( auto it = m_cache.find( key ); it != m_cache.end() )
            delete it->second;

        m_cache[key] = symbol;

        if( name == aName )
            found = symbol;
    }

    if( !found )
    {
        m_lastError = wxString::Format( _( "Symbol '%s' not found in pcbjam library '%s'" ),
                                        aName, aLibraryPath );
        wxLogTrace( wxS( "PCBJAM_LIB" ), m_lastError );
    }

    return found;
}
