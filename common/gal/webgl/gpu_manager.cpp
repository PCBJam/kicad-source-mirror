/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright 2013-2017 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * @author Maciej Suminski <maciej.suminski@cern.ch>
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

#include <gal/webgl/gpu_manager.h>
#include <gal/webgl/cached_container_gpu.h>
#include <gal/webgl/cached_container_ram.h>
#include <gal/webgl/noncached_container.h>
#include <gal/webgl/shader.h>
#include <gal/webgl/utils.h>
#include <gal/webgl/vertex_item.h>

#include <core/profile.h>

#include <typeinfo>
#include <confirm.h>
#include <trace_helpers.h>

#ifdef KICAD_GAL_PROFILE
#include <core/profile.h>
#include <wx/log.h>
#endif /* KICAD_GAL_PROFILE */

using namespace KIGFX;

GPU_MANAGER* GPU_MANAGER::MakeManager( VERTEX_CONTAINER* aContainer )
{
    if( aContainer->IsCached() )
        return new GPU_CACHED_MANAGER( aContainer );
    else
        return new GPU_NONCACHED_MANAGER( aContainer );
}


GPU_MANAGER::GPU_MANAGER( VERTEX_CONTAINER* aContainer ) :
        m_isDrawing( false ),
        m_container( aContainer ),
        m_shader( nullptr ),
        m_shaderAttrib( 0 ),
        m_vertexAttrib( 0 ),
        m_colorAttrib( 0 ),
        m_enableDepthTest( true ),
        m_vao( 0 )
{
}


GPU_MANAGER::~GPU_MANAGER()
{
    // Delete VAO if it was created
    if( m_vao != 0 )
    {
        glDeleteVertexArrays( 1, &m_vao );
        m_vao = 0;
    }
}


void GPU_MANAGER::SetShader( SHADER& aShader )
{
    m_shader = &aShader;
    m_shaderAttrib = m_shader->GetAttribute( "a_shaderParams" );
    m_vertexAttrib = m_shader->GetAttribute( "a_vertex" );
    m_colorAttrib = m_shader->GetAttribute( "a_color" );

    if( m_shaderAttrib == -1 )
    {
        DisplayError( nullptr, wxT( "Could not get the shader attribute location" ) );
    }

    // Create VAO for WebGL 2.0 / OpenGL ES 3.0 compatibility
    // WebGL 2.0 requires a VAO to be bound before calling glVertexAttribPointer
    if( m_vao == 0 )
    {
        glGenVertexArrays( 1, &m_vao );
    }
}


// Cached manager
GPU_CACHED_MANAGER::GPU_CACHED_MANAGER( VERTEX_CONTAINER* aContainer ) :
        GPU_MANAGER( aContainer ),
        m_buffersInitialized( false ),
        m_indicesCapacity( 0 ),
        m_totalHuge( 0 ),
        m_totalNormal( 0 ),
        m_indexBufSize( 0 ),
        m_indexBufMaxSize( 0 ),
        m_curVrangeSize( 0 ),
        m_ebo( 0 )
{
}


GPU_CACHED_MANAGER::~GPU_CACHED_MANAGER()
{
    if( m_ebo )
    {
        glDeleteBuffers( 1, &m_ebo );
        m_ebo = 0;
    }
}


void GPU_CACHED_MANAGER::BeginDrawing()
{
    wxASSERT( !m_isDrawing );

    m_curVrangeSize = 0;
    m_indexBufMaxSize = 0;
    m_indexBufSize = 0;
    m_vranges.clear();

    m_isDrawing = true;
}


void GPU_CACHED_MANAGER::DrawIndices( const VERTEX_ITEM* aItem )
{
    // Hot path: don't use wxASSERT
    assert( m_isDrawing );

    unsigned int offset = aItem->GetOffset();
    unsigned int size = aItem->GetSize();

    if( size == 0 )
        return;

    if( size <= 1000 )
    {
        m_totalNormal += size;
        m_vranges.emplace_back( offset, offset + size - 1, false );
        m_curVrangeSize += size;
    }
    else
    {
        m_totalHuge += size;
        m_vranges.emplace_back( offset, offset + size - 1, true );
        m_indexBufSize = std::max( m_curVrangeSize, m_indexBufSize );
        m_curVrangeSize = 0;
    }
}


