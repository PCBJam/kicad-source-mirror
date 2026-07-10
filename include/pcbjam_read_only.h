/*
 * pcbjam WASM addition: process-global read-only viewer mode
 * (read-only-viewer).
 *
 * While active, the tool system swallows every action except an explicit
 * allowlist of view-only actions (zoom/pan/cursor/grid/units/display), and
 * the selection tools report nothing selectable — the canvas becomes a
 * pannable, zoomable viewer. Mouse wheel/drag zoom-pan is untouched by
 * construction (WX_VIEW_CONTROLS binds those on the GAL panel directly,
 * below the tool system). The collab bridge keeps working: remote peer
 * edits apply via BOARD_COMMIT, not tool actions.
 *
 * Set from the wasm bindings (kicadSetReadOnly) for sessions with read-only
 * access (e.g. anonymous viewers on public projects). Native builds compile
 * this too, but without the flag set every query is a cheap constant false —
 * zero behavioral change outside the wasm app.
 */

#ifndef PCBJAM_READ_ONLY_H
#define PCBJAM_READ_ONLY_H

#include <string>

namespace PCBJAM_READ_ONLY
{

/** Enable/disable the process-global read-only viewer mode. */
void Set( bool aReadOnly );

/** True while the read-only viewer mode is active. */
bool IsReadOnly();

/** True when the TOOL_ACTION command string names a view-only action a
 *  read-only session may still run. Exact-name matches only — prefixes are
 *  unsafe ("common.Control.save" shares the zoom prefix). Only meaningful
 *  while IsReadOnly(); callers check that first. */
bool IsActionAllowed( const std::string& aActionName );

} // namespace PCBJAM_READ_ONLY

#endif // PCBJAM_READ_ONLY_H
