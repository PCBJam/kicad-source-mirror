// pcbjam WASM addition — see pcbjam_remote_lock.h.

#include <pcbjam_remote_lock.h>

namespace PCBJAM_REMOTE_LOCK
{

static QUERY g_query;

void SetQuery( QUERY aQuery )
{
    g_query = std::move( aQuery );
}

bool IsLocked( const KIID& aId, wxString* aHolder )
{
    return g_query && g_query( aId, aHolder );
}

} // namespace PCBJAM_REMOTE_LOCK
