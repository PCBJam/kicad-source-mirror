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
#include <wx/tokenzr.h>

#include <footprint.h>
#include <footprint_library_adapter.h>
#include <trace_helpers.h>

#include <pcb_io/pcbjam_fp/pcbjam_footprint_index.h>
#include <pcb_io/pcbjam_fp/pcb_io_pcbjam_fp.h>


namespace
{
std::string unescapeTsv( const std::string& aIn )
{
    if( aIn.find( '\\' ) == std::string::npos )
        return aIn;

    std::string out;
    out.reserve( aIn.size() );

    for( size_t i = 0; i < aIn.size(); ++i )
    {
        if( aIn[i] != '\\' || i + 1 >= aIn.size() )
        {
            out += aIn[i];
            continue;
        }

        char e = aIn[++i];
        out += e == 't' ? '\t' : e == 'n' ? '\n' : e;
    }

    return out;
}


int toInt( const std::string& aIn )
{
    try
    {
        return aIn.empty() ? 0 : std::stoi( aIn );
    }
    catch( ... )
    {
        return 0;
    }
}
} // namespace


bool ParseFootprintIndexTsv( const std::string& aTsv, std::vector<PCBJAM_FOOTPRINT_ROW>& aRows )
{
    size_t      nl = aTsv.find( '\n' );
    std::string header = aTsv.substr( 0, nl );

    if( header.rfind( "v=", 0 ) != 0 || header.find( "\tschema=1\t" ) == std::string::npos
            || header.find( "\tkind=footprint" ) == std::string::npos )
    {
        return false;
    }

    size_t start = nl == std::string::npos ? aTsv.size() : nl + 1;

    while( start < aTsv.size() )
    {
        size_t end = aTsv.find( '\n', start );

        if( end == std::string::npos )
            end = aTsv.size();

        std::vector<std::string> c;
        size_t                   from = start;

        for( size_t i = start; i <= end; ++i )
        {
            if( i == end || aTsv[i] == '\t' )
            {
                c.emplace_back( aTsv, from, i - from );
                from = i + 1;
            }
        }

        start = end + 1;

        if( c.size() < 5 )
            continue;

        PCBJAM_FOOTPRINT_ROW& row = aRows.emplace_back();
        row.name = unescapeTsv( c[0] );
        row.description = unescapeTsv( c[1] );
        row.keywords = unescapeTsv( c[2] );
        row.padCount = toInt( c[3] );
        row.uniquePadCount = toInt( c[4] );
    }

    return true;
}


std::map<std::string, std::string> SplitFootprintIndexBatch( const std::string& aBatch )
{
    std::map<std::string, std::string> out;
    size_t                             pos = aBatch.find( '\x1d' );

    while( pos != std::string::npos )
    {
        size_t next = aBatch.find( '\x1d', pos + 1 );
        size_t nl = aBatch.find( '\n', pos );
        size_t end = next == std::string::npos ? aBatch.size() : next;

        if( nl != std::string::npos && nl < end )
            out[aBatch.substr( pos + 1, nl - pos - 1 )] = aBatch.substr( nl + 1, end - nl - 1 );

        pos = next;
    }

    return out;
}


PCBJAM_FOOTPRINT_STUB::PCBJAM_FOOTPRINT_STUB( const PCBJAM_FOOTPRINT_ROW& aRow,
                                              const wxString& aNickname ) :
        m_name( wxString::FromUTF8( aRow.name ) ),
        m_nickname( aNickname ),
        m_desc( wxString::FromUTF8( aRow.description ) ),
        m_uniquePadCount( aRow.uniquePadCount )
{
    m_libId.SetLibNickname( aNickname );
    m_libId.SetLibItemName( m_name );

    // As FOOTPRINT::GetSearchTerms, same order and weights.
    wxString keywords = wxString::FromUTF8( aRow.keywords );

    m_searchTerms.reserve( 6 );
    m_searchTerms.emplace_back( SEARCH_TERM( aNickname, 4 ) );
    m_searchTerms.emplace_back( SEARCH_TERM( m_name, 8, true ) );
    m_searchTerms.emplace_back( SEARCH_TERM( m_libId.Format().wx_str(), 16, true ) );

    wxStringTokenizer keywordTokenizer( keywords, wxS( " \t\r\n" ), wxTOKEN_STRTOK );

    while( keywordTokenizer.HasMoreTokens() )
        m_searchTerms.emplace_back( SEARCH_TERM( keywordTokenizer.GetNextToken(), 4 ) );

    m_searchTerms.emplace_back( SEARCH_TERM( keywords, 1 ) );
    m_searchTerms.emplace_back( SEARCH_TERM( m_desc, 1 ) );
}


#ifdef __EMSCRIPTEN__

std::shared_ptr<const std::vector<PCBJAM_FOOTPRINT_ROW>>
FOOTPRINT_LIBRARY_ADAPTER::GetIndexRows( const wxString& aNickname )
{
    // Already parsed: the real footprints are just as fast.
    {
        std::shared_lock lock( PreloadedFootprintsMutex );

        if( PreloadedFootprints.Get().count( aNickname ) )
            return nullptr;
    }

    std::optional<LIB_DATA*> lib = fetchIfLoaded( aNickname );

    if( !lib )
        return nullptr;

    auto* plugin = dynamic_cast<PCB_IO_PCBJAM_FP*>( pcbplugin( *lib ) );
    return plugin ? plugin->IndexRows( getUri( ( *lib )->row ) ) : nullptr;
}


