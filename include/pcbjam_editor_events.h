/*
 * pcbjam WASM addition: editor events for the page (overlay-system 0002 M2).
 *
 * The guide overlay needs to know what the user DID — "the Place Symbols
 * tool was activated", "the symbol chooser opened" — to advance a tutorial.
 * These hooks turn two funnels into `pcbjam:editor-event` CustomEvents on
 * `window`:
 *
 *   - TOOL_MANAGER::processEvent: every action (toolbar, hotkey, menu,
 *     RunAction) passes through as a TC_COMMAND event with its name.
 *   - DIALOG_SHIM::Show: every KiCad dialog shows and hides through it (the
 *     wasm port's ShowModal/EndModal call Show virtually).
 *
 * window CustomEvents rather than a window.kicadCollab slot: those have one
 * handler per key; any number of page listeners can subscribe here.
 * Native builds compile these as no-ops.
 */

#ifndef PCBJAM_EDITOR_EVENTS_H
#define PCBJAM_EDITOR_EVENTS_H

#include <string>
#include <typeinfo>

#include <kicommon.h>

class wxWindow;
class wxString;

namespace PCBJAM_EDITOR_EVENTS
{

/** An action ran (TC_COMMAND TA_ACTION/TA_ACTIVATE). `aDepth` is the
 *  processEvent nesting: 0 for user input, > 0 for actions a running tool
 *  issued itself. */
KICOMMON_API void NotifyAction( const std::string& aName, int aDepth );

/** A KiCad dialog was shown or hidden. `aWindow` is the dialog as a wxWindow
 *  (its pointer is the window's id in wxElementRegistry). */
KICOMMON_API void NotifyDialog( bool aShown, const wxWindow* aWindow,
                                const std::string& aClassName, const wxString& aTitle );

/** An electrical or design rules check finished in its dialog (`aKind` "erc" or "drc"):
 *  the error and warning counts the dialog shows, and the unconnected items (DRC).
 *  "Run ERC" / "Run DRC" are dialog buttons, not tool actions, so without this the page
 *  cannot tell that a check ran, let alone that it came back clean (overlay-system 0005). */
KICOMMON_API void NotifyCheckFinished( const std::string& aKind, int aErrors, int aWarnings,
                                       int aUnconnected );

/** The unqualified, demangled class name of a polymorphic object's dynamic
 *  type, e.g. "DIALOG_SYMBOL_CHOOSER". */
KICOMMON_API std::string DynamicClassName( const std::type_info& aType );

} // namespace PCBJAM_EDITOR_EVENTS

#endif // PCBJAM_EDITOR_EVENTS_H
