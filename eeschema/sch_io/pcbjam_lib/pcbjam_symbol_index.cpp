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

#include <set>

#include <nlohmann/json.hpp>
#include <wx/tokenzr.h>

#include <lib_symbol.h>
#include <string_utils.h>
#include <template_fieldnames.h>
#include <trace_helpers.h>

#include <libraries/symbol_library_adapter.h>
#include <sch_io/pcbjam_lib/pcbjam_symbol_index.h>
#include <sch_io/pcbjam_lib/sch_io_pcbjam_lib.h>


// ---------------------------------------------------------------- TSV

namespace
{
/// `\\`, `\t`, `\n` (and in key=value pairs `\=`, `\u`) back to their characters.
std::string unescapeTsv( const std::string& aIn )
{
    if( aIn.find( '\\' ) == std::string::npos )
        return aIn;

    std::string out;
    out.reserve( aIn.size() );

    for( size_t i = 0; i < aIn.size(); ++i )
    {
        char c = aIn[i];

        if( c != '\\' || i + 1 >= aIn.size() )
        {
            out += c;
            continue;
        }

        char e = aIn[++i];

        switch( e )
        {
        case 't': out += '\t'; break;
        case 'n': out += '\n'; break;
        case 'u': out += '\x1f'; break;
        default:  out += e; break;
        }
    }

    return out;
}


std::vector<std::string> split( const std::string& aIn, char aSep )
{
    std::vector<std::string> out;
    size_t                   start = 0;

    for( size_t i = 0; i <= aIn.size(); ++i )
    {
        if( i == aIn.size() || aIn[i] == aSep )
        {
            out.emplace_back( aIn, start, i - start );
            start = i + 1;
        }
    }

    return out;
}


/// `key=value` pairs joined by 0x1f; the first unescaped `=` splits a pair.
std::vector<std::pair<std::string, std::string>> parsePairs( const std::string& aIn )
{
    std::vector<std::pair<std::string, std::string>> out;

    if( aIn.empty() )
        return out;

    for( const std::string& pair : split( aIn, '\x1f' ) )
    {
        size_t eq = 0;

        for( ; eq < pair.size(); ++eq )
        {
            if( pair[eq] == '\\' )
                ++eq;
            else if( pair[eq] == '=' )
                break;
        }

        out.emplace_back( unescapeTsv( pair.substr( 0, eq ) ),
                          eq < pair.size() ? unescapeTsv( pair.substr( eq + 1 ) ) : std::string() );
    }

    return out;
}


int toInt( const std::string& aIn, int aDefault )
{
    if( aIn.empty() )
        return aDefault;

    try
    {
        return std::stoi( aIn );
    }
    catch( ... )
    {
        return aDefault;
    }
}
} // namespace


bool ParseSymbolIndexTsv( const std::string& aTsv, std::vector<PCBJAM_SYMBOL_ROW>& aRows,
                          std::string* aToken )
{
    size_t nl = aTsv.find( '\n' );
    std::string header = aTsv.substr( 0, nl );

    if( header.rfind( "v=", 0 ) != 0 || header.find( "\tschema=1\t" ) == std::string::npos
            || header.find( "\tkind=symbol" ) == std::string::npos )
    {
        return false;
    }

    if( aToken )
        *aToken = unescapeTsv( header.substr( 2, header.find( '\t' ) - 2 ) );

    if( nl == std::string::npos )
        return true;

    size_t start = nl + 1;

    while( start < aTsv.size() )
    {
        size_t end = aTsv.find( '\n', start );

        if( end == std::string::npos )
            end = aTsv.size();

        std::vector<std::string> c = split( aTsv.substr( start, end - start ), '\t' );
        start = end + 1;

        if( c.size() < 16 )
            continue;

        PCBJAM_SYMBOL_ROW& row = aRows.emplace_back();
        row.name = unescapeTsv( c[0] );
        row.extends = unescapeTsv( c[1] );
        row.power = toInt( c[2], 0 );
        row.units = toInt( c[3], 1 );
        row.graphicalPins = toInt( c[4], 0 );
        row.pinCount = toInt( c[5], 0 );
        row.textVars = !c[6].empty();
        row.description = unescapeTsv( c[7] );
        row.keywords = unescapeTsv( c[8] );
        row.fpFilters = unescapeTsv( c[9] );
        row.footprint = unescapeTsv( c[10] );
        row.reference = unescapeTsv( c[11] );
        row.value = unescapeTsv( c[12] );
        row.datasheet = unescapeTsv( c[13] );

        for( auto& [unit, name] : parsePairs( c[14] ) )
            row.unitNames.emplace_back( toInt( unit, 0 ), name );

        row.fields = parsePairs( c[15] );
    }

    return true;
}


