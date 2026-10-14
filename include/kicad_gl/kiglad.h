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
 * This file is used for including the proper OpenGL loader header for the platform.
 */

#ifndef KIGLAD_H_
#define KIGLAD_H_

#ifdef _WIN32

// GL/GLU needs APIENTRY, CALLBACK and WINGDIAPI defined on Windows.
// Don't include <windows.h> to avoid name space pollution.
// This is based on GLEW code.

/* <windef.h> and <gl.h>*/
#ifndef APIENTRY
#  if defined(__MINGW32__) || defined(__CYGWIN__) || (_MSC_VER >= 800) || defined(_STDCALL_SUPPORTED) || defined(__BORLANDC__)
#    define APIENTRY __stdcall
#  else
#    define APIENTRY
#  endif
#endif

/* <winnt.h> */
#ifndef CALLBACK
#  if defined(__MINGW32__) || defined(__CYGWIN__)
#    define CALLBACK __attribute__ ((__stdcall__))
#  elif (defined(_M_MRX000) || defined(_M_IX86) || defined(_M_ALPHA) || defined(_M_PPC)) && !defined(MIDL_PASS)
#    define CALLBACK __stdcall
#  else
#    define CALLBACK
#  endif
#endif

/* <wingdi.h> and <winnt.h> */
#ifndef WINGDIAPI
#define WINGDIAPI __declspec(dllimport)
#endif

#endif

#if defined( __EMSCRIPTEN__ )
// WASM: GLAD's desktop-GL loader is incompatible with Emscripten's built-in GLES —
// glad/gl.h declares glad_* function pointers and #define's the GL entry points onto
// them, which collide with the real declarations pulled in by <wx/glcanvas.h> →
// <GLES2/gl2.h>. Route through our kiglew.h shim instead (Emscripten GL via legacy-GL
// emulation); gladLoaderLoadGL() is stubbed in the WASM layer.
#include <gal/opengl/kiglew.h>
#else
#include <glad/gl.h>
#endif

#endif  // KIGLAD_H_
