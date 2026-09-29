// pcbjam WASM addition — see pcbjam_editor_events.h.

#include <pcbjam_editor_events.h>

#include <cstdint>
#include <cstdlib>
#include <typeinfo>

#include <wx/string.h>
#include <wx/window.h>

#if defined( __EMSCRIPTEN__ )
#include <cxxabi.h>
#include <emscripten.h>
#endif

namespace PCBJAM_EDITOR_EVENTS
{

void NotifyAction( const std::string& aName, int aDepth )
{
#if defined( __EMSCRIPTEN__ )
    // try/catch: a throwing page listener must never reach the wasm stack
    // (under JSPI it would reject the suspended coroutine).
    EM_ASM(
            {
                try
                {
                    window.dispatchEvent( new CustomEvent( 'pcbjam:editor-event', {
                        detail : { type : 'action', name : UTF8ToString( $0 ), depth : $1 }
                    } ) );
                }
                catch( e )
                {
                    console.error( 'pcbjam:editor-event action', e );
                }
            },
            aName.c_str(), aDepth );
#else
    (void) aName;
    (void) aDepth;
#endif
}


void NotifyDialog( bool aShown, const wxWindow* aWindow, const std::string& aClassName,
                   const wxString& aTitle )
{
#if defined( __EMSCRIPTEN__ )
    // The pointer as a decimal string: the same id wxElementRegistry uses.
    uintptr_t ptr = reinterpret_cast<uintptr_t>( aWindow );

    EM_ASM(
            {
                try
                {
                    window.dispatchEvent( new CustomEvent( 'pcbjam:editor-event', {
                        detail : {
                            type : $0 ? 'dialogShown' : 'dialogClosed',
                            cls : UTF8ToString( $1 ),
                            ptr : $2.toString(),
                            title : UTF8ToString( $3 )
                        }
                    } ) );
                }
                catch( e )
                {
                    console.error( 'pcbjam:editor-event dialog', e );
                }
            },
            aShown ? 1 : 0, aClassName.c_str(), ptr, aTitle.utf8_str().data() );
#else
    (void) aShown;
    (void) aWindow;
    (void) aClassName;
    (void) aTitle;
#endif
}


std::string DynamicClassName( const std::type_info& aType )
{
    std::string name = aType.name();

#if defined( __EMSCRIPTEN__ ) || defined( __GNUG__ )
    int   status = 0;
    char* demangled = abi::__cxa_demangle( aType.name(), nullptr, nullptr, &status );

    if( status == 0 && demangled )
        name = demangled;

    std::free( demangled );
#endif

    // Drop any namespace / template noise: "ns::DIALOG_FOO" → "DIALOG_FOO".
    size_t colon = name.rfind( "::" );

    if( colon != std::string::npos )
        name = name.substr( colon + 2 );

    return name;
}

} // namespace PCBJAM_EDITOR_EVENTS