std::map<std::string, std::string> SplitSymbolIndexBatch( const std::string& aBatch )
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


// ---------------------------------------------------------------- the stub

PCBJAM_SYMBOL_STUB::PCBJAM_SYMBOL_STUB( const PCBJAM_SYMBOL_ROW& aRow, const wxString& aNickname,
                                        bool aRoot ) :
        m_name( wxString::FromUTF8( aRow.name ) ),
        m_nickname( aNickname ),
        m_isRoot( aRoot ),
        m_isPower( aRow.power != 0 ),
        // LIB_SYMBOL caches its pin count when parsed, before a derived symbol is
        // linked to its parent: a derived symbol owns no pins, so it counts 0.
        m_pinCount( aRow.extends.empty() ? aRow.pinCount : 0 ),
        m_units( std::max( aRow.units, 1 ) )
{
    m_libId.SetLibNickname( aNickname );
    m_libId.SetLibItemName( m_name );

    // Shown text: KiCad's EDA_TEXT unescapes {slash}-style escapes. Rows with
    // text variables never get here (the real symbol is used for those).
    auto shown = []( const std::string& aRaw ) { return UnescapeString( wxString::FromUTF8( aRaw ) ); };

    // LIB_SYMBOL caches these at parse time, before a derived symbol is linked:
    // the description and keywords are the symbol's own, with no parent fallback.
    m_desc = shown( aRow.description );
    m_footprint = shown( aRow.footprint );

    wxString keywords = wxString::FromUTF8( aRow.keywords );

    // Chooser fields, as LIB_SYMBOL::cacheChooserFields: every field by name with
    // its shown text (the mandatory five always exist), then "Keywords" unless a
    // field already has that name.
    m_fields[GetCanonicalFieldName( FIELD_T::REFERENCE )] = shown( aRow.reference );
    m_fields[GetCanonicalFieldName( FIELD_T::VALUE )] = shown( aRow.value );
    m_fields[GetCanonicalFieldName( FIELD_T::FOOTPRINT )] = m_footprint;
    m_fields[GetCanonicalFieldName( FIELD_T::DATASHEET )] = shown( aRow.datasheet );
    m_fields[GetCanonicalFieldName( FIELD_T::DESCRIPTION )] = m_desc;

    for( const auto& [name, value] : aRow.fields )
        m_fields[wxString::FromUTF8( name )] = shown( value );

    const wxString localizedKeywords = _( "Keywords" );

    if( !m_fields.contains( localizedKeywords ) )
        m_fields[localizedKeywords] = keywords;

    // Search terms, as LIB_SYMBOL::cacheSearchTerms, in the same order and weights.
    // Term 0 is the nickname, which is still empty when KiCad caches the terms at
    // parse time; the adapter's SetLibId later fixes only the LIB_ID term.
    m_searchTerms.reserve( 6 );
    m_searchTerms.emplace_back( SEARCH_TERM( wxEmptyString, 4 ) );
    m_searchTerms.emplace_back( SEARCH_TERM( m_name, 8, true ) );
    m_searchTerms.emplace_back( SEARCH_TERM( m_libId.Format().wx_str(), 16, true ) );

    wxStringTokenizer keywordTokenizer( keywords, " \t\r\n", wxTOKEN_STRTOK );

    while( keywordTokenizer.HasMoreTokens() )
        m_searchTerms.emplace_back( SEARCH_TERM( keywordTokenizer.GetNextToken(), 4 ) );

    m_searchTerms.emplace_back( SEARCH_TERM( keywords, 1 ) );
    m_searchTerms.emplace_back( SEARCH_TERM( m_desc, 1 ) );

    if( !m_footprint.IsEmpty() )
        m_searchTerms.emplace_back( SEARCH_TERM( m_footprint, 1 ) );

    for( const auto& [unit, name] : aRow.unitNames )
        m_unitNames[unit] = wxString::FromUTF8( name );
}


