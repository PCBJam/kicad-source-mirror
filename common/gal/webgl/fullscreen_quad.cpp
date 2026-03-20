/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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

#include <gal/webgl/fullscreen_quad.h>
#include <gal/webgl/utils.h>

#include <cstdio>

using namespace KIGFX;

FULLSCREEN_QUAD::FULLSCREEN_QUAD() :
        m_initialized( false ),
        m_quadVBO( 0 ),
        m_quadVAO( 0 ),
        m_triangleVBO( 0 ),
        m_triangleVAO( 0 )
{
}


FULLSCREEN_QUAD::~FULLSCREEN_QUAD()
{
    Cleanup();
}


void FULLSCREEN_QUAD::Initialize()
{
    if( m_initialized )
        return;

    // Quad vertices: 2 triangles covering -1 to +1 in clip space
    // Each vertex has: x, y, z, w (position) + s, t, 0, 0 (texcoord)
    // Note: z=0, w=1 for positions; texcoords map [0,1] to screen
    static const float quadVertices[] = {
        // First triangle (top-left, bottom-left, top-right)
        // Position (x,y,z,w)    TexCoord (s,t,0,0)
        -1.0f,  1.0f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f, 0.0f,  // top-left
        -1.0f, -1.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 0.0f,  // bottom-left
         1.0f,  1.0f, 0.0f, 1.0f,   1.0f, 1.0f, 0.0f, 0.0f,  // top-right
        // Second triangle (top-right, bottom-left, bottom-right)
         1.0f,  1.0f, 0.0f, 1.0f,   1.0f, 1.0f, 0.0f, 0.0f,  // top-right
        -1.0f, -1.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 0.0f,  // bottom-left
         1.0f, -1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 0.0f,  // bottom-right
    };

    // Create quad VAO and VBO
    glGenVertexArrays( 1, &m_quadVAO );
    glGenBuffers( 1, &m_quadVBO );

    glBindVertexArray( m_quadVAO );
    glBindBuffer( GL_ARRAY_BUFFER, m_quadVBO );
    glBufferData( GL_ARRAY_BUFFER, sizeof( quadVertices ), quadVertices, GL_STATIC_DRAW );

    // Position attribute (a_vertex) - location 0
    glVertexAttribPointer( VERTEX_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE, 8 * sizeof( float ),
                           (void*) 0 );
    glEnableVertexAttribArray( VERTEX_ATTRIB_LOC );

    // TexCoord attribute (a_texCoord0) - location 1
    glVertexAttribPointer( TEXCOORD_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE, 8 * sizeof( float ),
                           (void*) ( 4 * sizeof( float ) ) );
    glEnableVertexAttribArray( TEXCOORD_ATTRIB_LOC );

    glBindVertexArray( 0 );
    checkGlError( "creating fullscreen quad VBO", __FILE__, __LINE__ );

    // Oversized triangle vertices: covers entire screen with one triangle
    // Uses coordinates that extend beyond the viewport
    static const float triangleVertices[] = {
        // Position (x,y,z,w)        TexCoord (s,t,0,0)
        -1.0f,  1.0f, 0.0f, 1.0f,    0.0f, 1.0f, 0.0f, 0.0f,   // top-left
        -1.0f, -3.0f, 0.0f, 1.0f,    0.0f, -1.0f, 0.0f, 0.0f,  // bottom-left (extended)
         3.0f,  1.0f, 0.0f, 1.0f,    2.0f, 1.0f, 0.0f, 0.0f,   // top-right (extended)
    };

    // Create triangle VAO and VBO
    glGenVertexArrays( 1, &m_triangleVAO );
    glGenBuffers( 1, &m_triangleVBO );

    glBindVertexArray( m_triangleVAO );
    glBindBuffer( GL_ARRAY_BUFFER, m_triangleVBO );
    glBufferData( GL_ARRAY_BUFFER, sizeof( triangleVertices ), triangleVertices, GL_STATIC_DRAW );

    // Position attribute (a_vertex) - location 0
    glVertexAttribPointer( VERTEX_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE, 8 * sizeof( float ),
                           (void*) 0 );
    glEnableVertexAttribArray( VERTEX_ATTRIB_LOC );

    // TexCoord attribute (a_texCoord0) - location 1
    glVertexAttribPointer( TEXCOORD_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE, 8 * sizeof( float ),
                           (void*) ( 4 * sizeof( float ) ) );
    glEnableVertexAttribArray( TEXCOORD_ATTRIB_LOC );

    glBindVertexArray( 0 );
    checkGlError( "creating fullscreen triangle VBO", __FILE__, __LINE__ );

    m_initialized = true;
}


void FULLSCREEN_QUAD::Draw()
{
    if( !m_initialized )
        Initialize();

    // Temporary debug: detect draws with no shader program bound ("not linked" warnings)
    GLint currentProgram = 0;
    glGetIntegerv( GL_CURRENT_PROGRAM, &currentProgram );
    if( currentProgram == 0 )
    {
        fprintf( stderr, "FULLSCREEN_QUAD::Draw() called with no shader program bound!\n" );
        return;  // Skip the draw — it would produce a "not linked" WebGL warning
    }

    glBindVertexArray( m_quadVAO );
    glDrawArrays( GL_TRIANGLES, 0, 6 );
    glBindVertexArray( 0 );
}


void FULLSCREEN_QUAD::DrawTriangle()
{
    if( !m_initialized )
        Initialize();

    glBindVertexArray( m_triangleVAO );
    glDrawArrays( GL_TRIANGLES, 0, 3 );
    glBindVertexArray( 0 );
}


void FULLSCREEN_QUAD::Cleanup()
{
    if( m_quadVBO )
    {
        glDeleteBuffers( 1, &m_quadVBO );
        m_quadVBO = 0;
    }

    if( m_quadVAO )
    {
        glDeleteVertexArrays( 1, &m_quadVAO );
        m_quadVAO = 0;
    }

    if( m_triangleVBO )
    {
        glDeleteBuffers( 1, &m_triangleVBO );
        m_triangleVBO = 0;
    }

    if( m_triangleVAO )
    {
        glDeleteVertexArrays( 1, &m_triangleVAO );
        m_triangleVAO = 0;
    }

    m_initialized = false;
}


// Global instance
static FULLSCREEN_QUAD s_fullscreenQuad;

FULLSCREEN_QUAD& KIGFX::GetFullscreenQuad()
{
    return s_fullscreenQuad;
}
