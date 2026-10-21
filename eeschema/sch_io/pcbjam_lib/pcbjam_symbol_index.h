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
#include <string>
#include <utility>
#include <vector>

#include <lib_id.h>
#include <lib_tree_item.h>

/**
 * pcbjam (libs 0028): the symbol chooser tree from the library index, without
 * parsing a library into LIB_SYMBOLs.
 *
 * The JS provider answers a library's index as tab-separated rows (one per
 * symbol, already resolved against their `extends` parents; @pcbjam/shared
 * lib-index.ts encodeLibIndexTsv). PCBJAM_SYMBOL_ROW holds one row as compact
 * UTF-8 strings; PCBJAM_SYMBOL_STUB is a throwaway LIB_TREE_ITEM built from a
 * row that reproduces exactly what LIB_TREE_NODE_ITEM copies out of a parsed
 * LIB_SYMBOL (name, description, footprint, chooser fields, search terms,
 * root/power flags, units), so the tree can't tell the two apart.
 */
struct PCBJAM_SYMBOL_ROW
{
    std::string name;
    std::string extends;      ///< parent name; empty for a root symbol
    int         power = 0;    ///< 1 = (power), 2 = (power local); from the root
    int         units = 1;    ///< unit count; from the root
    int         graphicalPins = 0;
    int         pinCount = 0;
    bool        textVars = false; ///< some text holds ${ or @{: let KiCad resolve it
    std::string description;  ///< raw, as in the file
    std::string keywords;
    std::string fpFilters;
    std::string footprint;
    std::string reference;
    std::string value;
    std::string datasheet;
    std::vector<std::pair<int, std::string>>         unitNames;
    std::vector<std::pair<std::string, std::string>> fields; ///< every other property
};


/// Parse the provider's TSV for one library. False when it isn't a schema-1
/// symbol index. `aToken` gets the library state token from the header; for a
/// page (`index:<from>`), `aFrom` / `aTotal` get where it starts and how many
/// rows the library has (both -1 for a whole-library answer).
bool ParseSymbolIndexTsv( const std::string& aTsv, std::vector<PCBJAM_SYMBOL_ROW>& aRows,
                          std::string* aToken = nullptr, long* aFrom = nullptr,
                          long* aTotal = nullptr );


/// Split the batch answer ("\x1d<uri>\n<tsv>" frames) into uri -> tsv; an empty
/// tsv means the provider has no index for that library.
std::map<std::string, std::string> SplitSymbolIndexBatch( const std::string& aBatch );


class PCBJAM_SYMBOL_STUB : public LIB_TREE_ITEM
{
public:
    /// @param aRoot whether the row's `extends` parent is absent from the library
    ///        (KiCad then leaves the symbol unlinked, and it counts as a root).
    PCBJAM_SYMBOL_STUB( const PCBJAM_SYMBOL_ROW& aRow, const wxString& aNickname, bool aRoot );

    LIB_ID   GetLIB_ID() const override { return m_libId; }
    wxString GetName() const override { return m_name; }
    wxString GetLibNickname() const override { return m_nickname; }
    wxString GetDesc() override { return m_desc; }
    void     GetChooserFields( std::map<wxString, wxString>& aColumnMap ) override { aColumnMap = m_fields; }
    std::vector<SEARCH_TERM>& GetSearchTerms() override { return m_searchTerms; }
    bool     IsRoot() const override { return m_isRoot; }
    bool     IsPowerSymbol() const override { return m_isPower; }
    wxString GetFootprint() override { return m_footprint; }
    int      GetPinCount() override { return m_pinCount; }
    int      GetSubUnitCount() const override { return m_units; }
    wxString GetUnitName( int aUnit ) const override;

private:
    LIB_ID                       m_libId;
    wxString                     m_name;
    wxString                     m_nickname;
    wxString                     m_desc;
    wxString                     m_footprint;
    std::map<wxString, wxString> m_fields;
    std::vector<SEARCH_TERM>     m_searchTerms;
    std::map<int, wxString>      m_unitNames;
    bool                         m_isRoot;
    bool                         m_isPower;
    int                          m_pinCount;
    int                          m_units;
};