wxString PCBJAM_SYMBOL_STUB::GetUnitName( int aUnit ) const
{
    // LIB_SYMBOL::GetUnitDisplayName( aUnit, true ), with the root's unit names.
    if( auto it = m_unitNames.find( aUnit ); it != m_unitNames.end() )
        return it->second;

    return wxString::Format( _( "Unit %s" ), LIB_SYMBOL::LetterSubReference( aUnit, 'A' ) );
}


// ---------------------------------------------------------------- adapter members

#ifdef __EMSCRIPTEN__

void SYMBOL_LIBRARY_ADAPTER::PrefetchIndex( const std::vector<wxString>& aNicknames )
{
    // One crossing for every library that is loaded, ours, not fat-loaded and
    // without index rows yet; the answers go to each library's own plugin.
    std::map<std::string, SCH_IO_PCBJAM_LIB*> wanted;
    nlohmann::json                            uris = nlohmann::json::array();

    for( const wxString& nickname : aNicknames )
    {
        std::optional<LIB_DATA*> lib = fetchIfLoaded( nickname );

        if( !lib )
            continue;

        auto* plugin = dynamic_cast<SCH_IO_PCBJAM_LIB*>( schplugin( *lib ) );
        wxString uri = getUri( ( *lib )->row );

        if( !plugin || plugin->IsFatLoaded( uri ) || plugin->HasIndexAnswer( uri ) )
            continue;

        std::string key( uri.utf8_str() );
        wanted[key] = plugin;
        uris.push_back( key );
    }

    if( wanted.empty() )
        return;

    std::optional<std::string> batch = SCH_IO_PCBJAM_LIB::BridgeRequest(
            "index", wxS( "/mnt/pcbjam/" ), wxString::FromUTF8( uris.dump() ) );

    if( !batch )
        return; // no provider support: each library asks (or falls back) on its own

    std::map<std::string, std::string> frames = SplitSymbolIndexBatch( *batch );

    for( auto& [uri, plugin] : wanted )
    {
        auto it = frames.find( uri );

        if( it != frames.end() )
            plugin->SetIndexAnswer( wxString::FromUTF8( uri ), it->second );
    }

    KI_TRACE( wxT( "KI_TRACE_SYM_CHOOSER" ), wxT( "PrefetchIndex libs=%zu answered=%zu bytes=%zu\n" ),
              wanted.size(), frames.size(), batch->size() );
}


bool SYMBOL_LIBRARY_ADAPTER::GetIndexedTreeItems( const wxString& aNickname, bool aPowerOnly,
                                                  std::vector<LIB_TREE_ITEM*>& aItems,
                                                  std::vector<std::unique_ptr<LIB_TREE_ITEM>>& aStorage )
{
    std::optional<LIB_DATA*> lib = fetchIfLoaded( aNickname );

    if( !lib )
        return false;

    auto* plugin = dynamic_cast<SCH_IO_PCBJAM_LIB*>( schplugin( *lib ) );
    wxString uri = getUri( ( *lib )->row );

    // A fat-loaded library's real symbols are just as fast.
    if( !plugin || plugin->IsFatLoaded( uri ) )
        return false;

    std::lock_guard pluginGuard( pluginMutex( aNickname ) );

    std::shared_ptr<const std::vector<PCBJAM_SYMBOL_ROW>> rows = plugin->IndexRows( uri );

    if( !rows )
        return false;

    std::set<std::string> names;

    for( const PCBJAM_SYMBOL_ROW& row : *rows )
        names.insert( row.name );

    const wxString nickname = ( *lib )->row->Nickname();

    for( const PCBJAM_SYMBOL_ROW& row : *rows )
    {
        if( aPowerOnly && !row.power )
            continue;

        if( row.textVars )
        {
            // KiCad must resolve the text variables itself: list the real symbol
            // (one per-item load), exactly as GetSymbols would hand it over.
            if( LIB_SYMBOL* real = plugin->LoadMaster( uri, wxString::FromUTF8( row.name ) ) )
            {
                LIB_ID id = real->GetLibId();
                id.SetLibNickname( nickname );
                real->SetLibId( id );
                aItems.push_back( real );
            }

            continue;
        }

        bool root = row.extends.empty() || !names.count( row.extends );
        aStorage.push_back( std::make_unique<PCBJAM_SYMBOL_STUB>( row, nickname, root ) );
        aItems.push_back( aStorage.back().get() );
    }

    return true;
}


