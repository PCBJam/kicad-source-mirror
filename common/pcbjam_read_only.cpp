// pcbjam WASM addition — see pcbjam_read_only.h.

#include <pcbjam_read_only.h>

#include <atomic>
#include <set>

namespace PCBJAM_READ_ONLY
{

static std::atomic<bool> g_readOnly{ false };


void Set( bool aReadOnly )
{
    g_readOnly.store( aReadOnly );
}


bool IsReadOnly()
{
    return g_readOnly.load();
}


bool IsActionAllowed( const std::string& aActionName )
{
    // View-only allowlist, exact names (see the header for why no prefixes).
    // Everything here is a COMMON_TOOLS/ZOOM_TOOL view action or tool-system
    // plumbing; nothing mutates the document.
    static const std::set<std::string> allowed = {
        // Zoom (COMMON_TOOLS + ZOOM_TOOL, common/tool/common_tools.cpp)
        "common.Control.zoomRedraw",
        "common.Control.zoomIn",
        "common.Control.zoomOut",
        "common.Control.zoomInCenter",
        "common.Control.zoomOutCenter",
        "common.Control.zoomCenter",
        "common.Control.zoomFitScreen",
        "common.Control.zoomFitObjects",
        "common.Control.zoomPreset",
        "common.Control.zoomTool",
        "common.Control.centerContents",

        // Pan / keyboard cursor
        "common.Control.panUp",
        "common.Control.panDown",
        "common.Control.panLeft",
        "common.Control.panRight",
        "common.Control.cursorUp",
        "common.Control.cursorDown",
        "common.Control.cursorLeft",
        "common.Control.cursorRight",
        "common.Control.cursorUpFast",
        "common.Control.cursorDownFast",
        "common.Control.cursorLeftFast",
        "common.Control.cursorRightFast",

        // Grid / units / display prefs (view-only state)
        "common.Control.gridNext",
        "common.Control.gridPrev",
        "common.Control.gridPreset",
        "common.Control.gridFast1",
        "common.Control.gridFast2",
        "common.Control.gridFastCycle",
        "common.Control.toggleGrid",
        "common.Control.imperialUnits",
        "common.Control.metricUnits",
        "common.Control.mils",
        "common.Control.toggleUnits",
        "common.Control.togglePolarCoords",
        "common.Control.resetLocalCoords",
        "common.Control.toggleCursor",
        "common.Control.cursorSmallCrosshairs",
        "common.Control.cursorFullCrosshairs",
        "common.Control.cursor45Crosshairs",
        "common.Control.highContrastMode",
        "common.Control.highContrastModeCycle",

        // Tool-system plumbing: Esc, menu state refresh, and the selection
        // tool's activation/idle loop (Selectable() already reports nothing
        // selectable, so keeping the tool alive is inert but necessary).
        "common.Interactive.cancel",
        "common.Interactive.updateMenu",
        "common.InteractiveSelection",
        "common.InteractiveSelection.selectionTool",
        "common.InteractiveSelection.clear",
    };

    return allowed.count( aActionName ) > 0;
}

} // namespace PCBJAM_READ_ONLY
