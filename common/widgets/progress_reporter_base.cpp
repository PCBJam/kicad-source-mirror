/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2017 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <wx/evtloop.h>
#include <thread>
#include <widgets/progress_reporter_base.h>

#ifdef __EMSCRIPTEN__
#include <chrono>
#include <emscripten/threading.h>
#include <wx/utils.h>

// pcbjam web shell (wasm/bindings): 0 while a file open is in flight.
extern "C" int pcbjam_progress_may_yield() __attribute__(( weak ));

namespace
{
// pcbjam: the browser runs this on its main thread. Long operations (DRC, zone
// fill, ERC) join their thread-pool tasks in emscripten's busy-spin futex wait,
// so between the refresh points below the tab could neither paint nor take
// input. Hand the browser a turn at most every 80 ms: a main-thread nanosleep
// suspends to the event loop (pcbjam/wasm/shims/nanosleep_yield.c); while the
// operation is suspended the wx dispatch interlock queues input and only paints.
void yieldToBrowser()
{
    using clock = std::chrono::steady_clock;
    static clock::time_point s_lastYield;

    if( !emscripten_is_main_browser_thread() )
        return;

    if( pcbjam_progress_may_yield && !pcbjam_progress_may_yield() )
        return;

    if( clock::now() - s_lastYield < std::chrono::milliseconds( 80 ) )
        return;

    wxMilliSleep( 1 );
    s_lastYield = clock::now();
}
} // namespace
#endif

PROGRESS_REPORTER_BASE::PROGRESS_REPORTER_BASE( int aNumPhases ) :
    PROGRESS_REPORTER(),
    m_phase( 0 ),
    m_numPhases( aNumPhases ),
    m_progress( 0 ),
    m_maxProgress( 1000 ),
    m_cancelled( false ),
    m_messageChanged( false )
{
}


void PROGRESS_REPORTER_BASE::BeginPhase( int aPhase )
{
    m_phase.store( aPhase );
    m_progress.store( 0 );
}


void PROGRESS_REPORTER_BASE::AdvancePhase()
{
    m_phase.fetch_add( 1 );
    m_progress.store( 0 );

#ifdef __EMSCRIPTEN__
    yieldToBrowser();
#endif
}


void PROGRESS_REPORTER_BASE::AdvancePhase( const wxString& aMessage )
{
    AdvancePhase();
    Report( aMessage );
}


void PROGRESS_REPORTER_BASE::Report( const wxString& aMessage )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    m_messageChanged = m_rptMessage != aMessage;
    m_rptMessage = aMessage;
}


void PROGRESS_REPORTER_BASE::SetMaxProgress( int aMaxProgress )
{
    m_maxProgress.store( aMaxProgress );
}


void PROGRESS_REPORTER_BASE::SetCurrentProgress( double aProgress )
{
    m_maxProgress.store( 1000 );
    m_progress.store( (int) ( aProgress * 1000.0 ) );
}


void PROGRESS_REPORTER_BASE::AdvanceProgress()
{
    m_progress.fetch_add( 1 );
}


void PROGRESS_REPORTER_BASE::SetNumPhases( int aNumPhases )
{
    m_numPhases = aNumPhases;
}


void PROGRESS_REPORTER_BASE::AddPhases( int aNumPhases )
{
    m_numPhases += aNumPhases;
}


int PROGRESS_REPORTER_BASE::CurrentProgress() const
{
    double current = ( 1.0 / (double) m_numPhases ) *
                     ( (double) m_phase + ( (double) m_progress.load() / (double) m_maxProgress ) );

    return (int)( current * 1000 );
}


bool PROGRESS_REPORTER_BASE::KeepRefreshing( bool aWait )
{
    if( aWait )
    {
        while( m_progress.load() < m_maxProgress && m_maxProgress > 0 )
        {
            if( !updateUI() )
            {
                m_cancelled.store( true );
                return false;
            }

            wxMilliSleep( 33 /* 30 FPS refresh rate */ );
        }

        // Force one terminal refresh after throttled updates.
        if( m_maxProgress > 0 && !updateUI() )
        {
            m_cancelled.store( true );
            return false;
        }

        return true;
    }
    else
    {
        if( !updateUI() )
        {
            m_cancelled.store( true );
            return false;
        }

#ifdef __EMSCRIPTEN__
        yieldToBrowser();
#endif

        return true;
    }
}

