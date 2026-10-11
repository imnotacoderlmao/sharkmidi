#ifndef SHADERS_H
#define SHADERS_H

static const char* LineVertSrc =
"#version 420 core\n"
"in int aStartTick;\n"
"in int aEndTick;\n"
"in uint aPackedData;\n"
"uniform vec3 uMetrics;\n"
"uniform int uViewStart;\n"
"uniform int uViewEnd;\n"
"uniform int uCurrentTick;\n"
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
"    vColor = texelFetch(uPalette, int(aPackedData >> 16), 0);\n"
"    vIsActive = (uCurrentTick >= aStartTick && uCurrentTick <= endTick) ? 1 : 0;\n"
"    opacity = float(((aPackedData & 0xFFu) + 1u)) / 128.0;\n"
"    gl_Position = vec4(x, y, 0, 1.0);\n"
"}\n";

static const char* LineFragSrc =
"#version 420 core\n"
"flat in vec4 vColor;\n"
"flat in int vIsActive;\n"
"flat in float opacity;\n"
"uniform int uGlowEnabled;\n"
"uniform int uTransparencyEnabled;\n"
"out vec4 fragColor;\n"
"void main() {\n"
"    float note_opacity = (uTransparencyEnabled == 1) ? opacity : 1.0;\n"
"    vec3 color = vColor.rgb;\n"
"    color = (uGlowEnabled == 1 && vIsActive == 1) ? min(color * 2.0 + 0.1, vec3(1.0)) : color;\n"
"    fragColor = vec4(color, note_opacity);\n"
"}\n";

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

#endif
