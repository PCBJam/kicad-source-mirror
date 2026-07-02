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
#include <cstring>
#include <exception>

#include "pcbjam_static_3d_plugins.h"

class SCENEGRAPH;

// The plugins are compiled with per-source-file COMPILE_DEFINITIONS renaming their
// flat C ABI (see plugins/3d/{vrml,oce}/CMakeLists.txt): every export of vrml.cpp
// becomes vrml3d_* — except `Load`, which keeps its upstream name (the token also
// names X3DPARSER::Load in that TU; unique in the binary because oce's IS renamed)
// — and every export of oce.cpp becomes oce3d_*.  Declared here rather than
// through 3d_plugin.h because that header *defines* three of them.
extern "C"
{
    char const* vrml3d_GetKicadPluginClass( void );
    void        vrml3d_GetClassVersion( unsigned char*, unsigned char*, unsigned char*,
                                        unsigned char* );
    bool        vrml3d_CheckClassVersion( unsigned char, unsigned char, unsigned char,
                                          unsigned char );
    const char* vrml3d_GetKicadPluginName( void );
    void        vrml3d_GetPluginVersion( unsigned char*, unsigned char*, unsigned char*,
                                         unsigned char* );
    int         vrml3d_GetNExtensions( void );
    char const* vrml3d_GetModelExtension( int aIndex );
    int         vrml3d_GetNFilters( void );
    char const* vrml3d_GetFileFilter( int aIndex );
    bool        vrml3d_CanRender( void );
    SCENEGRAPH* Load( char const* aFileName );  // vrml's, un-renamed (see above)

    char const* oce3d_GetKicadPluginClass( void );
    void        oce3d_GetClassVersion( unsigned char*, unsigned char*, unsigned char*,
                                       unsigned char* );
    bool        oce3d_CheckClassVersion( unsigned char, unsigned char, unsigned char,
                                         unsigned char );
    const char* oce3d_GetKicadPluginName( void );
    void        oce3d_GetPluginVersion( unsigned char*, unsigned char*, unsigned char*,
                                        unsigned char* );
    int         oce3d_GetNExtensions( void );
    char const* oce3d_GetModelExtension( int aIndex );
    int         oce3d_GetNFilters( void );
    char const* oce3d_GetFileFilter( int aIndex );
    bool        oce3d_CanRender( void );
    SCENEGRAPH* oce3d_Load( char const* aFileName );
}


namespace
{

// Wrap the plugins' Load with (a) a per-model console line — model loads are
// rare (once per unique file) and this is the only field-visible signal of a
// parse failure, wxLogTrace being mask-gated — and (b) an exception barrier:
// OCCT throws Standard_Failure on malformed STEP internals, and letting that
// unwind into the Asyncify-instrumented scene build would kill the whole
// viewer instead of skipping one model.
SCENEGRAPH* loggedLoad( const char* aPlugin, SCENEGRAPH* ( *aLoad )( char const* ),
                        char const* aFileName )
{
    SCENEGRAPH* sp = nullptr;

    try
    {
        sp = aLoad( aFileName );
    }
    catch( const std::exception& e )
    {
        std::printf( "[pcbjam-3d] %s Load threw: %s (%s)\n", aPlugin, e.what(),
                     aFileName ? aFileName : "?" );
        return nullptr;
    }
    catch( ... )
    {
        std::printf( "[pcbjam-3d] %s Load threw (non-std) (%s)\n", aPlugin,
                     aFileName ? aFileName : "?" );
        return nullptr;
    }

    std::printf( "[pcbjam-3d] %s Load %s: %s\n", aPlugin, sp ? "ok" : "FAILED",
                 aFileName ? aFileName : "?" );
    return sp;
}

SCENEGRAPH* vrmlLoadLogged( char const* aFileName )
{
    return loggedLoad( "vrml", &Load, aFileName );
}

SCENEGRAPH* oceLoadLogged( char const* aFileName )
{
    return loggedLoad( "oce", &oce3d_Load, aFileName );
}

struct STATIC_3D_PLUGIN_ENTRY
{
    const char* symbol;
    void*       fn;
};

struct STATIC_3D_PLUGIN
{
    const wxChar*                 path;
    const STATIC_3D_PLUGIN_ENTRY* entries;
};

#define PCBJAM_PLUGIN_TABLE( prefix, loadFn )                                \
    {                                                                        \
        { "GetKicadPluginClass", (void*) &prefix##_GetKicadPluginClass },    \
        { "GetClassVersion", (void*) &prefix##_GetClassVersion },            \
        { "CheckClassVersion", (void*) &prefix##_CheckClassVersion },        \
        { "GetKicadPluginName", (void*) &prefix##_GetKicadPluginName },      \
        { "GetPluginVersion", (void*) &prefix##_GetPluginVersion },          \
        { "GetNExtensions", (void*) &prefix##_GetNExtensions },              \
        { "GetModelExtension", (void*) &prefix##_GetModelExtension },        \
        { "GetNFilters", (void*) &prefix##_GetNFilters },                    \
        { "GetFileFilter", (void*) &prefix##_GetFileFilter },                \
        { "CanRender", (void*) &prefix##_CanRender },                        \
        { "Load", (void*) &loadFn },                                         \
        { nullptr, nullptr }                                                 \
    }

const STATIC_3D_PLUGIN_ENTRY vrmlEntries[] = PCBJAM_PLUGIN_TABLE( vrml3d, vrmlLoadLogged );
const STATIC_3D_PLUGIN_ENTRY oceEntries[] = PCBJAM_PLUGIN_TABLE( oce3d, oceLoadLogged );

const STATIC_3D_PLUGIN plugins[] = {
    { wxT( "static://vrml" ), vrmlEntries },
    { wxT( "static://oce" ), oceEntries },
};

} // namespace


std::list<wxString> PCBJAM_3D::StaticPluginPaths()
{
    std::list<wxString> paths;

    for( const STATIC_3D_PLUGIN& plugin : plugins )
        paths.emplace_back( plugin.path );

    // Once per S3D_PLUGIN_MANAGER construction — field-visible proof that the
    // static registry fed the loader (there is no dlopen to observe).
    std::printf( "[pcbjam-3d] registering %zu static plugins\n", paths.size() );

    return paths;
}


void* PCBJAM_3D::StaticPluginSymbol( const wxString& aPluginPath, const char* aSymbolName )
{
    for( const STATIC_3D_PLUGIN& plugin : plugins )
    {
        if( aPluginPath != plugin.path )
            continue;

        for( const STATIC_3D_PLUGIN_ENTRY* entry = plugin.entries; entry->symbol; ++entry )
        {
            if( std::strcmp( entry->symbol, aSymbolName ) == 0 )
                return entry->fn;
        }

        std::printf( "[pcbjam-3d] %s: unknown symbol %s\n",
                     (const char*) aPluginPath.utf8_str(), aSymbolName );
        return nullptr;
    }

    std::printf( "[pcbjam-3d] unknown static plugin '%s' (symbol %s)\n",
                 (const char*) aPluginPath.utf8_str(), aSymbolName );
    return nullptr;
}
