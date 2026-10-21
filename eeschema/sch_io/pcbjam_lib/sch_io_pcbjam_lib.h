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
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <sch_io/sch_io.h>
#include <sch_io/sch_io_mgr.h>
#include <symbol_library_common.h>

struct PCBJAM_SYMBOL_ROW;

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
 *   request( "delete", uri, name )  -> remove one symbol (a rename is
 *                                      delete-old + save-new upstream)
 *   request( "list", uri, "index" ) -> the library index as TSV, one row per
 *                                      symbol (libs 0028), or null when the
 *                                      library has none
 *   request( "index", "/mnt/pcbjam/", json uri list ) -> several libraries'
 *                                      indexes in one crossing (framed)
 *
 * The call suspends via the JSPI token wait on the main thread, or proxies
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

    void DeleteSymbol( const wxString& aLibraryPath, const wxString& aSymbolName,
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

    /// Every field name in the library, as SCH_IO_KICAD_SEXPR reports it (the
    /// union of the symbols' chooser fields): from the real symbols once the
    /// library is fat-loaded, otherwise from its index (libs 0028). This is what
    /// the chooser's "Select Columns" offers.
    void GetAvailableSymbolFields( std::vector<wxString>& aNames ) override;

    /// One provider request outside a plugin instance (the batch index fetch);
    /// nullopt when the provider yields null or there is none.
    static std::optional<std::string> BridgeRequest( const std::string& aOp,
                                                     const wxString& aLibraryPath,
                                                     const wxString& aArg );

    /// pcbjam (libs 0028): the library has been fat-loaded (real symbols cached).
    bool IsFatLoaded( const wxString& aLibraryPath ) const { return m_loadedLibs.count( aLibraryPath ) > 0; }

    /// Whether the index of the library has been asked for (rows or "none").
    bool HasIndexAnswer( const wxString& aLibraryPath ) const { return m_indexRows.count( aLibraryPath ) > 0; }

    /// Store the provider's index answer for the library (TSV; empty = none).
    void SetIndexAnswer( const wxString& aLibraryPath, const std::string& aTsv );

    /// The library's index rows, fetched in one crossing when not stored yet;
    /// null when the library has no index (the caller then parses it).
    std::shared_ptr<const std::vector<PCBJAM_SYMBOL_ROW>> IndexRows( const wxString& aLibraryPath );

    /// Background warm-up (libs 0028 8.3): the row the next page should start at.
    size_t WarmCursor( const wxString& aLibraryPath ) const;

    enum class WARM_PAGE
    {
        MORE,     ///< page stored, more rows to fetch
        COMPLETE, ///< the library's rows are now stored (or it has no index)
        STALE     ///< the page didn't follow the stored ones; start again
    };

    /// Add one `index:<from>` page (TSV) to the library's partial rows; publishes
    /// them as the library's index once the last page is in. Parses only: no
    /// bridge crossing, so it is safe right after a step's suspension.
    WARM_PAGE AddIndexPage( const wxString& aLibraryPath, const std::string& aTsv );

    /// A text-variable row whose real symbol isn't cached yet (the chooser lists
    /// those as real symbols), or empty. The warm-up loads them ahead of the click.
    wxString NextUncachedTextVarSymbol( const wxString& aLibraryPath ) const;

    /// Cache a provider "get" answer fetched outside the plugin (the warm-up).
    void CacheItemDocument( const wxString& aLibraryPath, const std::string& aBody )
    {
        cacheLibDocument( aLibraryPath, aBody );
    }

    /// The cache-owned master of one symbol (loaded with one "get" if needed);
    /// non-owning, like the symbols EnumerateSymbolLib hands out.
    LIB_SYMBOL* LoadMaster( const wxString& aLibraryPath, const wxString& aName )
    {
        return loadOne( aLibraryPath, aName );
    }

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

    /// Merge an already-parsed document's symbols into m_cache (cache-if-absent;
    /// duplicates are deleted), appending newly-cached masters to aFresh. Does NOT
    /// link `extends` parents — call linkExtends() once after all merges so a
    /// derived symbol whose parent lives in a different (parallel-parsed) body can
    /// still resolve. m_cache is plugin-local, so this runs on the app thread.
    void mergeLibDoc( const wxString& aLibraryPath, LIB_SYMBOL_MAP& aMap,
                      std::vector<LIB_SYMBOL*>& aFresh );

    /// Resolve `extends` (derived-symbol) parent pointers for aFresh against
    /// m_cache (run after all of a library's bodies are merged).
    void linkExtends( const wxString& aLibraryPath, const std::vector<LIB_SYMBOL*>& aFresh );

    /// Fetch the whole library in one "fat list" crossing and cache every body;
    /// records the lib's symbol names so a repeat enumerate rebuilds from cache.
    /// The per-symbol s-expr parse (the dominant cost) runs in parallel on the
    /// KiCad thread pool; fetch + merge stay on the calling (app) thread.
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

    /// libs 0028: each library's index rows; a null entry = asked, no index.
    /// Dropped with the plugin (kicadLibsInvalidate) and on SaveSymbol/DeleteSymbol.
    std::map<wxString, std::shared_ptr<const std::vector<PCBJAM_SYMBOL_ROW>>> m_indexRows;

    /// Warm-up pages received so far, per library (until the last one arrives).
    struct WARM_STATE
    {
        std::string                    token;
        std::vector<PCBJAM_SYMBOL_ROW> rows;
    };

    std::map<wxString, WARM_STATE> m_warm;

    /// The library this instance serves (one plugin per library row), for the
    /// path-less GetAvailableSymbolFields.
    wxString m_libraryPath;

    wxString m_lastError;
};
