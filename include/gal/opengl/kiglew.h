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
 * This file is used for including the proper GLEW header for the platform.
 */

#ifndef KIGLEW_H_
#define KIGLEW_H_

// Pull in the configuration options for wxWidgets
#include <wx/platform.h>

#if defined( __EMSCRIPTEN__ )

    // WASM: the 3D viewer's renderer is legacy fixed-function OpenGL. Route it
    // through Emscripten's desktop-GL emulation: <GL/gl.h> declares the
    // fixed-function pipeline (glBegin/glMatrixMode/glLight*/glVertexPointer/...)
    // and, with GL_GLEXT_PROTOTYPES, pulls <GL/glext.h> for the modern VBO/shader
    // entry points the renderer also uses. The runtime is supplied by
    // -sLEGACY_GL_EMULATION (see scripts/kicad/build-kicad-target.sh). The 2D GAL
    // keeps its pure-GLES3 path in gal/webgl/kiglew.h; the two never share a TU.
    #ifndef __glew_h__
    #define __glew_h__   // keep Emscripten's bundled GL/glew.h out
    #endif

    #define GL_GLEXT_PROTOTYPES
    #include <GL/gl.h>
    #include <GL/glu.h>  // GLU tesselator stub (wasm/stubs/GL/glu.h, earcut-based)

    // GLEW compatibility shims — the GL backend probes GLEW at init time.
    #define GLEW_OK 0
    #define GLEW_VERSION 1
    #define GLEW_VERSION_1_2 1
    #define GLEW_VERSION_1_3 1
    #define GLEW_VERSION_1_4 1
    #define GLEW_VERSION_1_5 1
    #define GLEW_VERSION_2_0 1
    #define GLEW_VERSION_2_1 1
    #define GLEW_ARB_vertex_array_object 1
    #define GLEW_ARB_vertex_buffer_object 1
    #define GLEW_ARB_framebuffer_object 1
    #define GLEW_EXT_framebuffer_object 1
    #define GLEW_ARB_texture_non_power_of_two 1

    inline int glewInit() { return GLEW_OK; }
    inline const unsigned char* glewGetString( int ) { return (const unsigned char*) "WebGL"; }
    inline const char* glewGetErrorString( int ) { return ""; }
    inline int glewIsSupported( const char* ) { return 1; }

#elif defined( __unix__ ) and not defined( __APPLE__ )

    #ifdef KICAD_USE_EGL

        #if wxUSE_GLCANVAS_EGL
            // wxWidgets was compiled with the EGL canvas, so use the EGL header for GLEW
            #include <GL/eglew.h>
        #else
            #error "KICAD_USE_EGL can only be used when wxWidgets is compiled with the EGL canvas"
        #endif

    #else   // KICAD_USE_EGL

        #if wxUSE_GLCANVAS_EGL
            #error "KICAD_USE_EGL must be defined since wxWidgets has been compiled with the EGL canvas"
        #else
            // wxWidgets wasn't compiled with the EGL canvas, so use the X11 GLEW
            #include <GL/glxew.h>
        #endif

    #endif  // KICAD_USE_EGL

#else   // defined( __unix__ ) and not defined( __APPLE__ )

    // Non-GTK platforms only need the normal GLEW include
    #include <GL/glew.h>

#endif  // defined( __unix__ ) and not defined( __APPLE__ )

#ifdef _WIN32

    #include <GL/wglew.h>

#endif  // _WIN32

#endif  // KIGLEW_H_
