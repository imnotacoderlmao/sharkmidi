#include "renderer.h"
#include "../parse/midistorage.h"
#include "../parse/parser.h"
#include "../third_party/glad/include/glad/glad.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(__SSSE3__)
#include <tmmintrin.h>
#define HAVE_SSSE3 1
#else
#define HAVE_SSSE3 0
#endif

typedef struct __attribute__((packed))
{
    int32_t StartTick;
    int32_t EndTick;
    uint32_t PackedData; // colorIdx:8 << 16 | key:8 << 8 | velocity:8
} RenderNote;

typedef struct
{
    int32_t ActiveAbsId;
    int32_t ActiveCount;
} KeyHeader;

#define TOTAL_KEYS (128 * 16 * 16) // key:128 * channel:16 * colorIdx:16

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

static GLuint lineShader;
static GLint u_metrics, u_viewStart, u_viewEnd, u_palette, u_glowEnabled, u_transparencyEnabled, u_currentTick;
static GLuint vao, vboBuffer, paletteTex;

static RenderNote* ring;
static int ringCap = 1 << 23;
static int deferredRingCap = -1;
static int mask;
static int headIdx = 1, tailIdx = 1;

static int paletteUploadPending = 0;

static KeyHeader* keyHeaders;

static int lookaheadTicks = 4000;
static float pixelsPerTick;
static int lastWindowTicks = -1;
static int lastSweepEnd = -1;
static int isInitialized = 0;

int WindowTicks = 2000;
int RingCap = 0;
int NotesDrawnLastFrame = 0;
int UseForceCull = 0;
int EnableGlow = 1;
int EnableTransparency = 0;

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

static GLuint build_shader(const char* vert, const char* frag)
{
    GLuint v = compile_stage(GL_VERTEX_SHADER, vert);
    GLuint f = compile_stage(GL_FRAGMENT_SHADER, frag);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
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

static const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_READ_BIT |
    GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
static const GLbitfield accessFlags = storageFlags; // same bits, different call sites in GL API

static void bind_vertex_attribs(void)
{
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vboBuffer);

    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 1, GL_INT, sizeof(RenderNote), (void*)0);
    glVertexAttribDivisor(0, 1);

    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_INT, sizeof(RenderNote), (void*)4);
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(RenderNote), (void*)8);
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void alloc_ring(int cap)
{
    mask = cap - 1;

    if (vboBuffer != 0)
    {
        glBindBuffer(GL_ARRAY_BUFFER, vboBuffer);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDeleteBuffers(1, &vboBuffer);
    }

    glGenBuffers(1, &vboBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, vboBuffer);

    size_t totalBytes = (size_t)cap * sizeof(RenderNote);
    glBufferStorage(GL_ARRAY_BUFFER, totalBytes, NULL, storageFlags);
    ring = (RenderNote*)glMapBufferRange(GL_ARRAY_BUFFER, 0, totalBytes, accessFlags);

    bind_vertex_attribs();
    ringCap = cap;
}

static void resize_ring(int newCap)
{
    if (newCap < 0) return;
    int newMask = newCap - 1;

    size_t totalBytes = (size_t)newCap * sizeof(RenderNote);
    GLuint newBuffer;
    glGenBuffers(1, &newBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, newBuffer);
    glBufferStorage(GL_ARRAY_BUFFER, totalBytes, NULL, storageFlags);
    RenderNote* newRing = (RenderNote*)glMapBufferRange(GL_ARRAY_BUFFER, 0, totalBytes, accessFlags);

    if (headIdx > tailIdx)
    {
        int remaining = headIdx - tailIdx;
        int absId = tailIdx;
        while (remaining > 0)
        {
            int oldIdx = absId & mask;
            int newIdx = absId & newMask;
            int chunk = remaining;
            if (ringCap - oldIdx < chunk) chunk = ringCap - oldIdx;
            if (newCap - newIdx < chunk) chunk = newCap - newIdx;
            memcpy(newRing + newIdx, ring + oldIdx, (size_t)chunk * sizeof(RenderNote));
            absId += chunk;
            remaining -= chunk;
        }
    }

    if (vboBuffer != 0)
    {
        glBindBuffer(GL_ARRAY_BUFFER, vboBuffer);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDeleteBuffers(1, &vboBuffer);
    }

    vboBuffer = newBuffer;
    ring = newRing;
    mask = newMask;
    ringCap = newCap;

    bind_vertex_attribs();
}

