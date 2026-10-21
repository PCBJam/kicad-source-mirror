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

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <set>

#include <nlohmann/json.hpp>
#include <wx/dialog.h>
#include <wx/evtloop.h>
#include <wx/timer.h>
#include <wx/tokenzr.h>
#include <wx/toplevel.h>

#include <lib_symbol.h>
#include <string_utils.h>
#include <template_fieldnames.h>
#include <trace_helpers.h>

#include <libraries/library_manager.h>
#include <libraries/symbol_library_adapter.h>
#include <sch_edit_frame.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <pgm_base.h>
#include <project_sch.h>
#include <settings/settings_manager.h>
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
                          std::string* aToken, long* aFrom, long* aTotal )
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

    auto headerNumber = [&]( const char* aKey ) -> long
    {
        size_t at = header.find( aKey );
        return at == std::string::npos ? -1 : toInt( header.substr( at + strlen( aKey ) ), -1 );
    };

    if( aFrom )
        *aFrom = headerNumber( "\tfrom=" );

    if( aTotal )
        *aTotal = headerNumber( "\ttotal=" );

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
    // text variables never get here (the real symbol is used for those). Most
    // text holds no escape at all: skip the per-character pass for it.
    auto shown = []( const std::string& aRaw )
    {
        wxString text = wxString::FromUTF8( aRaw );
        return aRaw.find( '{' ) == std::string::npos ? text : UnescapeString( text );
    };

    // Names every stub uses: resolved once, not per symbol.
    static const wxString refName = GetCanonicalFieldName( FIELD_T::REFERENCE );
    static const wxString valueName = GetCanonicalFieldName( FIELD_T::VALUE );
    static const wxString footprintName = GetCanonicalFieldName( FIELD_T::FOOTPRINT );
    static const wxString datasheetName = GetCanonicalFieldName( FIELD_T::DATASHEET );
    static const wxString descriptionName = GetCanonicalFieldName( FIELD_T::DESCRIPTION );
    static const wxString localizedKeywords = _( "Keywords" );

    // LIB_SYMBOL caches these at parse time, before a derived symbol is linked:
    // the description and keywords are the symbol's own, with no parent fallback.
    m_desc = shown( aRow.description );
    m_footprint = shown( aRow.footprint );

    wxString keywords = wxString::FromUTF8( aRow.keywords );

    // Chooser fields, as LIB_SYMBOL::cacheChooserFields: every field by name with
    // its shown text (the mandatory five always exist), then "Keywords" unless a
    // field already has that name.
    m_fields[refName] = shown( aRow.reference );
    m_fields[valueName] = shown( aRow.value );
    m_fields[footprintName] = m_footprint;
    m_fields[datasheetName] = shown( aRow.datasheet );
    m_fields[descriptionName] = m_desc;

    for( const auto& [name, value] : aRow.fields )
        m_fields[wxString::FromUTF8( name )] = shown( value );

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


int SYMBOL_LIBRARY_ADAPTER::WarmIndex( const wxString& aNickname, const wxString& aSymbolName )
{
    std::optional<LIB_DATA*> lib = fetchIfLoaded( aNickname );

    if( !lib )
        return 0;

    auto*    plugin = dynamic_cast<SCH_IO_PCBJAM_LIB*>( schplugin( *lib ) );
    wxString uri = getUri( ( *lib )->row );

    if( !plugin || plugin->IsFatLoaded( uri ) )
        return 0;

    // One named symbol (a placed one: the chooser's "Already Placed" group loads
    // each), through the static bridge like every step.
    if( !aSymbolName.IsEmpty() )
    {
        if( plugin->IsCached( uri, aSymbolName ) )
            return 0;

        std::optional<std::string> body = SCH_IO_PCBJAM_LIB::BridgeRequest( "get", uri, aSymbolName );

        lib = fetchIfLoaded( aNickname );
        plugin = lib ? dynamic_cast<SCH_IO_PCBJAM_LIB*>( schplugin( *lib ) ) : nullptr;

        if( plugin && body )
            plugin->CacheItemDocument( uri, *body );

        return 0;
    }

    // Rows in: load the real symbols the chooser lists for text-variable rows,
    // one per step, so the click needs no "get" for them either.
    if( plugin->HasIndexAnswer( uri ) )
    {
        const wxString name = plugin->NextUncachedTextVarSymbol( uri );

        if( name.IsEmpty() )
            return 0;

        std::optional<std::string> body = SCH_IO_PCBJAM_LIB::BridgeRequest( "get", uri, name );

        lib = fetchIfLoaded( aNickname );
        plugin = lib ? dynamic_cast<SCH_IO_PCBJAM_LIB*>( schplugin( *lib ) ) : nullptr;

        if( !plugin || !body )
            return 0;   // dropped (re-queued by the invalidation), or gone: the click loads it

        plugin->CacheItemDocument( uri, *body );
        return plugin->NextUncachedTextVarSymbol( uri ) == name ? 0 : 1;
    }

    const size_t cursor = plugin->WarmCursor( uri );

    // The crossing goes through the static bridge, not the plugin: while it is
    // parked the library can be invalidated, which deletes this LIB_DATA and its
    // plugin (pcbjam_libs::invalidateLibrary). Look both up again afterwards.
    std::optional<std::string> page = SCH_IO_PCBJAM_LIB::BridgeRequest(
            "list", uri, wxString::Format( wxS( "index:%zu" ), cursor ) );

    lib = fetchIfLoaded( aNickname );
    plugin = lib ? dynamic_cast<SCH_IO_PCBJAM_LIB*>( schplugin( *lib ) ) : nullptr;

    if( !plugin )
        return 0;   // dropped: the invalidation re-queues it

    if( page && *page == "~" )
        return 2;

    switch( plugin->AddIndexPage( uri, page ? *page : std::string() ) )
    {
    case SCH_IO_PCBJAM_LIB::WARM_PAGE::MORE:
    case SCH_IO_PCBJAM_LIB::WARM_PAGE::STALE:
        return 1;

    case SCH_IO_PCBJAM_LIB::WARM_PAGE::COMPLETE:
    default:
        // Rows in; text-variable symbols (if any) follow in the next steps.
        return plugin->NextUncachedTextVarSymbol( uri ).IsEmpty() ? 0 : 1;
    }
}

