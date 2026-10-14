#!/usr/bin/env python3
"""
Convert GLSL 1.20 shaders to GLSL ES 3.00 for WebGL 2.0.

This is the canonical converter — also imported by tests/gal-regression/wasm/generate_shaders.py.
Usage: python3 convert_glsl_es3.py <input.glsl> <output.glsl> <vertex|fragment>
"""

import os
import re
import sys


def convert_glsl120_to_es300(shader_source, is_fragment_shader):
    """
    Convert GLSL 1.20 (OpenGL 2.1) shader to GLSL ES 3.00 (WebGL 2.0).

    Key changes:
    - #version 120 -> #version 300 es (must be ABSOLUTE first line!)
    - Move any comments before #version to after declarations
    - Add precision qualifiers
    - attribute -> in
    - varying -> out (vertex) / in (fragment)
    - gl_FragColor -> custom output variable (fragment)
    - texture2D -> texture
    - Fix int * float type issues (2 * x -> 2.0 * x)
    - Fix int / float type issues (x / 4 -> x / 4.0)

    Legacy GL built-in conversions (for KiCad shaders):
    - gl_ModelViewProjectionMatrix -> uniform u_modelViewProjectionMatrix
    - gl_Vertex -> attribute a_vertex
    - gl_Color -> attribute a_color (vertex) / varying v_color (fragment)
    - gl_FrontColor -> varying v_color (vertex output)
    - gl_TexCoord[0] -> varying v_texCoord
    - ftransform() -> u_modelViewProjectionMatrix * a_vertex
    """
    lines = shader_source.split('\n')
    result = []
    version_replaced = False
    next_attrib_loc = 0  # Track next available attribute location for layout qualifiers

    # Track what legacy built-ins are used so we can add declarations
    uses_mvp_matrix = 'gl_ModelViewProjectionMatrix' in shader_source or 'ftransform()' in shader_source
    uses_gl_vertex = 'gl_Vertex' in shader_source or 'ftransform()' in shader_source
    uses_gl_color = 'gl_Color' in shader_source
    uses_gl_front_color = 'gl_FrontColor' in shader_source
    uses_gl_texcoord = 'gl_TexCoord' in shader_source
    uses_gl_multitexcoord0 = 'gl_MultiTexCoord0' in shader_source

    # GLSL ES 3.00 requires #version to be the ABSOLUTE first line
    # Collect any comments before #version to add after declarations
    pre_version_comments = []
    in_multiline_comment = False

    # Patterns to fix type issues
    int_mult_pattern = re.compile(r'\b(\d+)\s*\*\s*([a-zA-Z_])')
    div_int_pattern = re.compile(r'([a-zA-Z_)\]]+)\s*/\s*(\d+)(?!\.)')

    for line in lines:
        stripped = line.strip()

        # Before #version is found, collect comments and empty lines
        if not version_replaced:
            # Track multiline comment state
            if '/*' in stripped and '*/' not in stripped:
                in_multiline_comment = True
                pre_version_comments.append(line)
                continue
            elif in_multiline_comment:
                pre_version_comments.append(line)
                if '*/' in stripped:
                    in_multiline_comment = False
                continue
            elif stripped.startswith('//') or stripped == '' or ('/*' in stripped and '*/' in stripped):
                pre_version_comments.append(line)
                continue

        # Replace #version 120 with #version 300 es + precision + legacy built-in replacements
        if stripped.startswith('#version'):
            result.append('#version 300 es')
            result.append('precision highp float;')
            result.append('precision highp int;')

            if is_fragment_shader:
                result.append('layout(location = 0) out vec4 fragColor;')
                # Fragment shader receives varyings from vertex shader
                if uses_gl_color or uses_gl_front_color:
                    result.append('in vec4 v_color;')
                if uses_gl_texcoord:
                    result.append('in vec2 v_texCoord;')
            else:
                # Vertex shader - add uniforms and attributes for legacy built-ins
                # Use layout(location = N) qualifiers to fix attribute locations
                # (WebGL 2.0 / GLSL ES 3.0 does not auto-assign consistent locations)
                loc = 0
                if uses_mvp_matrix:
                    result.append('uniform mat4 u_modelViewProjectionMatrix;')
                if uses_gl_vertex:
                    result.append(f'layout(location = {loc}) in vec4 a_vertex;'); loc += 1
                if uses_gl_color:
                    result.append(f'layout(location = {loc}) in vec4 a_color;'); loc += 1
                if uses_gl_multitexcoord0:
                    result.append(f'layout(location = {loc}) in vec4 a_texCoord0;'); loc += 1
                next_attrib_loc = loc
                # Vertex shader outputs varyings
                if uses_gl_front_color or uses_gl_color:
                    result.append('out vec4 v_color;')
                if uses_gl_texcoord:
                    result.append('out vec2 v_texCoord;')

            # Add back any pre-version comments after the declarations
            if pre_version_comments:
                result.append('')  # Blank line before comments
                result.extend(pre_version_comments)

            version_replaced = True
            continue

        # Convert attribute to in with layout qualifier (vertex shaders only)
        if not is_fragment_shader and stripped.startswith('attribute '):
            line = line.replace('attribute ', f'layout(location = {next_attrib_loc}) in ', 1)
            next_attrib_loc += 1

        # Convert varying to out (vertex) or in (fragment)
        if stripped.startswith('varying '):
            if is_fragment_shader:
                line = line.replace('varying ', 'in ', 1)
            else:
                line = line.replace('varying ', 'out ', 1)

        # Convert gl_FragColor to fragColor (fragment shaders)
        if is_fragment_shader and 'gl_FragColor' in line:
            line = line.replace('gl_FragColor', 'fragColor')

        # Convert texture2D to texture
        if 'texture2D' in line:
            line = line.replace('texture2D', 'texture')

        # Convert legacy GL built-ins
        # ftransform() -> u_modelViewProjectionMatrix * a_vertex (must be done before other replacements)
        if 'ftransform()' in line:
            line = line.replace('ftransform()', 'u_modelViewProjectionMatrix * a_vertex')

        # gl_ModelViewProjectionMatrix -> u_modelViewProjectionMatrix
        if 'gl_ModelViewProjectionMatrix' in line:
            line = line.replace('gl_ModelViewProjectionMatrix', 'u_modelViewProjectionMatrix')

        # gl_Vertex -> a_vertex
        if 'gl_Vertex' in line:
            line = line.replace('gl_Vertex', 'a_vertex')

        # gl_FrontColor -> v_color (vertex shader output)
        if 'gl_FrontColor' in line:
            line = line.replace('gl_FrontColor', 'v_color')

        # gl_Color -> a_color (vertex) or v_color (fragment)
        if 'gl_Color' in line:
            if is_fragment_shader:
                line = line.replace('gl_Color', 'v_color')
            else:
                line = line.replace('gl_Color', 'a_color')

        # gl_TexCoord[0].st or gl_TexCoord[0].xy -> v_texCoord
        if 'gl_TexCoord' in line:
            # Handle gl_TexCoord[0].st and gl_TexCoord[0].xy
            line = re.sub(r'gl_TexCoord\[0\]\.st', 'v_texCoord', line)
            line = re.sub(r'gl_TexCoord\[0\]\.xy', 'v_texCoord', line)
            # Handle bare gl_TexCoord[0] (less common)
            line = re.sub(r'gl_TexCoord\[0\]', 'vec4(v_texCoord, 0.0, 0.0)', line)

        # gl_MultiTexCoord0 -> a_texCoord0 (for SMAA shaders)
        if 'gl_MultiTexCoord0' in line:
            line = re.sub(r'gl_MultiTexCoord0\.st', 'a_texCoord0.st', line)
            line = re.sub(r'gl_MultiTexCoord0\.xy', 'a_texCoord0.xy', line)
            line = line.replace('gl_MultiTexCoord0', 'a_texCoord0')

        # Fix uniform int -> uniform float (GLSL ES 3.00 doesn't allow implicit int/float conversion)
        # This specifically handles u_fontTextureWidth which is multiplied with floats
        if 'uniform int ' in line:
            line = line.replace('uniform int ', 'uniform float ')

        # Fix int * float type issues: "2 * x" -> "2.0 * x"
        def fix_int_mult(match):
            int_val = match.group(1)
            var_start = match.group(2)
            return f'{int_val}.0 * {var_start}'
        line = int_mult_pattern.sub(fix_int_mult, line)

        # Fix float / int type issues: "x / 4" -> "x / 4.0"
        def fix_div_int(match):
            var_part = match.group(1)
            int_val = match.group(2)
            return f'{var_part} / {int_val}.0'
        line = div_int_pattern.sub(fix_div_int, line)

        result.append(line)

    return '\n'.join(result)


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <input.glsl> <output.glsl> <vertex|fragment>", file=sys.stderr)
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    shader_type = sys.argv[3]

    if shader_type not in ('vertex', 'fragment'):
        print(f"Error: shader type must be 'vertex' or 'fragment', got '{shader_type}'", file=sys.stderr)
        sys.exit(1)

    is_fragment = (shader_type == 'fragment')

    with open(input_path, 'r') as f:
        source = f.read()

    converted = convert_glsl120_to_es300(source, is_fragment)

    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)
    with open(output_path, 'w') as f:
        f.write(converted)


if __name__ == '__main__':
    main()
