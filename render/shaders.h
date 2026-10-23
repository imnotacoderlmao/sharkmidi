#ifndef SHADERS_H
#define SHADERS_H

#include "../third_party/glad/include/glad/glad.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAME_BLOCK \
"layout(std140, binding = 0) uniform FrameData {\n" \
"    vec3 uMetrics;\n" \
"    int uGlowEnabled;\n" \
"    int uViewStart;\n" \
"    int uViewEnd;\n" \
"    int uCurrentTick;\n" \
"    int uTransparencyEnabled;\n" \
"};\n"

static const char* QuadVertSrc =
"#version 420 core\n"
"layout(location = 0) in int aStartTick;\n"
"layout(location = 1) in int aEndTick;\n"
"layout(location = 2) in uint aPackedData;\n"
FRAME_BLOCK
"uniform sampler1D uPalette;\n"
"flat out vec4 vColor;\n"
"flat out int vIsActive;\n"
"flat out float opacity;\n"
"void main() {\n"
"    int endTick = aEndTick > 0 ? aEndTick : uViewEnd;\n"
"    bool isEnd = (uint(gl_VertexID) & 1u) != 0u;\n"
"    bool isTop = ((uint(gl_VertexID) >> 1) & 1u) != 0u;\n"
"    float x = float((bool(isEnd)? endTick : aStartTick) - uViewStart) * uMetrics.x - 1.0;\n"
"    float y = uMetrics.y + float(((aPackedData >> 8) & 0xFFu) + uint(isTop)) * uMetrics.z;\n"
"    vColor = texelFetch(uPalette, int((aPackedData >> 16) & 0xFFu), 0);\n"
"    vIsActive = (uCurrentTick >= aStartTick && uCurrentTick <= endTick) ? 1 : 0;\n"
"    opacity = float(((aPackedData & 0xFFu) + 1u)) / 128.0;\n"
"    gl_Position = vec4(x, y, 0, 1.0);\n"
"}\n";

static const char* QuadFragSrc =
"#version 420 core\n"
"flat in vec4 vColor;\n"
"flat in int vIsActive;\n"
"flat in float opacity;\n"
FRAME_BLOCK
"out vec4 fragColor;\n"
"void main() {\n"
"    float note_opacity = (uTransparencyEnabled == 1) ? opacity : 1.0;\n"
"    vec3 color = vColor.rgb;\n"
"    color = (uGlowEnabled == 1 && vIsActive == 1) ? min(color * 2.0 + 0.1, vec3(1.0)) : color;\n"
"    fragColor = vec4(color, note_opacity);\n"
"}\n";

// rewrites the notes of one page of the buffer of the notes, for transform feedback: the notes come in as they are, the
// EndTick is replaced when the table uOvr (one entry per note, in the order of the pages being rewritten) has one,
// 0 keeps it. gl_VertexID is the number of the note in the buffer, uShift moves it to its place in the table
static const char* ScatterVertSrc =
"#version 420 core\n"
"layout(location = 0) in int aStartTick;\n"
"layout(location = 1) in int aEndTick;\n"
"layout(location = 2) in uint aPackedData;\n"
"uniform usamplerBuffer uOvr;\n"
"uniform int uShift;\n"
"out int outStart;\n"
"out int outEnd;\n"
"out uint outPacked;\n"
"void main() {\n"
"    uint e = texelFetch(uOvr, gl_VertexID + uShift).r;\n"
"    outStart = aStartTick;\n"
"    outEnd = e != 0u ? int(e) : aEndTick;\n"
"    outPacked = aPackedData;\n"
"    gl_Position = vec4(0.0);\n"
"}\n";

static const char* ScatterFragSrc =
"#version 420 core\n"
"void main() {}\n";

static const char* CursorVertSrc =
"#version 420 core\n"
"uniform float uCursorX;\n"
"uniform float uHalfWidthNDC;\n"
"uniform float uYBottom;\n"
"uniform float uYTop;\n"
"void main()\n"
"{\n"
"    bool isRight = (uint(gl_VertexID) & 1u) != 0u;\n"
"    bool isTop = ((uint(gl_VertexID) >> 1) & 1u) != 0u;\n"
"    float x = uCursorX + (isRight ? uHalfWidthNDC : -uHalfWidthNDC);\n"
"    float y = isTop ? uYTop : uYBottom;\n"
"    gl_Position = vec4(x, y, 0.0, 1.0);\n"
"}\n";

static const char* CursorFragSrc =
"#version 420 core\n"
"uniform vec3 uLineColor;\n"
"out vec4 fragColor;\n"
"void main() { fragColor = vec4(uLineColor, 1.0); }\n";

static GLuint compile_stage(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile error:\n%s\n", log);
        exit(1);
    }
    return shader;
}

static GLuint build_shader(const char* vert, const char* frag, int isscatter)
{
    GLuint v = compile_stage(GL_VERTEX_SHADER, vert);
    GLuint f = compile_stage(GL_FRAGMENT_SHADER, frag);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    
    if (isscatter)
    {
        const char* varyings[] = { "outStart", "outEnd", "outPacked" }; // the layout of a RenderNote
        glTransformFeedbackVaryings(prog, 3, varyings, GL_INTERLEAVED_ATTRIBS);
    }

    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "shader link error:\n%s\n", log);
        exit(1);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return prog;
}

#endif