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
    // Prevent real GLEW header from being included (Emscripten has one too)
    #ifndef __glew_h__
    #define __glew_h__
    #endif

    // WebGL2/GLES3: Modern shader functions
    // Note: NO legacy GL includes - all rendering uses VBOs/shaders
    #include <GLES3/gl3.h>
    // GLU tesselator - provided by wasm/stubs/glu_wasm_impl.cpp
    #include <GL/glu.h>

    // GLEW compatibility stubs for WebGL
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
    #define GLEW_ARB_copy_buffer 0  // Not available in WebGL 1.0
    #define GLEW_EXT_framebuffer_multisample 0  // Limited in WebGL

    inline int glewInit() { return GLEW_OK; }
    inline const unsigned char* glewGetString(int) { return (const unsigned char*)"WebGL"; }
    inline const char* glewGetErrorString(int) { return ""; }
    inline int glewIsSupported(const char*) { return 1; }

    // VAO functions - available in WebGL2 / OpenGL ES 3.0
    #ifndef GL_VERTEX_ARRAY_BINDING
    #define GL_VERTEX_ARRAY_BINDING 0x85B5
    #endif

    // Geometry shader extensions - not supported in WebGL
    #ifndef GL_GEOMETRY_SHADER
    #define GL_GEOMETRY_SHADER 0x8DD9
    #endif
    #ifndef GL_GEOMETRY_VERTICES_OUT_EXT
    #define GL_GEOMETRY_VERTICES_OUT_EXT 0x8DDA
    #define GL_GEOMETRY_INPUT_TYPE_EXT 0x8DDB
    #define GL_GEOMETRY_OUTPUT_TYPE_EXT 0x8DDC
    #endif

    // Geometry shader function stub (not supported in WebGL)
    inline void glProgramParameteriEXT(GLuint program, GLenum pname, GLint value) {
        (void)program; (void)pname; (void)value;
    }

    // glMapBuffer family - not available in WebGL 1.0
    // Return nullptr to signal failure, KiCad has RAM-based fallback
    // Buffer access mode constants (needed for compilation, glMapBuffer returns nullptr)
    #ifndef GL_READ_ONLY
    #define GL_READ_ONLY 0x88B8
    #endif
    #ifndef GL_WRITE_ONLY
    #define GL_WRITE_ONLY 0x88B9
    #endif
    #ifndef GL_READ_WRITE
    #define GL_READ_WRITE 0x88BA
    #endif

    inline void* glMapBuffer(GLenum target, GLenum access) {
        (void)target; (void)access;
        return nullptr;
    }

    inline GLboolean glUnmapBuffer(GLenum target) {
        (void)target;
        return GL_TRUE;
    }

    // Buffer copy - not available in WebGL 1.0, no-op stub
    inline void glCopyBufferSubData(GLenum readTarget, GLenum writeTarget,
                                     GLintptr readOffset, GLintptr writeOffset,
                                     GLsizeiptr size) {
        (void)readTarget; (void)writeTarget;
        (void)readOffset; (void)writeOffset; (void)size;
    }

    // EXT framebuffer functions - alias to standard GL ES 2.0 functions
    #ifndef GL_FRAMEBUFFER_EXT
    #define GL_FRAMEBUFFER_EXT GL_FRAMEBUFFER
    #endif
    #ifndef GL_RENDERBUFFER_EXT
    #define GL_RENDERBUFFER_EXT GL_RENDERBUFFER
    #endif
    #ifndef GL_FRAMEBUFFER_COMPLETE_EXT
    #define GL_FRAMEBUFFER_COMPLETE_EXT GL_FRAMEBUFFER_COMPLETE
    #endif

    #define glGenFramebuffersEXT glGenFramebuffers
    #define glDeleteFramebuffersEXT glDeleteFramebuffers
    #define glBindFramebufferEXT glBindFramebuffer
    #define glCheckFramebufferStatusEXT glCheckFramebufferStatus
    #define glFramebufferTexture2DEXT glFramebufferTexture2D
    #define glFramebufferRenderbufferEXT glFramebufferRenderbuffer

    #define glGenRenderbuffersEXT glGenRenderbuffers
    #define glDeleteRenderbuffersEXT glDeleteRenderbuffers
    #define glBindRenderbufferEXT glBindRenderbuffer
    #define glRenderbufferStorageEXT glRenderbufferStorage

    // Framebuffer status constants (EXT aliases)
    #ifndef GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT
    #define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT 0x8CD6
    #endif
    #ifndef GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_EXT
    #define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_EXT 0x8CD7
    #endif
    #ifndef GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_EXT
    #define GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_EXT 0x8CD9
    #endif
    #ifndef GL_FRAMEBUFFER_INCOMPLETE_FORMATS_EXT
    #define GL_FRAMEBUFFER_INCOMPLETE_FORMATS_EXT 0x8CDA
    #endif
    #ifndef GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER_EXT
    #define GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER_EXT 0x8CDB
    #endif
    #ifndef GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER_EXT
    #define GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER_EXT 0x8CDC
    #endif
    #ifndef GL_FRAMEBUFFER_UNSUPPORTED_EXT
    #define GL_FRAMEBUFFER_UNSUPPORTED_EXT 0x8CDD
    #endif
    #ifndef GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_EXT
    #define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_EXT 0x8D56
    #endif
    #ifndef GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS_EXT
    #define GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS_EXT 0x8DA8
    #endif

    // Renderbuffer size query (EXT alias)
    #ifndef GL_MAX_RENDERBUFFER_SIZE_EXT
    #define GL_MAX_RENDERBUFFER_SIZE_EXT GL_MAX_RENDERBUFFER_SIZE
    #endif

    // GL_DEPTH24_STENCIL8 - map to GLES2/WebGL constant
    #ifndef GL_DEPTH24_STENCIL8
    #define GL_DEPTH24_STENCIL8 0x88F0
    #endif

    // GL_DEPTH_STENCIL_ATTACHMENT - WebGL uses separate depth/stencil, but this constant exists
    #ifndef GL_DEPTH_STENCIL_ATTACHMENT
    #define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
    #endif

    // Debug output - not available in WebGL, no-op stubs
    #ifndef GL_DEBUG_OUTPUT
    #define GL_DEBUG_OUTPUT 0x92E0
    #endif

    #ifndef GLchar
    typedef char GLchar;
    #endif

    typedef void (*GLDEBUGPROC)(GLenum source, GLenum type, GLuint id,
                                GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam);

    inline void glDebugMessageCallback(GLDEBUGPROC callback, const void* userParam) {
        (void)callback; (void)userParam;
    }

    // GLdouble type (needed for some API signatures)
    #ifndef GLdouble
    typedef double GLdouble;
    #endif

    // Display lists - not supported in WebGL, stub implementations
    inline GLuint glGenLists(GLsizei range) { (void)range; return 0; }
    inline GLboolean glIsList(GLuint list) { (void)list; return GL_FALSE; }
    inline void glNewList(GLuint list, GLenum mode) { (void)list; (void)mode; }
    inline void glEndList(void) {}
    inline void glCallList(GLuint list) { (void)list; }
    inline void glDeleteLists(GLuint list, GLsizei range) { (void)list; (void)range; }

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