void GPU_CACHED_MANAGER::EndDrawing()
{
    wxASSERT( m_isDrawing );

    CACHED_CONTAINER* cached = static_cast<CACHED_CONTAINER*>( m_container );

    if( cached->IsMapped() )
        cached->Unmap();

    m_indexBufSize = std::max( m_curVrangeSize, m_indexBufSize );
    m_indexBufMaxSize = std::max( 2*m_indexBufSize, m_indexBufMaxSize );

    resizeIndices( m_indexBufMaxSize );

    if( m_enableDepthTest )
        glEnable( GL_DEPTH_TEST );
    else
        glDisable( GL_DEPTH_TEST );

    // Bind VAO first (required for WebGL 2.0 / OpenGL ES 3.0)
    glBindVertexArray( m_vao );

    // Bind vertices data buffers
    glBindBuffer( GL_ARRAY_BUFFER, cached->GetBufferHandle() );

    // Modern vertex attributes (replacing legacy glEnableClientState/glVertexPointer/glColorPointer)
    // Vertex position (a_vertex)
    glEnableVertexAttribArray( m_vertexAttrib );
    glVertexAttribPointer( m_vertexAttrib, COORD_STRIDE, GL_FLOAT, GL_FALSE, VERTEX_SIZE,
                           (GLvoid*) COORD_OFFSET );

    // Vertex color (a_color) - note: normalize=GL_TRUE for unsigned bytes to [0,1]
    glEnableVertexAttribArray( m_colorAttrib );
    glVertexAttribPointer( m_colorAttrib, COLOR_STRIDE, GL_UNSIGNED_BYTE, GL_TRUE, VERTEX_SIZE,
                           (GLvoid*) COLOR_OFFSET );

    if( m_shader != nullptr ) // Use shader if applicable
    {
        m_shader->Use();
        glEnableVertexAttribArray( m_shaderAttrib );
        glVertexAttribPointer( m_shaderAttrib, SHADER_STRIDE, GL_FLOAT, GL_FALSE, VERTEX_SIZE,
                               (GLvoid*) SHADER_OFFSET );
    }

    PROF_TIMER cntDraw( "gl-draw-elements" );

    int     n_ranges = m_vranges.size();
    int     n = 0;
    GLuint* iptr = m_indices.get();
    GLuint  icnt = 0;

    int drawCalls = 0;

    // Lazily create element buffer object for indexed drawing
    // WebGL 2.0 does not support client-side index arrays in glDrawElements
    if( m_ebo == 0 )
        glGenBuffers( 1, &m_ebo );

    while( n < n_ranges )
    {
        VRANGE* cur = &m_vranges[n];

        if( cur->m_isContinuous )
        {
            if( icnt > 0 )
            {
                glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, m_ebo );
                glBufferData( GL_ELEMENT_ARRAY_BUFFER, icnt * sizeof( GLuint ),
                              m_indices.get(), GL_STREAM_DRAW );
                glDrawElements( GL_TRIANGLES, icnt, GL_UNSIGNED_INT, (GLvoid*) 0 );
                glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
                drawCalls++;
            }

            icnt = 0;
            iptr = m_indices.get();

            glDrawArrays( GL_TRIANGLES, cur->m_start, cur->m_end - cur->m_start + 1 );
            drawCalls++;
        }
        else
        {
            for( GLuint i = cur->m_start; i <= cur->m_end; i++ )
            {
                *iptr++ = i;
                icnt++;
            }
        }

        n++;
    }

    if( icnt > 0 )
    {
        glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, m_ebo );
        glBufferData( GL_ELEMENT_ARRAY_BUFFER, icnt * sizeof( GLuint ),
                      m_indices.get(), GL_STREAM_DRAW );
        glDrawElements( GL_TRIANGLES, icnt, GL_UNSIGNED_INT, (GLvoid*) 0 );
        glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
        drawCalls++;
    }

    cntDraw.Stop();

    KI_TRACE( traceGalProfile,
              "Cached manager size: VBO size %u iranges %zu max elt size %u drawcalls %u\n",
              cached->AllItemsSize(), m_vranges.size(), m_indexBufMaxSize, drawCalls );
    KI_TRACE( traceGalProfile, "Timing: %s\n", cntDraw.to_string() );

    glBindBuffer( GL_ARRAY_BUFFER, 0 );
    cached->ClearDirty();

    // Deactivate vertex arrays (modern vertex attributes)
    glDisableVertexAttribArray( m_colorAttrib );
    glDisableVertexAttribArray( m_vertexAttrib );

    if( m_shader != nullptr )
    {
        glDisableVertexAttribArray( m_shaderAttrib );
        m_shader->Deactivate();
    }

    // Unbind VAO
    glBindVertexArray( 0 );

    m_isDrawing = false;
}


