/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef WEBVIEW_PANEL_H
#define WEBVIEW_PANEL_H

#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/webview.h>
#include <wx/weakref.h>
#include <functional>
#include <map>

#if !wxUSE_WEBVIEW
// wx/webview.h declares nothing when wxUSE_WEBVIEW is 0 (e.g. WASM); forward-declare
// the types referenced by the (stubbed) class interface so the header still compiles.
class wxWebView;
class wxWebViewEvent;
#endif

class TOOL_MANAGER;
class TOOL_BASE;

namespace WEBVIEW_PANEL_DETAIL
{
template<typename T>
T* GetLiveWindow( const wxWeakRef<T>& aWindowRef )
{
    T* window = aWindowRef.get();

    if( window && !window->IsBeingDeleted() )
        return window;

    return nullptr;
}
} // namespace WEBVIEW_PANEL_DETAIL

class WEBVIEW_PANEL : public wxPanel
{
public:
    using MESSAGE_HANDLER = std::function<void( const wxString& )>;

    explicit WEBVIEW_PANEL( wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition,
                            const wxSize& size = wxDefaultSize, const int style = 0,
                            TOOL_MANAGER* aToolManager = nullptr, TOOL_BASE* aTool = nullptr );
    ~WEBVIEW_PANEL() override;

#if wxUSE_WEBVIEW
    wxWebView* GetWebView() const { return WEBVIEW_PANEL_DETAIL::GetLiveWindow( m_browser ); }
#else
    // No webview backend (e.g. WASM, wxUSE_WEBVIEW=0): always returns null.
    wxWebView* GetWebView() const { return nullptr; }
#endif
    const wxString& GetBackend() const { return m_backend; }

    void LoadURL( const wxString& url );
    void SetPage( const wxString& htmlContent );

    bool AddMessageHandler( const wxString& name, MESSAGE_HANDLER handler );
    void ClearMessageHandlers();

    void SetHandleExternalLinks( bool aHandle ) { m_handleExternalLinks = aHandle; }
    bool GetHandleExternalLinks() const { return m_handleExternalLinks; }

    void RunScriptAsync( const wxString& aScript, void* aClientData = nullptr ) const
    {
#if wxUSE_WEBVIEW
        if( wxWebView* browser = GetWebView() )
            browser->RunScriptAsync( aScript, aClientData );
#else
        (void) aScript;
        (void) aClientData;
#endif
    }

    bool HasLoadError() const { return m_loadError; }

    void BindLoadedEvent();

protected:
    void OnNavigationRequest( wxWebViewEvent& evt );
    void OnWebViewLoaded( wxWebViewEvent& evt );
    void OnNewWindow( wxWebViewEvent& evt );
    void OnScriptMessage( wxWebViewEvent& evt );
    void OnScriptResult( wxWebViewEvent& evt );
    void OnError( wxWebViewEvent& evt );

    void DoInitHandlers();

private:

    bool                                m_initialized;
    bool                                m_handleExternalLinks;
    bool                                m_loadError;
    bool                                m_loadedEventBound;
#if wxUSE_WEBVIEW
    wxWeakRef<wxWebView>                m_browser;
#endif
    wxString                            m_backend;
    std::map<wxString, MESSAGE_HANDLER> m_msgHandlers;
    TOOL_MANAGER*                       m_toolManager;
    TOOL_BASE*                          m_tool;
    wxTimer                             m_initRetryTimer;
};

#endif // WEBVIEW_PANEL_H
