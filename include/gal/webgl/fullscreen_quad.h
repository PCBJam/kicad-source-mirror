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
 * @file fullscreen_quad.h
 * @brief VBO-based fullscreen quad for WebGL texture compositing.
 *        Replaces legacy GL immediate mode (glBegin/glVertex/glEnd).
 */

#ifndef FULLSCREEN_QUAD_H_
#define FULLSCREEN_QUAD_H_

#include <gal/webgl/kiglew.h>

namespace KIGFX
{

/**
 * A VBO-based fullscreen quad for drawing textures to the screen.
 * Used by compositor and antialiasing passes.
 */
class FULLSCREEN_QUAD
{
public:
    FULLSCREEN_QUAD();
    ~FULLSCREEN_QUAD();

    /**
     * Initialize the VBO and VAO. Must be called after GL context is created.
     */
    void Initialize();

    /**
     * Draw the fullscreen quad. Assumes a shader is already bound.
     * The shader must have:
     *   - a_vertex (location 0): vec4 position
     *   - a_texCoord0 (location 1): vec4 texture coordinates
     */
    void Draw();

    /**
     * Draw a fullscreen triangle (more efficient than quad for some GPUs).
     * Uses an oversized triangle that covers the entire screen.
     */
    void DrawTriangle();

    /**
     * Check if initialized.
     */
    bool IsInitialized() const { return m_initialized; }

    /**
     * Clean up GL resources.
     */
    void Cleanup();

    // Attribute locations used by the fullscreen quad
    static const GLuint VERTEX_ATTRIB_LOC = 0;
    static const GLuint TEXCOORD_ATTRIB_LOC = 1;

private:
    bool   m_initialized;
    GLuint m_quadVBO;       ///< VBO for quad vertices (6 vertices, 2 triangles)
    GLuint m_quadVAO;       ///< VAO for quad
    GLuint m_triangleVBO;   ///< VBO for single oversized triangle
    GLuint m_triangleVAO;   ///< VAO for triangle
};

/**
 * Get the global fullscreen quad instance.
 * This is lazily initialized on first use.
 */
FULLSCREEN_QUAD& GetFullscreenQuad();

} // namespace KIGFX

#endif /* FULLSCREEN_QUAD_H_ */