bool FOOTPRINT_LIBRARY_ADAPTER::GetIndexedTreeItems( const wxString& aNickname, std::vector<LIB_TREE_ITEM*>& aItems,
                                                     std::vector<std::unique_ptr<LIB_TREE_ITEM>>& aStorage )
{
    std::shared_ptr<const std::vector<PCBJAM_FOOTPRINT_ROW>> rows = GetIndexRows( aNickname );

    if( !rows )
        return false;

    std::optional<LIB_DATA*> lib = fetchIfLoaded( aNickname );
    const wxString           nickname = lib ? ( *lib )->row->Nickname() : aNickname;

    for( const PCBJAM_FOOTPRINT_ROW& row : *rows )
    {
        aStorage.push_back( std::make_unique<PCBJAM_FOOTPRINT_STUB>( row, nickname ) );
        aItems.push_back( aStorage.back().get() );
    }

    return true;
}


void FOOTPRINT_LIBRARY_ADAPTER::PrefetchIndex( const std::vector<wxString>& aNicknames )
{
    std::map<std::string, wxString> wanted;   // uri -> nickname
    nlohmann::json                  uris = nlohmann::json::array();

    for( const wxString& nickname : aNicknames )
    {
        {
            std::shared_lock lock( PreloadedFootprintsMutex );

            if( PreloadedFootprints.Get().count( nickname ) )
                continue;
        }

        std::optional<LIB_DATA*> lib = fetchIfLoaded( nickname );

        if( !lib )
            continue;

        auto*    plugin = dynamic_cast<PCB_IO_PCBJAM_FP*>( pcbplugin( *lib ) );
        wxString uri = getUri( ( *lib )->row );

        if( !plugin || plugin->HasIndexAnswer( uri ) )
            continue;

        std::string key( uri.utf8_str() );
        wanted[key] = nickname;
        uris.push_back( key );
    }

    if( wanted.empty() )
        return;

    // A JSON URI list tells the provider this is the tree's batch, not the Add
    // Symbol footprint list (libs 0029) that shares the "index" op.
    std::optional<std::string> batch = PCB_IO_PCBJAM_FP::BridgeRequest(
            "index", wxS( "/mnt/pcbjam/" ), wxString::FromUTF8( uris.dump() ) );

    if( !batch )
        return;

    std::map<std::string, std::string> frames = SplitFootprintIndexBatch( *batch );

    // The crossing suspended: look every library up again before storing.
    for( auto& [uri, nickname] : wanted )
    {
        auto it = frames.find( uri );

        if( it == frames.end() )
            continue;

        if( std::optional<LIB_DATA*> lib = fetchIfLoaded( nickname ) )
        {
            if( auto* plugin = dynamic_cast<PCB_IO_PCBJAM_FP*>( pcbplugin( *lib ) ) )
                plugin->SetIndexAnswer( wxString::FromUTF8( uri ), it->second );
        }
    }

    KI_TRACE( wxT( "KI_TRACE_FP_CHOOSER" ), wxT( "PrefetchIndex libs=%zu answered=%zu bytes=%zu\n" ),
              wanted.size(), frames.size(), batch->size() );
}


std::string FOOTPRINT_LIBRARY_ADAPTER::CompareIndexedTreeItems( const wxString& aNickname )
{
    // Golden check (libs 0028 phase 9): the stubs against the real footprints of
    // the same library. Loads the library for real, so debug sessions only.
    nlohmann::json out = { { "lib", std::string( aNickname.utf8_str() ) } };
    nlohmann::json mismatches = nlohmann::json::array();

    std::vector<LIB_TREE_ITEM*>                 stubs;
    std::vector<std::unique_ptr<LIB_TREE_ITEM>> storage;

    if( !GetIndexedTreeItems( aNickname, stubs, storage ) )
    {
        out["error"] = "no index for this library (or already loaded)";
        return out.dump();
    }

    std::map<wxString, LIB_TREE_ITEM*> realByName;

    for( FOOTPRINT* fp : GetFootprints( aNickname, true ) )
        realByName[fp->GetName()] = fp;

    auto u8 = []( const wxString& s ) { return std::string( s.utf8_str() ); };
    auto describe = [&]( LIB_TREE_ITEM* aItem )
    {
        nlohmann::json terms = nlohmann::json::array();

        for( const SEARCH_TERM& t : aItem->GetSearchTerms() )
            terms.push_back( { u8( t.Text ), t.Score, t.IsName } );

        std::map<wxString, wxString> fieldMap;
        aItem->GetChooserFields( fieldMap );

        return nlohmann::json{ { "name", u8( aItem->GetName() ) },
                               { "nickname", u8( aItem->GetLibNickname() ) },
                               { "desc", u8( aItem->GetDesc() ) },
                               { "footprint", u8( aItem->GetFootprint() ) },
                               { "root", aItem->IsRoot() },
                               { "pins", aItem->GetPinCount() },
                               { "fields", fieldMap.size() },
                               { "terms", terms } };
    };

    for( LIB_TREE_ITEM* stub : stubs )
    {
        auto it = realByName.find( stub->GetName() );

        if( it == realByName.end() )
        {
            mismatches.push_back( { { "name", u8( stub->GetName() ) }, { "error", "no real footprint" } } );
            continue;
        }

        nlohmann::json a = describe( stub );
        nlohmann::json b = describe( it->second );

        if( a != b )
            mismatches.push_back( { { "stub", a }, { "real", b } } );

        realByName.erase( it );
    }

    for( const auto& [name, real] : realByName )
        mismatches.push_back( { { "name", u8( name ) }, { "error", "missing from the index" } } );

    out["footprints"] = stubs.size();
    out["mismatches"] = mismatches;
    return out.dump();
}

#endif
