/*
 * pcbjam WASM addition: process-global read-only viewer mode
 * (read-only-viewer).
 *
 * While active, the tool system swallows every action except an explicit
 * allowlist of view-only actions (zoom/pan/cursor/grid/units/display) — the
 * canvas becomes a pannable, zoomable viewer. SELECTION stays live
 * (viewer-panels): viewers may click items to inspect them (the shell's
 * inspector panel reads the selection through the presence bridge), but
 * everything downstream of a selection is blocked — move/properties/delete
 * are gated actions, the point editors carry their own read-only guards
 * (their point drags mutate without actions), and the right-click CONTEXT
 * menu is skipped in the selection tools' own RMB arms (the
 * click-disambiguation clarify list stays — it is pure selection, and both
 * menu kinds are CMENU_NOW so they cannot be told apart downstream in
 * TOOL_MANAGER). Mouse wheel/drag zoom-pan
 * is untouched by construction (WX_VIEW_CONTROLS binds those on the GAL
 * panel directly, below the tool system). The collab bridge keeps working:
 * remote peer edits apply via BOARD_COMMIT, not tool actions.
 *
 * Set from the wasm bindings (kicadSetReadOnly) for sessions with read-only
 * access (e.g. anonymous viewers on public projects). Native builds compile
 * this too, but without the flag set every query is a cheap constant false —
 * zero behavioral change outside the wasm app.
 */

#ifndef PCBJAM_READ_ONLY_H
#define PCBJAM_READ_ONLY_H

#include <string>

#include <kicommon.h>

namespace PCBJAM_READ_ONLY
{

// KICOMMON_API on every function: these live in libkicommon (the shared
// module on wasm), and emcc 6 only exports annotated symbols from a side
// module — without it the editor link fails "undefined symbol".

/** Enable/disable the process-global read-only viewer mode. */
KICOMMON_API void Set( bool aReadOnly );

/** True while the read-only viewer mode is active. */
KICOMMON_API bool IsReadOnly();

/** True when the TOOL_ACTION command string names a view-only action a
 *  read-only session may still run. Exact-name matches only — prefixes are
 *  unsafe ("common.Control.save" shares the zoom prefix). Only meaningful
 *  while IsReadOnly(); callers check that first. */
KICOMMON_API bool IsActionAllowed( const std::string& aActionName );

} // namespace PCBJAM_READ_ONLY

#endif // PCBJAM_READ_ONLY_H