void Renderer_Init(void)
{
    if (isInitialized) return;

    lineShader = build_shader(LineVertSrc, LineFragSrc);
    u_metrics = glGetUniformLocation(lineShader, "uMetrics");
    u_viewStart = glGetUniformLocation(lineShader, "uViewStart");
    u_viewEnd = glGetUniformLocation(lineShader, "uViewEnd");
    u_palette = glGetUniformLocation(lineShader, "uPalette");
    u_glowEnabled = glGetUniformLocation(lineShader, "uGlowEnabled");
    u_transparencyEnabled = glGetUniformLocation(lineShader, "uTransparencyEnabled");
    u_currentTick = glGetUniformLocation(lineShader, "uCurrentTick");

    glUseProgram(lineShader);
    glUniform1i(u_palette, 0);
    glUseProgram(0);

    keyHeaders = calloc(TOTAL_KEYS, sizeof(KeyHeader));

    glGenVertexArrays(1, &vao);
    glGenTextures(1, &paletteTex);

    alloc_ring(ringCap);
    isInitialized = 1;
}

void Renderer_InitForMIDI(void)
{
    isInitialized = 0;
    memset(keyHeaders, 0, TOTAL_KEYS * sizeof(KeyHeader));
    headIdx = 1;
    tailIdx = 1;
    lastSweepEnd = -1;
    lastWindowTicks = -1;
    paletteUploadPending = 1;
    isInitialized = 1;
}

void Renderer_ResetForUnload(void)
{
    isInitialized = 0;
    headIdx = 1;
    tailIdx = 1;
    lastSweepEnd = -1;
    lastWindowTicks = -1;
    if (ringCap != (1 << 23))
        deferredRingCap = 1 << 23;
}

void Renderer_Dispose(void)
{
    isInitialized = 0;
    free(keyHeaders);
    keyHeaders = NULL;
    if (vboBuffer != 0)
    {
        glBindBuffer(GL_ARRAY_BUFFER, vboBuffer);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDeleteBuffers(1, &vboBuffer);
        vboBuffer = 0;
        ring = NULL;
    }
    if (paletteTex != 0) { glDeleteTextures(1, &paletteTex); paletteTex = 0; }
    if (vao != 0) { glDeleteVertexArrays(1, &vao); vao = 0; }
    if (lineShader != 0) { glDeleteProgram(lineShader); lineShader = 0; }
}


static inline void process_one_event(uint8_t* messages, int64_t offset, uint8_t status, uint8_t key, uint8_t track,
    KeyHeader* keyheader, RenderNote* ringLocal, int maskLocal, int tick, int* headLocal)
{
    uint32_t headerIdx = (track | (status & 0x0Fu)) << 7 | key;
    KeyHeader* header = &keyheader[headerIdx];
    uint32_t statusHigh = status & 0xF0u;

    if (statusHigh == 0x90)
    {
        if (header->ActiveCount == 0)
        {
            header->ActiveAbsId = *headLocal;
            uint32_t color = headerIdx >> 7;
            uint8_t velocity = messages[offset * 3 + 2];
            ringLocal[*headLocal & maskLocal] = (RenderNote){
                .StartTick = tick,
                .EndTick = 0,
                .PackedData = (color << 16) | ((uint32_t)key << 8) | velocity
            };
            (*headLocal)++;
        }
        header->ActiveCount++;
    }
    else if (statusHigh == 0x80)
    {
        if (header->ActiveCount > 0)
        {
            header->ActiveCount--;
            if (header->ActiveCount == 0)
            {
                int absid = header->ActiveAbsId;
                if (absid >= *headLocal - (maskLocal + 1))
                    ringLocal[absid & maskLocal].EndTick = tick;
            }
        }
    }
}