std::string SYMBOL_LIBRARY_ADAPTER::CompareIndexedTreeItems( const wxString& aNickname )
{
    // Golden check (libs 0028 phase 5): the stubs against the real symbols of the
    // same library. Fat-loads the library, so run it in a debug session only.
    nlohmann::json out = { { "lib", std::string( aNickname.utf8_str() ) } };
    nlohmann::json mismatches = nlohmann::json::array();

    std::vector<LIB_TREE_ITEM*>                 stubs;
    std::vector<std::unique_ptr<LIB_TREE_ITEM>> storage;

    if( !GetIndexedTreeItems( aNickname, false, stubs, storage ) )
    {
        out["error"] = "no index for this library (or already fat-loaded)";
        return out.dump();
    }

    std::vector<wxString> stubFields;
    std::optional<LIB_DATA*> lib = fetchIfLoaded( aNickname );
    auto* plugin = lib ? dynamic_cast<SCH_IO_PCBJAM_LIB*>( schplugin( *lib ) ) : nullptr;

    if( plugin )
        plugin->GetAvailableSymbolFields( stubFields );

    std::vector<LIB_SYMBOL*> reals = GetSymbols( aNickname );
    std::map<wxString, LIB_TREE_ITEM*> realByName;

    for( LIB_SYMBOL* real : reals )
        realByName[real->GetName()] = real;

    auto u8 = []( const wxString& s ) { return std::string( s.utf8_str() ); };
    auto terms = [&]( std::vector<SEARCH_TERM>& aTerms )
    {
        nlohmann::json arr = nlohmann::json::array();

        for( const SEARCH_TERM& t : aTerms )
            arr.push_back( { u8( t.Text ), t.Score, t.IsName } );

        return arr;
    };
    auto fields = [&]( LIB_TREE_ITEM* aItem )
    {
        std::map<wxString, wxString> map;
        aItem->GetChooserFields( map );
        nlohmann::json obj = nlohmann::json::object();

        for( const auto& [k, v] : map )
            obj[u8( k )] = u8( v );

        return obj;
    };
    auto describe = [&]( LIB_TREE_ITEM* aItem )
    {
        nlohmann::json units = nlohmann::json::array();

        for( int u = 1; aItem->GetSubUnitCount() > 1 && u <= aItem->GetSubUnitCount(); ++u )
            units.push_back( u8( aItem->GetUnitName( u ) ) );

        return nlohmann::json{ { "name", u8( aItem->GetName() ) },
                               { "nickname", u8( aItem->GetLibNickname() ) },
                               { "desc", u8( aItem->GetDesc() ) },
                               { "footprint", u8( aItem->GetFootprint() ) },
                               { "root", aItem->IsRoot() },
                               { "power", aItem->IsPowerSymbol() },
                               { "units", aItem->GetSubUnitCount() },
                               { "unitNames", units },
                               { "fields", fields( aItem ) },
                               { "terms", terms( aItem->GetSearchTerms() ) } };
    };

    for( LIB_TREE_ITEM* stub : stubs )
    {
        auto it = realByName.find( stub->GetName() );

        if( it == realByName.end() )
        {
            mismatches.push_back( { { "name", u8( stub->GetName() ) }, { "error", "no real symbol" } } );
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

    std::vector<wxString> realFields;

    if( plugin )
        plugin->GetAvailableSymbolFields( realFields );

    nlohmann::json sf = nlohmann::json::array(), rf = nlohmann::json::array();

    for( const wxString& f : stubFields )
        sf.push_back( u8( f ) );

    for( const wxString& f : realFields )
        rf.push_back( u8( f ) );

    if( sf != rf )
        mismatches.push_back( { { "availableFields", { { "index", sf }, { "real", rf } } } } );

    out["symbols"] = stubs.size();
    out["mismatches"] = mismatches;
    return out.dump();
}

#endif
