/*
 * pcbjam WASM addition: ephemeral remote "soft locks" for collaborative
 * editing (collab-presence 0007).
 *
 * While a remote peer has an item selected, local edit tools treat it like a
 * locked item: still selectable for inspection, but filtered from move/drag/
 * rotate/delete acquisition. The lock set is fed by the wasm collab bridge
 * from the peers' live selections (Yjs awareness) — it is NEVER serialized,
 * never touches EDA_ITEM state, and is not affected by "Override locks".
 *
 * Native builds compile this too, but without an installed query IsLocked()
 * is a cheap constant false — zero behavioral change outside the wasm app.
 */

#ifndef PCBJAM_REMOTE_LOCK_H
#define PCBJAM_REMOTE_LOCK_H

#include <functional>

#include <kicommon.h>

class KIID;
class wxString;

namespace PCBJAM_REMOTE_LOCK
{

/** Returns true when the item is held by a remote peer; optionally reports
 *  the peer's display name for user feedback. */
using QUERY = std::function<bool( const KIID& aId, wxString* aHolder )>;

// KICOMMON_API: defined in libkicommon (shared module on wasm); emcc 6 only
// exports annotated symbols from a side module.

/** Install (or clear, with nullptr) the process-global lock query. */
KICOMMON_API void SetQuery( QUERY aQuery );

/** True when a query is installed and reports the item as remotely held. */
KICOMMON_API bool IsLocked( const KIID& aId, wxString* aHolder = nullptr );

} // namespace PCBJAM_REMOTE_LOCK

#endif // PCBJAM_REMOTE_LOCK_H
