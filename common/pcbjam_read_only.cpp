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
        // tool's activation/idle loop. Selection itself is ALLOWED for
        // viewers (viewer-panels — the inspector panel reads it); the tool's
        // mutating follow-ups (move/properties/delete) are actions this
        // gate swallows.
        // 3D viewer (read-only-viewer 3D entry, session menu → kicadShow3DViewer):
        // opening the viewer frame, and every action of the frame's OWN tool
        // manager, which funnels through the same gate. All of them are camera /
        // render-preference state or image export; none can reach the board.
        "common.Control.show3DViewer",
        "3DViewer.Control",
        "3DViewer.Control.reloadBoard",
        "3DViewer.Control.toggleRaytacing",
        "3DViewer.Control.copyToClipboard",
        "3DViewer.Control.exportImage",
        "3DViewer.Control.pivotCenter",
        "3DViewer.Control.rotateXclockwise",
        "3DViewer.Control.rotateXcounterclockwise",
        "3DViewer.Control.rotateYclockwise",
        "3DViewer.Control.rotateYcounterclockwise",
        "3DViewer.Control.rotateZclockwise",
        "3DViewer.Control.rotateZcounterclockwise",
        "3DViewer.Control.moveLeft",
        "3DViewer.Control.moveRight",
        "3DViewer.Control.moveUp",
        "3DViewer.Control.moveDown",
        "3DViewer.Control.homeView",
        "3DViewer.Control.flipView",
        "3DViewer.Control.toggleOrtho",
        "3DViewer.Control.viewFront",
        "3DViewer.Control.viewBack",
        "3DViewer.Control.viewLeft",
        "3DViewer.Control.viewRight",
        "3DViewer.Control.viewTop",
        "3DViewer.Control.viewBottom",
        "3DViewer.Control.noGrid",
        "3DViewer.Control.show10mmGrid",
        "3DViewer.Control.show5mmGrid",
        "3DViewer.Control.show2_5mmGrid",
        "3DViewer.Control.show1mmGrid",
        "3DViewer.Control.materialNormal",
        "3DViewer.Control.materialDiffuse",
        "3DViewer.Control.materialCAD",
        "3DViewer.Control.attributesTHT",
        "3DViewer.Control.attributesSMD",
        "3DViewer.Control.attributesOther",
        "3DViewer.Control.attribute_not_in_posfile",
        "3DViewer.Control.attribute_dnp",
        "3DViewer.Control.showBoundingBoxes",
        "3DViewer.Control.showNavigator",
        "3DViewer.Control.showLayersManager",

        // Sheet navigation (eeschema SCH_NAVIGATE_TOOL): changes which sheet
        // is SHOWN, never the document — the floating sheet panel and the
        // wx hierarchy pane both dispatch changeSheet (sheet-panel).
        "eeschema.NavigateTool.changeSheet",
        "eeschema.NavigateTool.enterSheet",
        "eeschema.NavigateTool.leaveSheet",

        "common.Interactive.cancel",
        "common.Interactive.updateMenu",
        "common.InteractiveSelection",
        "common.InteractiveSelection.selectionTool",
        "common.InteractiveSelection.clear",
    };

    return allowed.count( aActionName ) > 0;
}

} // namespace PCBJAM_READ_ONLY