#if HAVE_SSSE3
static const uint8_t StatusShuffleMaskBytes[16] __attribute__((aligned(16))) = {
    0, 3, 6, 9, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
};
static const uint8_t KeyShuffleMaskBytes[16] __attribute__((aligned(16))) = {
    1, 4, 7, 10, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
};
#endif

static int64_t process_tick_events(uint8_t* messages, uint8_t* tracks, KeyHeader* keyheader,
    RenderNote* ringLocal, int maskLocal, int64_t currentOffset, int64_t nextOffset, int tick, int* headLocal)
{
#if HAVE_SSSE3
    __m128i statusMask = _mm_load_si128((const __m128i*)StatusShuffleMaskBytes);
    __m128i keyMask    = _mm_load_si128((const __m128i*)KeyShuffleMaskBytes);

    while (nextOffset - currentOffset >= 4)
    {
        uint8_t* synthev = messages + currentOffset * 3;
        __m128i raw = _mm_lddqu_si128((const __m128i*)synthev);

        __m128i statusVec = _mm_shuffle_epi8(raw, statusMask);
        __m128i keyVec    = _mm_shuffle_epi8(raw, keyMask);

        uint32_t statusPacked = (uint32_t)_mm_cvtsi128_si32(statusVec);
        uint32_t keyPacked    = (uint32_t)_mm_cvtsi128_si32(keyVec);
        uint32_t trackPacked  = tracks != NULL ? *(uint32_t*)(tracks + currentOffset) : 0;

        for (int lane = 0; lane < 4; lane++)
        {
            uint8_t status = (uint8_t)(statusPacked >> (lane * 8));
            uint8_t key    = (uint8_t)(keyPacked >> (lane * 8));
            uint8_t track  = (uint8_t)(trackPacked >> (lane * 8));

            process_one_event(messages, currentOffset + lane, status, key, track,
                keyheader, ringLocal, maskLocal, tick, headLocal);
        }
        currentOffset += 4;
    }
#endif

    while (currentOffset < nextOffset)
    {
        uint8_t* synthev = messages + currentOffset * 3;
        uint8_t track = tracks != NULL ? tracks[currentOffset] : 0;
        process_one_event(messages, currentOffset, synthev[0], synthev[1], track,
            keyheader, ringLocal, maskLocal, tick, headLocal);
        currentOffset++;
    }
    return currentOffset;
}

static void sweep_range(int fromTick, int toTick)
{
    TickGroup* group = timingArr;
    uint8_t* messages = (uint8_t*)eventArr;
    //uint8_t* tracks = trackArr; // NULL for now
    uint8_t* tracks = NULL;

    int from = fromTick < maxTick ? fromTick : maxTick;
    int limit = toTick < maxTick ? toTick : maxTick;
    int headLocal = headIdx;

    int64_t currentOffset = group[from].event_offset;
    for (int tick = from; tick <= limit; tick++)
    {
        if (group[tick].tick == INT32_MAX)
            break;

        int64_t nextOffset = group[tick + 1].event_offset;
        while (headLocal - tailIdx + (nextOffset - currentOffset) >= mask + 1)
        {
            headIdx = headLocal;
            resize_ring((mask + 1) * 2);
        }
        currentOffset = process_tick_events(messages, tracks, keyHeaders, ring, mask,
            currentOffset, nextOffset, tick, &headLocal);
    }
    headIdx = headLocal;
}

static void advance_tail(int viewStart)
{
    int safeTail = headIdx - ringCap;
    if (tailIdx < safeTail) tailIdx = safeTail;

    int forceCullThresh = lookaheadTicks * 2;
    if (forceCullThresh > 65535) forceCullThresh = 65535;
    int forceCull = UseForceCull && NotesDrawnLastFrame > 262144;
    int forceCullBefore = viewStart - forceCullThresh;

    int maskLocal = mask;
    int headLocal = headIdx;
    int tailLocal = tailIdx;

    while (tailLocal < headLocal)
    {
        int physIdx = tailLocal & maskLocal;
        RenderNote note = ring[physIdx];
        int isopen = note.EndTick == 0;
        int startTick = note.StartTick;

        if ((!isopen && note.EndTick < viewStart) || (forceCull && startTick < forceCullBefore))
            tailLocal++;
        else
            break;
    }
    tailIdx = tailLocal;
}

