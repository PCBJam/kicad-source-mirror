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

/**
 * @file eda_3d_canvas_wasm.cpp
 *
 * WASM-only WebGL2 presentation for EDA_3D_CANVAS: blits the CPU raytracer's RAM
 * RGBA buffer to the canvas via a textured fullscreen quad.
 *
 * This is COMPLETELY NEW code for the WASM port (it does not modify any upstream
 * function), so it lives in its own translation unit rather than inline in
 * eda_3d_canvas.cpp — that keeps the (upstream) eda_3d_canvas.cpp fork diff limited
 * to the small `#ifdef __EMSCRIPTEN__` hooks, which makes rebasing onto upstream
 * KiCad far easier. The matching declaration (blitRaytracerImage) and the m_rtBlit*
 * members live in eda_3d_canvas.h, also under `#ifdef __EMSCRIPTEN__`.
 *
 * This whole TU is compiled only for the WASM build (see the EMSCRIPTEN genex in
 * 3d-viewer/CMakeLists.txt); the `#ifdef __EMSCRIPTEN__` guard below is belt-and-
 * suspenders.
 */

#include <gal/opengl/kiglew.h>    // Must be included first
#include "eda_3d_canvas.h"
#include <3d_rendering/raytracing/render_3d_raytrace_ram.h>
#include <wx/log.h>

#ifdef __EMSCRIPTEN__

static GLuint compileBlitShader( GLenum aType, const char* aSrc )
{
    GLuint sh = glCreateShader( aType );
    glShaderSource( sh, 1, &aSrc, nullptr );
    glCompileShader( sh );

    GLint ok = GL_FALSE;
    glGetShaderiv( sh, GL_COMPILE_STATUS, &ok );

    if( !ok )
    {
        char log[512] = { 0 };
        glGetShaderInfoLog( sh, sizeof( log ), nullptr, log );
        wxLogError( wxT( "3D raytracer blit shader compile failed: %s" ), log );
        glDeleteShader( sh );
        return 0;
    }

    return sh;
}


void EDA_3D_CANVAS::blitRaytracerImage()
{
    uint8_t* buffer = m_3d_render_raytracing->GetBuffer();

    if( !buffer )
        return;

    const wxSize bufSize = m_3d_render_raytracing->GetRealBufferSize();

    if( bufSize.x <= 0 || bufSize.y <= 0 )
        return;

    // Lazily build the blit program + fullscreen-quad VAO (once per GL context).
    if( m_rtBlitProgram == 0 )
    {
        const char* vtxSrc =
                "#version 300 es\n"
                "layout(location = 0) in vec2 a_pos;\n"
                "out vec2 v_uv;\n"
                "void main() {\n"
                "    v_uv = a_pos * 0.5 + 0.5;\n"
                "    gl_Position = vec4( a_pos, 0.0, 1.0 );\n"
                "}\n";
        const char* frgSrc =
                "#version 300 es\n"
                "precision mediump float;\n"
                "in vec2 v_uv;\n"
                "uniform sampler2D u_tex;\n"
                "out vec4 o_color;\n"
                "void main() {\n"
                // Force opaque: the canvas is alpha=true/premultiplied, and the
                // raytracer's background alpha is 0 — passing it through would
                // composite the page (grey) through the canvas.
                "    o_color = vec4( texture( u_tex, v_uv ).rgb, 1.0 );\n"
                "}\n";

        GLuint vs = compileBlitShader( GL_VERTEX_SHADER, vtxSrc );
        GLuint fs = compileBlitShader( GL_FRAGMENT_SHADER, frgSrc );

        if( vs == 0 || fs == 0 )
            return;

        GLuint prog = glCreateProgram();
        glAttachShader( prog, vs );
        glAttachShader( prog, fs );
        glLinkProgram( prog );
        glDeleteShader( vs );
        glDeleteShader( fs );

        GLint linked = GL_FALSE;
        glGetProgramiv( prog, GL_LINK_STATUS, &linked );

        if( !linked )
        {
            char log[512] = { 0 };
            glGetProgramInfoLog( prog, sizeof( log ), nullptr, log );
            wxLogError( wxT( "3D raytracer blit program link failed: %s" ), log );
            glDeleteProgram( prog );
            return;
        }

        m_rtBlitProgram = prog;
        m_rtBlitTexLoc  = glGetUniformLocation( prog, "u_tex" );

        // Two triangles covering the clip-space viewport; UVs derived in the VS.
        static const float quad[] = { -1.f, -1.f,  1.f, -1.f, -1.f,  1.f,
                                      -1.f,  1.f,  1.f, -1.f,  1.f,  1.f };

        glGenVertexArrays( 1, &m_rtBlitVAO );
        glBindVertexArray( m_rtBlitVAO );
        glGenBuffers( 1, &m_rtBlitVBO );
        glBindBuffer( GL_ARRAY_BUFFER, m_rtBlitVBO );
        glBufferData( GL_ARRAY_BUFFER, sizeof( quad ), quad, GL_STATIC_DRAW );
        glEnableVertexAttribArray( 0 );
        glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );
        glBindVertexArray( 0 );

        glGenTextures( 1, &m_rtBlitTexture );
    }

    // Upload the raytraced RGBA image (rows are bottom-up, matching GL textures).
    glBindTexture( GL_TEXTURE_2D, m_rtBlitTexture );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, bufSize.x, bufSize.y, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, buffer );

    // Draw it across the whole canvas. Reset any GL state left bound by other
    // rendering so the quad actually reaches the visible default framebuffer.
    const wxSize clientSize = GetNativePixelSize();
    glBindFramebuffer( GL_FRAMEBUFFER, 0 );
    glViewport( 0, 0, clientSize.x, clientSize.y );
    glDisable( GL_DEPTH_TEST );
    glDisable( GL_BLEND );
    glDisable( GL_SCISSOR_TEST );
    glDisable( GL_CULL_FACE );
    glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
    glDepthMask( GL_TRUE );
    glUseProgram( m_rtBlitProgram );
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, m_rtBlitTexture );
    glUniform1i( m_rtBlitTexLoc, 0 );
    glBindVertexArray( m_rtBlitVAO );
    glDrawArrays( GL_TRIANGLES, 0, 6 );
    glBindVertexArray( 0 );
    glUseProgram( 0 );
}

#endif // __EMSCRIPTEN__