#endif


// ---------------------------------------------------------------- background warm-up

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>

// Defined in the wasm bindings (kicad_editor_embind.cpp) and pcbnew.cpp; weak so
// an image without them still links.
extern "C" int  pcbjam_progress_may_yield() __attribute__(( weak ));
extern "C" void pcbjam_fp_index_warm() __attribute__(( weak ));

namespace
{
/// A modal dialog is up. The wasm port has no nested event loops (a modal
/// JSPI-suspends its opener), so this is checked on the dialogs themselves.
bool modalShown()
{
    for( wxWindow* window : wxTopLevelWindows )
    {
        if( auto* dialog = dynamic_cast<wxDialog*>( window ); dialog && dialog->IsModal() )
            return true;
    }

    return false;
}


/**
 * libs 0028 section 8.3: after boot settles, pull the library index into the
 * plugins one short step at a time, so Add Symbol finds every library's rows in
 * memory and makes no crossing. Each step is one wx timer callback (a fresh
 * browser task on the mailbox lane): one page (up to 256 rows) of one library.
 * It pauses while an open is in flight, a modal is up or a long operation
 * yields, and stops for good above 75 % of the maximum heap. Clicking Add Symbol
 * before it has finished is fine: the chooser fetches the rest in one batch.
 */
class INDEX_WARMUP : public wxTimer
{
public:
    void Begin()
    {
        if( m_started )
            return;

        m_started = true;
        m_t0 = std::chrono::steady_clock::now();

        for( const LIBRARY_TABLE_ROW* row : Pgm().GetLibraryManager().Rows( LIBRARY_TABLE_TYPE::SYMBOL ) )
        {
            if( row->Disabled() || row->Hidden() )
                continue;

            if( LIBRARY_MANAGER::GetFullURI( row, true ).StartsWith( wxS( "/mnt/pcbjam/" ) ) )
                m_queue.push_back( row->Nickname() );
        }

        KI_TRACE( wxT( "KI_TRACE_SYM_CHOOSER" ), wxT( "warmup:start libs=%zu\n" ), m_queue.size() );
        arm( 1 );
    }

    void Requeue( const wxString& aNickname )
    {
        if( !m_started || m_stopped )
            return;

        if( std::find( m_queue.begin(), m_queue.end(), aNickname ) == m_queue.end() )
            m_queue.push_back( aNickname );

        if( !m_inStep && !IsRunning() )
            arm( 1 );
    }

    void Notify() override
    {
        if( m_inStep || m_stopped )
            return;

        wxEventLoopBase* loop = wxEventLoopBase::GetActive();

        if( ( pcbjam_progress_may_yield && !pcbjam_progress_may_yield() ) || modalShown()
                || ( loop && loop->IsYielding() ) )
        {
            arm( 250 );
            return;
        }

        if( emscripten_get_heap_size() > emscripten_get_heap_max() / 4 * 3 )
        {
            KI_TRACE( wxT( "KI_TRACE_SYM_CHOOSER" ), wxT( "warmup:stopped heap=%zu\n" ),
                      emscripten_get_heap_size() );
            m_stopped = true;
            return;
        }

        SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &Pgm().GetSettingsManager().Prj() );

