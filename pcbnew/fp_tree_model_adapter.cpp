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

#include <pgm_base.h>
#include <pcb_base_frame.h>
#include <core/kicad_algo.h>
#include <settings/common_settings.h>
#include <pcbnew_settings.h>
#include <project/project_file.h>
#include <wx/tokenzr.h>
#include <string_utils.h>
#include <footprint_library_adapter.h>
#include <footprint.h>
#include <generate_footprint_info.h>

#include "fp_tree_model_adapter.h"

wxObjectDataPtr<LIB_TREE_MODEL_ADAPTER>
FP_TREE_MODEL_ADAPTER::Create( PCB_BASE_FRAME* aParent, FOOTPRINT_LIBRARY_ADAPTER* aLibs )
{
    auto* adapter = new FP_TREE_MODEL_ADAPTER( aParent, aLibs );
    return wxObjectDataPtr<LIB_TREE_MODEL_ADAPTER>( adapter );
}


FP_TREE_MODEL_ADAPTER::FP_TREE_MODEL_ADAPTER( PCB_BASE_FRAME* aParent, FOOTPRINT_LIBRARY_ADAPTER* aLibs ) :
        LIB_TREE_MODEL_ADAPTER( aParent, wxT( "pinned_footprint_libs" ),
                                aParent->GetViewerSettingsBase()->m_LibTree ),
        m_libs( aLibs )
{}


void FP_TREE_MODEL_ADAPTER::AddLibraries( EDA_BASE_FRAME* aParent )
{
    COMMON_SETTINGS* cfg = Pgm().GetCommonSettings();
    PROJECT_FILE&    project = aParent->Prj().GetProjectFile();
    std::vector<wxString> libNames = m_libs->GetLibraryNames();

#ifdef __EMSCRIPTEN__
    // pcbjam (libs 0028 phase 9): one crossing for the index of every library below.
    m_libs->PrefetchIndex( libNames );
#endif

    for( const wxString& libName : libNames )
    {
        if( !m_libs->HasLibrary( libName, true ) )
            continue;

        bool pinned = alg::contains( cfg->m_Session.pinned_fp_libs, libName )
                        || alg::contains( project.m_PinnedFootprintLibs, libName );

#ifdef __EMSCRIPTEN__
        // pcbjam (libs 0028 phase 9): list the library from its index, without parsing it.
        {
            std::vector<LIB_TREE_ITEM*>                 items;
            std::vector<std::unique_ptr<LIB_TREE_ITEM>> storage;

            if( m_libs->GetIndexedTreeItems( libName, items, storage ) )
            {
                DoAddLibrary( libName, *m_libs->GetLibraryDescription( libName ), items, pinned, true );
                continue;
            }
        }
#endif

        std::vector<FOOTPRINT*> footprints = m_libs->GetFootprints( libName, true );
        std::vector<LIB_TREE_ITEM*> treeItems;
        treeItems.reserve( footprints.size() );

        for( FOOTPRINT* fp : footprints )
            treeItems.push_back( fp );

        DoAddLibrary( libName, *m_libs->GetLibraryDescription( libName ), treeItems, pinned, true );
    }

    m_tree.AssignIntrinsicRanks( m_shownColumns );
}


wxString FP_TREE_MODEL_ADAPTER::GenerateInfo( LIB_ID const& aLibId, int aUnit )
{
    return GenerateFootprintInfo( m_libs, aLibId );
}
