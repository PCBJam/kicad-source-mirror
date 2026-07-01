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

#include <pcb_io/pcb_io.h>

class FOOTPRINT;

/**
 * A footprint library plugin backed by a JS-side provider (globalThis.kicadLibs)
 * in the WASM build — the footprint counterpart of SCH_IO_PCBJAM_LIB.  It reuses
 * the exact same JS bridge: a single window.kicadLibs.request(op, lib, arg, kind)
 * hook, here always called with kind = "footprint".
 *
 * Library URIs are absolute POSIX paths so KiCad's lib-table URI expansion
 * (wxFileName::MakeAbsolute) is a no-op and the path reaches the provider
 * unmangled.  Every lib mounts under the single root "/mnt/pcbjam/<lib>" and
 * reports writable; what a save MEANS (write a user lib, mirror an origin, or
 * reject) is decided entirely by the provider/server — the fork stays agnostic.
 *
 * Note: the footprint editor selects this plugin from the fp-lib-table row's
 * type field ("PCBJAM_FP" -> PCB_IO_MGR::EnumFromStr -> FindPlugin), unlike the
 * symbol side which routes saves by probing CanReadLibrary.  Both are honoured
 * here: the row type selects the plugin and CanReadLibrary matches the mount.
 *
 * The provider answers (kind = "footprint"):
 *
 *   request( "list", uri, "",   "footprint" ) -> JSON {"footprints":["R_0402",..]}
 *   request( "list", uri, "bodies", "footprint" ) -> the "fat list": a framed
 *                                                Uint8Array — one-line JSON header
 *                                                {"footprints":[{"name":..,"len":..}]}
 *                                                then '\n' then every body's raw
 *                                                s-expr bytes concatenated (copied
 *                                                as-is, no JSON escaping). A whole
 *                                                library hydrates in ONE crossing,
 *                                                parsed in parallel (0011 / 0013)
 *   request( "get",  uri, name, "footprint" ) -> a complete (footprint …) s-expr
 *                                                document, or null if not found
 *   request( "save", uri, json, "footprint" ) -> persist one footprint; json is
 *                                                {"name":..,"body":<(footprint …)>}
 *
 * The call suspends via Asyncify (EM_ASYNC_JS) on the main thread, or proxies to
 * the main thread and futex-blocks on a worker, until the provider's promise
 * resolves.  Outside Emscripten builds every operation throws IO_ERROR.
 */
class PCB_IO_PCBJAM_FP : public PCB_IO
{
public:
    PCB_IO_PCBJAM_FP();
    ~PCB_IO_PCBJAM_FP() override;

    const IO_BASE::IO_FILE_DESC GetLibraryDesc() const override
    {
        return IO_BASE::IO_FILE_DESC( _HKI( "pcbjam remote footprint library" ), {} );
    }

    bool CanReadLibrary( const wxString& aFileName ) const override;

    // Timestamps are only compared for equality by the adapter's cache; a stable
    // constant means "never changed under us" — our own cache is dropped on save.
    long long GetLibraryTimestamp( const wxString& aLibraryPath ) const override { return 0; }

    void FootprintEnumerate( wxArrayString& aFootprintNames, const wxString& aLibraryPath,
                             bool aBestEfforts,
                             const std::map<std::string, UTF8>* aProperties = nullptr ) override;

    FOOTPRINT* FootprintLoad( const wxString& aLibraryPath, const wxString& aFootprintName,
                              bool aKeepUUID = false,
                              const std::map<std::string, UTF8>* aProperties = nullptr ) override;

    void FootprintSave( const wxString& aLibraryPath, const FOOTPRINT* aFootprint,
                        const std::map<std::string, UTF8>* aProperties = nullptr ) override;

    /// Every pcbjam lib is writable; save semantics are server-side policy.
    bool IsLibraryWritable( const wxString& aLibraryPath ) override
    {
        return aLibraryPath.StartsWith( wxS( "/mnt/pcbjam/" ) );
    }

private:
    /// Perform a provider request; returns the response or throws IO_ERROR.
    std::string request( const std::string& aOp, const wxString& aLibraryPath,
                         const wxString& aArg );

    /// Like request() but returns nullopt (instead of throwing) when the provider
    /// yields null — used by "get" so a missing footprint reads as "not found"
    /// (CreateNewFootprint's uniqueness probe relies on this).
    std::optional<std::string> requestOpt( const std::string& aOp, const wxString& aLibraryPath,
                                           const wxString& aArg );

    /// Load (or return cached) master copy of one footprint; callers get clones.
    FOOTPRINT* loadOne( const wxString& aLibraryPath, const wxString& aName );

    /// Parse one (footprint …) document and cache it (cache-if-absent). A parse
    /// failure is logged and skipped, leaving the item simply unavailable. Thin
    /// wrapper over parseFpDoc (pure) + mergeFpDoc (cache) for the single-get path.
    void cacheFootprint( const wxString& aLibraryPath, const wxString& aName,
                         const std::string& aBody );

    /// Cache-if-absent one already-parsed footprint master (app thread). Takes
    /// ownership: an absent key adopts it, a duplicate frees it. Null is a no-op.
    void mergeFpDoc( const wxString& aLibraryPath, const wxString& aName, FOOTPRINT* aFootprint );

    /// Fetch the whole library in one "fat list" crossing and cache every body;
    /// records the footprint names so a repeat enumerate rebuilds from cache.
    void fatLoad( const wxString& aLibraryPath );

    std::map<wxString, FOOTPRINT*> m_cache;

    /// Libraries already fat-loaded (so the adapter's per-name FootprintLoad calls
    /// all hit the cache, no crossing). Invalidated for a lib on FootprintSave.
    std::set<wxString> m_loadedLibs;

    /// The footprint names of each fat-loaded lib, in list order — used to answer
    /// a repeat FootprintEnumerate from cache without re-fetching.
    std::map<wxString, std::vector<wxString>> m_libNames;

    wxString m_lastError;
};