void GPU_CACHED_MANAGER::resizeIndices( unsigned int aNewSize )
{
    if( aNewSize > m_indicesCapacity )
    {
        m_indicesCapacity = aNewSize;
        m_indices.reset( new GLuint[m_indicesCapacity] );
    }
}


// Noncached manager
GPU_NONCACHED_MANAGER::GPU_NONCACHED_MANAGER( VERTEX_CONTAINER* aContainer ) :
        GPU_MANAGER( aContainer ),
        m_vbo( 0 )
{
}


GPU_NONCACHED_MANAGER::~GPU_NONCACHED_MANAGER()
{
    if( m_vbo )
    {
        glDeleteBuffers( 1, &m_vbo );
        m_vbo = 0;
    }
}


void GPU_NONCACHED_MANAGER::BeginDrawing()
{
    // Nothing has to be prepared
}


void GPU_NONCACHED_MANAGER::DrawIndices( const VERTEX_ITEM* aItem )
{
    wxASSERT_MSG( false, wxT( "Not implemented yet" ) );
}


void GPU_NONCACHED_MANAGER::EndDrawing()
{
#ifdef KICAD_GAL_PROFILE
    PROF_TIMER totalRealTime;
#endif /* KICAD_GAL_PROFILE */

    if( m_container->GetSize() == 0 )
        return;

    VERTEX* vertices = m_container->GetAllVertices();

    if( m_enableDepthTest )
        glEnable( GL_DEPTH_TEST );
    else
        glDisable( GL_DEPTH_TEST );

    // Bind VAO first (required for WebGL 2.0 / OpenGL ES 3.0)
    glBindVertexArray( m_vao );

    // Upload vertex data to VBO each frame
    // WebGL 2.0 does not support client-side vertex arrays — a VBO must be bound
    if( m_vbo == 0 )
        glGenBuffers( 1, &m_vbo );

    glBindBuffer( GL_ARRAY_BUFFER, m_vbo );
    glBufferData( GL_ARRAY_BUFFER, m_container->GetSize() * VERTEX_SIZE,
                  vertices, GL_STREAM_DRAW );

    // Vertex position (a_vertex) — byte offsets into VBO, not raw pointers
    glEnableVertexAttribArray( m_vertexAttrib );
    glVertexAttribPointer( m_vertexAttrib, COORD_STRIDE, GL_FLOAT, GL_FALSE, VERTEX_SIZE,
                           (GLvoid*) COORD_OFFSET );

    // Vertex color (a_color) - note: normalize=GL_TRUE for unsigned bytes to [0,1]
    glEnableVertexAttribArray( m_colorAttrib );
    glVertexAttribPointer( m_colorAttrib, COLOR_STRIDE, GL_UNSIGNED_BYTE, GL_TRUE, VERTEX_SIZE,
                           (GLvoid*) COLOR_OFFSET );

    if( m_shader != nullptr ) // Use shader if applicable
    {
        m_shader->Use();
        glEnableVertexAttribArray( m_shaderAttrib );
        glVertexAttribPointer( m_shaderAttrib, SHADER_STRIDE, GL_FLOAT, GL_FALSE, VERTEX_SIZE,
                               (GLvoid*) SHADER_OFFSET );
    }

    glDrawArrays( GL_TRIANGLES, 0, m_container->GetSize() );

#ifdef KICAD_GAL_PROFILE
    wxLogTrace( traceGalProfile, wxT( "Noncached manager size: %d" ), m_container->GetSize() );
#endif /* KICAD_GAL_PROFILE */

    // Deactivate vertex arrays
    glDisableVertexAttribArray( m_colorAttrib );
    glDisableVertexAttribArray( m_vertexAttrib );

    if( m_shader != nullptr )
    {
        glDisableVertexAttribArray( m_shaderAttrib );
        m_shader->Deactivate();
    }

    glBindBuffer( GL_ARRAY_BUFFER, 0 );

    // Unbind VAO
    glBindVertexArray( 0 );

    m_container->Clear();

#ifdef KICAD_GAL_PROFILE
    totalRealTime.Stop();
    wxLogTrace( traceGalProfile, wxT( "GPU_NONCACHED_MANAGER::EndDrawing(): %.1f ms" ),
                totalRealTime.msecs() );
#endif /* KICAD_GAL_PROFILE */
}

void GPU_MANAGER::EnableDepthTest( bool aEnabled )
{
    m_enableDepthTest = aEnabled;
}
