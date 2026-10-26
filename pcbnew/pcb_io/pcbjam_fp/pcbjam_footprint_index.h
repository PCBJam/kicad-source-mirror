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
#include <vector>

#include <lib_id.h>
#include <lib_tree_item.h>

/**
 * pcbjam (libs 0028 phase 9): the footprint chooser's and the footprint
 * editor's library tree from the library index, without parsing a library.
 *
 * The JS provider answers a footprint library's index as tab-separated rows
 * (@pcbjam/shared lib-index.ts encodeFootprintIndexTsv). PCBJAM_FOOTPRINT_STUB
 * is a throwaway LIB_TREE_ITEM built from a row that reproduces what the tree
 * copies out of a FOOTPRINT: name, description, search terms (as
 * FOOTPRINT::GetSearchTerms) and the unique pad count.
 */
struct PCBJAM_FOOTPRINT_ROW
{
    std::string name;
    std::string description; ///< (descr …), raw
    std::string keywords;    ///< (tags …), raw
    int         padCount = 0;       ///< GetPadCount( DO_NOT_INCLUDE_NPTH )
    int         uniquePadCount = 0; ///< GetUniquePadCount( DO_NOT_INCLUDE_NPTH )
};


/// Parse the provider's footprint TSV for one library. False when it isn't a
/// schema-1 footprint index.
bool ParseFootprintIndexTsv( const std::string& aTsv, std::vector<PCBJAM_FOOTPRINT_ROW>& aRows );


/// Split the batch answer ("\x1d<uri>\n<tsv>" frames) into uri -> tsv.
std::map<std::string, std::string> SplitFootprintIndexBatch( const std::string& aBatch );


class PCBJAM_FOOTPRINT_STUB : public LIB_TREE_ITEM
{
public:
    PCBJAM_FOOTPRINT_STUB( const PCBJAM_FOOTPRINT_ROW& aRow, const wxString& aNickname );

    LIB_ID   GetLIB_ID() const override { return m_libId; }
    wxString GetName() const override { return m_name; }
    wxString GetLibNickname() const override { return m_nickname; }
    wxString GetDesc() override { return m_desc; }
    std::vector<SEARCH_TERM>& GetSearchTerms() override { return m_searchTerms; }
    int      GetPinCount() override { return m_uniquePadCount; }

private:
    LIB_ID                   m_libId;
    wxString                 m_name;
    wxString                 m_nickname;
    wxString                 m_desc;
    std::vector<SEARCH_TERM> m_searchTerms;
    int                      m_uniquePadCount;
};
