// pcbjam WASM addition — see pcbjam_sch_remote_lock.h.

#include <tools/pcbjam_sch_remote_lock.h>

#include <vector>

#include <eda_base_frame.h>
#include <pcbjam_remote_lock.h>
#include <sch_item.h>
#include <tool/tool_manager.h>
#include <tools/sch_selection.h>
#include <tools/sch_selection_tool.h>

namespace PCBJAM_SCH_REMOTE_LOCK
{

static bool isHeld( SCH_ITEM* aItem, wxString* aHolder )
{
    if( PCBJAM_REMOTE_LOCK::IsLocked( aItem->m_Uuid, aHolder ) )
        return true;

    if( aItem->GetParent() && PCBJAM_REMOTE_LOCK::IsLocked( aItem->GetParent()->m_Uuid, aHolder ) )
        return true;

    bool held = false;

    aItem->RunOnChildren(
            [&]( SCH_ITEM* aChild )
            {
                held = held || PCBJAM_REMOTE_LOCK::IsLocked( aChild->m_Uuid, aHolder );
            },
            RECURSE_MODE::NO_RECURSE );

    return held;
}


SCH_SELECTION& Trim( SCH_SELECTION& aSelection, SCH_SELECTION_TOOL* aTool )
{
    std::vector<EDA_ITEM*> held;
    wxString               holder;

    for( EDA_ITEM* item : aSelection )
    {
        if( item->IsSCH_ITEM() && isHeld( static_cast<SCH_ITEM*>( item ), &holder ) )
            held.push_back( item );
    }

    if( held.empty() )
        return aSelection;

    for( EDA_ITEM* item : held )
        aTool->RemoveItemFromSel( item, true );

    if( auto* frame = dynamic_cast<EDA_BASE_FRAME*>( aTool->GetManager()->GetToolHolder() ) )
    {
        frame->ShowInfoBarWarning( wxString::Format( _( "Some items are being edited by %s and "
                                                        "were skipped." ),
                                                     holder ),
                                   true );
    }

    return aSelection;
}

} // namespace PCBJAM_SCH_REMOTE_LOCK
