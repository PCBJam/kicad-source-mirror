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

#pragma once

#include <map>
#include <optional>
#include <set>
#include <vector>

#include <sch_io/sch_io.h>
#include <sch_io/sch_io_mgr.h>
#include <symbol_library_common.h>

/**
 * A symbol library plugin backed by a JS-side provider (globalThis.kicadLibs)
 * in the WASM build.  Library URIs are absolute POSIX paths so KiCad's
 * lib-table URI expansion (LIBRARY_MANAGER::ExpandURI -> wxFileName::
 * MakeAbsolute) is a no-op and the path reaches the plugin/provider unmangled
 * (a "scheme://" URI gets rewritten to "/scheme:/..." against the cwd).  Every
 * lib mounts under a single root "/mnt/pcbjam/<lib>" and reports writable; what
 * a save MEANS (write a user lib, mirror an origin, or reject) is decided
 * entirely by the provider/server — the fork stays agnostic to origin/user.
 *
 * The provider answers:
 *
 *   request( "list", uri, "" )      -> JSON {"symbols":["R","C",...]}
 *   request( "list", uri, "bodies" )-> JSON {"symbols":[{"name":..,"body":..}]}
 *                                      the "fat list": every symbol's body in
 *                                      ONE crossing, so a whole library hydrates
 *                                      without N per-item "get"s (0011)
 *   request( "get",  uri, name )    -> a complete kicad_symbol_lib s-expr
 *                                      document containing that one symbol
 *                                      (plus any `extends` parents), or null
 *                                      if the symbol does not exist
 *   request( "save", uri, json )    -> persist one symbol; json is
 *                                      {"name":..,"body":<kicad_symbol_lib>}
 *
 * The call suspends via Asyncify (EM_ASYNC_JS) on the main thread, or proxies
 * to the main thread and futex-blocks on a worker, until the provider's
 * promise resolves.  Outside Emscripten builds every operation throws IO_ERROR.
 */
class SCH_IO_PCBJAM_LIB : public SCH_IO
{
public:
    SCH_IO_PCBJAM_LIB();
    ~SCH_IO_PCBJAM_LIB() override;

    const IO_BASE::IO_FILE_DESC GetLibraryDesc() const override
    {
        return IO_BASE::IO_FILE_DESC( _HKI( "pcbjam remote symbol library" ), {} );
    }

    bool CanReadLibrary( const wxString& aFileName ) const override;

    int GetModifyHash() const override { return 0; }

    void EnumerateSymbolLib( wxArrayString& aSymbolNameList, const wxString& aLibraryPath,
                             const std::map<std::string, UTF8>* aProperties = nullptr ) override;

    void EnumerateSymbolLib( std::vector<LIB_SYMBOL*>& aSymbolList, const wxString& aLibraryPath,
                             const std::map<std::string, UTF8>* aProperties = nullptr ) override;

    LIB_SYMBOL* LoadSymbol( const wxString& aLibraryPath, const wxString& aAliasName,
                            const std::map<std::string, UTF8>* aProperties = nullptr ) override;

    void SaveSymbol( const wxString& aLibraryPath, const LIB_SYMBOL* aSymbol,
                     const std::map<std::string, UTF8>* aProperties = nullptr ) override;

    /// Per-item plugin: SaveSymbol persists immediately, so this is a no-op.
    /// (Override so the base class's NOT_IMPLEMENTED throw doesn't fail saves.)
    void SaveLibrary( const wxString& aFileName,
                      const std::map<std::string, UTF8>* aProperties = nullptr ) override;

    /// Every pcbjam lib is writable; save semantics are server-side policy.
    bool IsLibraryWritable( const wxString& aLibraryPath ) override
    {
        return aLibraryPath.StartsWith( wxS( "/mnt/pcbjam/" ) );
    }

    const wxString& GetError() const override { return m_lastError; }

private:
    /// Perform a provider request; returns the response or throws IO_ERROR.
    std::string request( const std::string& aOp, const wxString& aLibraryPath,
                         const wxString& aArg );

    /// Like request() but returns nullopt (instead of throwing) when the
    /// provider yields null — used by "get" so a missing symbol reads as
    /// "not found" (the save flow's existence checks rely on this).
    std::optional<std::string> requestOpt( const std::string& aOp, const wxString& aLibraryPath,
                                           const wxString& aArg );

    /// Load (or return cached) master copy of one symbol.
    LIB_SYMBOL* loadOne( const wxString& aLibraryPath, const wxString& aName );

    /// Parse one kicad_symbol_lib document and cache every symbol it holds
    /// (the requested symbol plus any intra-lib `extends` parents).
    void cacheLibDocument( const wxString& aLibraryPath, const std::string& aBody );

    /// Fetch the whole library in one "fat list" crossing and cache every body;
    /// records the lib's symbol names so a repeat enumerate rebuilds from cache.
    void fatLoad( const wxString& aLibraryPath );

    /// Per-(lib,name) master symbols owned by this plugin; callers get clones.
    std::map<wxString, LIB_SYMBOL*> m_cache;

    /// Libraries already fat-loaded (so a repeat enumerate hits the cache, no
    /// crossing). Invalidated for a lib on SaveSymbol so the next enumerate
    /// refreshes (new/changed/removed items).
    std::set<wxString> m_loadedLibs;

    /// The symbol names of each fat-loaded lib, in list order — used to rebuild
    /// the enumerate result from m_cache without re-fetching.
    std::map<wxString, std::vector<wxString>> m_libNames;

    wxString m_lastError;
};
