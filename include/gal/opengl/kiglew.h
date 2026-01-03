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
    // WebGL with LEGACY_GL_EMULATION: Emscripten's GL/gl.h provides both GLES2
    // and legacy OpenGL functions (glMatrixMode, glColor4d, etc.)
    #include <GL/gl.h>
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

    // GLdouble type for double-precision functions
    #ifndef GLdouble
    typedef double GLdouble;
    #endif

    // Double-precision GL function wrappers - LEGACY_GL_EMULATION only provides float versions
    // These convert double arguments to float and call the float variants
    inline void glVertex2d(GLdouble x, GLdouble y) {
        glVertex2f((GLfloat)x, (GLfloat)y);
    }
    inline void glVertex3d(GLdouble x, GLdouble y, GLdouble z) {
        glVertex3f((GLfloat)x, (GLfloat)y, (GLfloat)z);
    }
    inline void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a) {
        glColor4f((GLfloat)r, (GLfloat)g, (GLfloat)b, (GLfloat)a);
    }
    inline void glColor3d(GLdouble r, GLdouble g, GLdouble b) {
        glColor3f((GLfloat)r, (GLfloat)g, (GLfloat)b);
    }
    inline void glTranslated(GLdouble x, GLdouble y, GLdouble z) {
        glTranslatef((GLfloat)x, (GLfloat)y, (GLfloat)z);
    }
    inline void glScaled(GLdouble x, GLdouble y, GLdouble z) {
        glScalef((GLfloat)x, (GLfloat)y, (GLfloat)z);
    }
    inline void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z) {
        glRotatef((GLfloat)angle, (GLfloat)x, (GLfloat)y, (GLfloat)z);
    }
    inline void glNormal3d(GLdouble x, GLdouble y, GLdouble z) {
        glNormal3f((GLfloat)x, (GLfloat)y, (GLfloat)z);
    }
    inline void glTexCoord2d(GLdouble s, GLdouble t) {
        glTexCoord2f((GLfloat)s, (GLfloat)t);
    }
    inline void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) {
        glRectf((GLfloat)x1, (GLfloat)y1, (GLfloat)x2, (GLfloat)y2);
    }
    inline void glLoadMatrixd(const GLdouble* m) {
        GLfloat fm[16];
        for(int i = 0; i < 16; i++) fm[i] = (GLfloat)m[i];
        glLoadMatrixf(fm);
    }
    inline void glMultMatrixd(const GLdouble* m) {
        GLfloat fm[16];
        for(int i = 0; i < 16; i++) fm[i] = (GLfloat)m[i];
        glMultMatrixf(fm);
    }

    // Display lists - not supported in WebGL, stub implementations
    inline GLuint glGenLists(GLsizei range) { (void)range; return 0; }
    inline GLboolean glIsList(GLuint list) { (void)list; return GL_FALSE; }
    inline void glNewList(GLuint list, GLenum mode) { (void)list; (void)mode; }
    inline void glEndList(void) {}
    inline void glCallList(GLuint list) { (void)list; }
    inline void glDeleteLists(GLuint list, GLsizei range) { (void)list; (void)range; }

    // Lighting and material functions - stubs (lighting not fully supported in WebGL)
    inline void glLightModeli(GLenum pname, GLint param) { (void)pname; (void)param; }
    inline void glColorMaterial(GLenum face, GLenum mode) { (void)face; (void)mode; }
    inline void glMaterialf(GLenum face, GLenum pname, GLfloat param) {
        (void)face; (void)pname; (void)param;
    }
    inline void glMaterialfv(GLenum face, GLenum pname, const GLfloat* params) {
        (void)face; (void)pname; (void)params;
    }
    inline void glLightfv(GLenum light, GLenum pname, const GLfloat* params) {
        (void)light; (void)pname; (void)params;
    }
    inline void glLightf(GLenum light, GLenum pname, GLfloat param) {
        (void)light; (void)pname; (void)param;
    }

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
