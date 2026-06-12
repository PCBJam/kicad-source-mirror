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

#include <sch_io/sch_io.h>
#include <sch_io/sch_io_mgr.h>
#include <symbol_library_common.h>

/**
 * A read-only symbol library plugin backed by a JS-side provider
 * (globalThis.kicadLibs) in the WASM build.  Library URIs look like
 * "pcbjam://<source>/<lib>"; the provider answers two operations:
 *
 *   request( "list", uri, "" )      -> JSON {"symbols":["R","C",...]}
 *   request( "get",  uri, name )    -> a complete kicad_symbol_lib s-expr
 *                                      document containing that one symbol
 *                                      (plus any `extends` parents)
 *
 * The call suspends via Asyncify (EM_ASYNC_JS) until the provider's promise
 * resolves.  Outside Emscripten builds every operation throws IO_ERROR.
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

    bool IsLibraryWritable( const wxString& aLibraryPath ) override { return false; }

    const wxString& GetError() const override { return m_lastError; }

private:
    /// Perform a provider request; returns the response or throws IO_ERROR.
    std::string request( const std::string& aOp, const wxString& aLibraryPath,
                         const wxString& aArg );

    /// Load (or return cached) master copy of one symbol.
    LIB_SYMBOL* loadOne( const wxString& aLibraryPath, const wxString& aName );

    /// Per-(lib,name) master symbols owned by this plugin; callers get clones.
    std::map<wxString, LIB_SYMBOL*> m_cache;

    wxString m_lastError;
};