void Renderer_Render(int screenWidth, int screenHeight, int32_t tick, int pad)
{
    if (!midiloaded || !isInitialized) return;

    if (paletteUploadPending)
    {
        uint8_t paletteData[256 * 3];
        for (int i = 0; i < 256; i++)
        {
            uint32_t c = (uint32_t)(rand() % (0x1000000 - 0x808080) + 0x808080);
            paletteData[i * 3 + 0] = (c >> 16) & 0xFF;
            paletteData[i * 3 + 1] = (c >> 8) & 0xFF;
            paletteData[i * 3 + 2] = c & 0xFF;
        }
        glBindTexture(GL_TEXTURE_1D, paletteTex);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB8, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, paletteData);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_1D, 0);
        paletteUploadPending = 0;
    }

    if (deferredRingCap > 0)
    {
        resize_ring(deferredRingCap);
        deferredRingCap = -1;
    }

    int half = WindowTicks >> 1;
    int viewStart = tick - half; if (viewStart < 0) viewStart = 0; if (viewStart > maxTick) viewStart = maxTick;
    int viewEnd = tick + half; if (viewEnd < 0) viewEnd = 0; if (viewEnd > maxTick) viewEnd = maxTick;

    if (WindowTicks != lastWindowTicks)
    {
        pixelsPerTick = 2.0f / WindowTicks;
        lastWindowTicks = WindowTicks;
        lookaheadTicks = WindowTicks / 2 < 2000 ? WindowTicks / 2 : 2000;
    }

    int sweepEnd = viewEnd + lookaheadTicks;
    int incremental = lastSweepEnd >= 0 && sweepEnd >= lastSweepEnd && (sweepEnd - lastSweepEnd) < WindowTicks;

    if (!incremental)
    {
        headIdx = 1;
        tailIdx = 1;
        memset(keyHeaders, 0, TOTAL_KEYS * sizeof(KeyHeader));
        int from = viewStart - WindowTicks;
        sweep_range(from > 0 ? from : 0, sweepEnd);
    }
    else
    {
        sweep_range(lastSweepEnd + 1, sweepEnd);
    }

    lastSweepEnd = sweepEnd;
    advance_tail(viewStart);

    NotesDrawnLastFrame = headIdx - tailIdx;
    RingCap = ringCap;

    if (NotesDrawnLastFrame > 0)
    {
        float yBottom = -1.0f + 2.0f * pad / screenHeight;
        float yTop = 1.0f - 2.0f * pad / screenHeight;
        float yStep = (yTop - yBottom) / 128.0f;

        glViewport(0, 0, screenWidth, screenHeight);
        glUseProgram(lineShader);

        glUniform3f(u_metrics, pixelsPerTick, yBottom, yStep);
        glUniform1i(u_viewStart, viewStart);
        glUniform1i(u_viewEnd, viewEnd);
        glUniform1i(u_glowEnabled, EnableGlow ? 1 : 0);
        glUniform1i(u_transparencyEnabled, EnableTransparency ? 1 : 0);
        glUniform1i(u_currentTick, tick);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_1D, paletteTex);

        glBindVertexArray(vao);

        int startIdx = tailIdx & mask;
        if (startIdx + NotesDrawnLastFrame <= ringCap)
        {
            glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, NotesDrawnLastFrame, startIdx);
        }
        else
        {
            int firstChunk = ringCap - startIdx;
            int secondChunk = NotesDrawnLastFrame - firstChunk;
            glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, firstChunk, startIdx);
            glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, secondChunk, 0);
        }

        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_1D, 0);
        glUseProgram(0);
    }
}