        // After the libraries: the symbols placed in the open schematic, one per
        // step, so the chooser's "Already Placed" scan finds them cached.
        if( m_queue.empty() && !m_placedListed )
            listPlaced();

        if( m_queue.empty() && !m_placed.empty() )
        {
            auto [nick, name] = m_placed.front();
            m_placed.pop_front();
            m_inStep = true;
            auto t0 = std::chrono::steady_clock::now();

            if( adapter )
                adapter->WarmIndex( nick, name );

            m_inStep = false;
            double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
            m_steps++;
            KI_TRACE( wxT( "KI_TRACE_SYM_CHOOSER" ), wxT( "warmup:placed lib=%s name=%s ms=%.1f\n" ), nick, name, ms );
            arm( ms < 4.0 ? 1 : 16 );
            return;
        }

        if( m_queue.empty() )
        {
            finish();
            return;
        }

        const wxString nickname = m_queue.front();
        m_queue.pop_front();

        auto t0 = std::chrono::steady_clock::now();
        m_inStep = true;
        int result = adapter ? adapter->WarmIndex( nickname ) : 0;
        m_inStep = false;
        double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();

        m_steps++;
        KI_TRACE( wxT( "KI_TRACE_SYM_CHOOSER" ), wxT( "warmup:step lib=%s result=%d ms=%.1f\n" ),
                  nickname, result, ms );

        if( result == 1 )
        {
            m_queue.push_front( nickname );   // its next page
            m_notReady = 0;
        }
        else if( result == 2 )
        {
            // Its rows aren't ready on the JS side yet: try it again later, and
            // wait a little once every queued library has said so.
            m_queue.push_back( nickname );

            if( ++m_notReady >= m_queue.size() )
            {
                m_notReady = 0;
                arm( 250 );
                return;
            }
        }
        else
        {
            m_notReady = 0;
        }

        // Back to back while steps are short, one per frame otherwise.
        arm( ms < 4.0 ? 1 : 16 );
    }

private:
    void arm( int aMs ) { StartOnce( aMs ); }

    /// The unique library symbols placed in the open schematic (once per warm-up).
    void listPlaced()
    {
        m_placedListed = true;
        auto* frame = wxTheApp ? dynamic_cast<SCH_EDIT_FRAME*>( wxTheApp->GetTopWindow() ) : nullptr;

        if( !frame )
            return;

        std::set<wxString> seen;

        for( const SCH_SHEET_PATH& sheet : frame->Schematic().Hierarchy() )
        {
            for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            {
                const LIB_ID& id = static_cast<SCH_SYMBOL*>( item )->GetLibId();

                if( id.IsValid() && seen.insert( id.Format().wx_str() ).second )
                    m_placed.emplace_back( id.GetLibNickname().wx_str(), id.GetLibItemName().wx_str() );
            }
        }
    }

    void finish()
    {
        if( !m_fpDone && pcbjam_fp_index_warm )
        {
            m_fpDone = true;
            m_inStep = true;
            pcbjam_fp_index_warm();
            m_inStep = false;
        }

        KI_TRACE( wxT( "KI_TRACE_SYM_CHOOSER" ), wxT( "warmup:done steps=%zu total_ms=%.0f\n" ), m_steps,
                  std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - m_t0 ).count() );
    }

    std::deque<wxString>                  m_queue;
    std::chrono::steady_clock::time_point m_t0;
    size_t                                m_steps = 0;
    size_t                                m_notReady = 0;
    bool                                  m_started = false;
    bool                                  m_stopped = false;
    bool                                  m_inStep = false;
    bool                                  m_fpDone = false;
    bool                                  m_placedListed = false;
    std::deque<std::pair<wxString, wxString>> m_placed;
};


INDEX_WARMUP* s_warmup = nullptr;
} // namespace


/// Called by the kicadLibIndexWarmup embind export (wasm bindings) once boot settles.
extern "C" void pcbjam_lib_index_warmup_start( const char* aKind )
{
    if( std::strcmp( aKind, "symbol" ) != 0 )
        return;

    if( !s_warmup )
        s_warmup = new INDEX_WARMUP();

    s_warmup->Begin();
}


/// Called after a library is invalidated (pcbjam_libs::invalidateLibrary): pull its rows again.
extern "C" void pcbjam_lib_index_requeue( const char* aKind, const char* aNickname )
{
    if( s_warmup && std::strcmp( aKind, "symbol" ) == 0 )
        s_warmup->Requeue( wxString::FromUTF8( aNickname ) );
}

#endif
