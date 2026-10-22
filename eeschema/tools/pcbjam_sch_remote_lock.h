/*
 * pcbjam WASM addition: remote soft-locks for the schematic editor's edit
 * tools (collab-presence 0007, issue #47).
 *
 * pcbnew filters remote-held items inside the lock filter every edit tool
 * already calls. eeschema has no working item lock to hook into, so each
 * mutating tool wraps its RequestSelection() call in Trim(): held items are
 * dropped from the selection, the rest is edited, and the infobar names the
 * peer. Inspection (properties, copy, cross-probe) stays allowed, like
 * pcbnew.
 *
 * Without an installed lock query (native builds) Trim() changes nothing.
 */

#ifndef PCBJAM_SCH_REMOTE_LOCK_H
#define PCBJAM_SCH_REMOTE_LOCK_H

class SCH_SELECTION;
class SCH_SELECTION_TOOL;

namespace PCBJAM_SCH_REMOTE_LOCK
{

/**
 * Remove the items a remote peer holds from aSelection (the selection tool's
 * own selection, as RequestSelection() returns it). An item counts as held
 * when it, its parent (a field or pin of a held symbol) or one of its
 * children (a held field or pin of a selected symbol) is held.
 *
 * @return aSelection, so a call site can wrap RequestSelection() in place.
 */
SCH_SELECTION& Trim( SCH_SELECTION& aSelection, SCH_SELECTION_TOOL* aTool );

} // namespace PCBJAM_SCH_REMOTE_LOCK

#endif // PCBJAM_SCH_REMOTE_LOCK_H
