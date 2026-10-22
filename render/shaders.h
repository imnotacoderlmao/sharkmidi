#ifndef SHADERS_H
#define SHADERS_H

#define FRAME_BLOCK \
"layout(std140, binding = 0) uniform FrameData {\n" \
"    vec3 uMetrics;\n" \
"    int uGlowEnabled;\n" \
"    int uViewStart;\n" \
"    int uViewEnd;\n" \
"    int uCurrentTick;\n" \
"    int uTransparencyEnabled;\n" \
"    int uDepthEnabled;\n" \
"    int uTrackCount;\n" \
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
"    uint rank = min(((aPackedData >> 24) << 4) | ((aPackedData >> 20) & 15u), uint(uTrackCount - 1));\n"
"    float tie = float(uint(uTrackCount - 1) - rank) / float(uTrackCount);\n"
"    float z = uDepthEnabled == 1 ? tie * 2.0 - 1.0 : 0.0; // track priority: the higher track in front\n"
"    gl_Position = vec4(x, y, z, 1.0);\n"
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

// writes the end ticks of notes into the buffer of the notes, one point per patch: aWord is where the EndTick is, in words
static const char* ScatterVertSrc =
"#version 420 core\n"
"layout(location = 0) in uint aWord;\n"
"layout(location = 1) in uint aEnd;\n"
"layout(r32ui, binding = 0) uniform writeonly uimageBuffer uRing;\n"
"void main() {\n"
"    imageStore(uRing, int(aWord), uvec4(aEnd));\n"
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

#endif