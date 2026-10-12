#include "renderer.h"
#include "shaders.h"
#include "../parse/midistorage.h"
#include "../parse/parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../misc/util.h"

#if defined(__SSSE3__)
#include <tmmintrin.h>
#define HAVE_SSSE3 1
#else
#define HAVE_SSSE3 0
#endif

typedef struct
{
    int32_t StartTick;
    int32_t EndTick;
    uint32_t PackedData; // colorIdx:8 << 16 | key:8 << 8 | velocity:8
} RenderNote;

typedef struct {
    float uMetrics[3];
    float uGlowEnabled;
    int uViewStart;
    int uViewEnd;
    int uCurrentTick;
    int uTransparencyEnabled;
} FrameUniforms;

#define TOTAL_KEYS (128 * 16 * 16) // key:128 * channel:16 * colorIdx:16
#define MAX_WINDOW_TICKS (1 << 27) // keeps viewStart + WindowTicks inside int range

static GLuint quadShader, cursorShader, frameUBO;
static GLint uc_cursorX, uc_halfWidth, uc_yBottom, uc_yTop, uc_lineColor;
static GLint u_metrics, u_viewStart, u_viewEnd, u_palette, u_glowEnabled, u_transparencyEnabled, u_currentTick;
static GLuint vao, cursorVAO, vboBuffer, paletteTex;

static RenderNote* ring;
static int ringCapacity = 1 << 23;
static int deferredRingCap = -1;
static int mask;
static int headIdx = 1, tailIdx = 1;

static int paletteUploadPending = 0;

static int32_t* khdr_absid;
static uint16_t* khdr_activecount;

static int lookaheadTicks = 500;
static float pixelsPerTick;
static int lastWindowTicks = -1;
static int lastSweepEnd = -1;
static int isInitialized = 0;

int WindowTicks = 2000;
int RingCap = 0;
int NotesDrawnLastFrame = 0;
int UseForceCull = 1;
int EnableGlow = 1;
int EnableTransparency = 0;
int stationarynotes = 0;

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

GLuint build_shader(const char* vert, const char* frag)
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

static void resize_ring(int newCap)
{
    if (newCap < 0) return;
    int newMask = newCap - 1;

    size_t totalBytes = (size_t)newCap * sizeof(RenderNote);
    GLuint newBuffer;
    glGenBuffers(1, &newBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, newBuffer);
    glBufferStorage(GL_ARRAY_BUFFER, totalBytes, NULL, storageFlags);
    RenderNote* newRing = (RenderNote*)glMapBufferRange(GL_ARRAY_BUFFER, 0, totalBytes, storageFlags);

    if (headIdx > tailIdx)
    {
        int remaining = headIdx - tailIdx;
        int absId = tailIdx;
        while (remaining > 0)
        {
            int oldIdx = absId & mask;
            int newIdx = absId & newMask;
            int chunk = remaining;
            if (ringCapacity - oldIdx < chunk) chunk = ringCapacity - oldIdx;
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
    ringCapacity = newCap;

    bind_vertex_attribs();
}

void Renderer_Init(void)
{
    if (isInitialized) return;

    quadShader = build_shader(QuadVertSrc, QuadFragSrc);
    cursorShader = build_shader(CursorVertSrc, CursorFragSrc);
    glGenVertexArrays(1, &cursorVAO); 

    // Setup Uniform Buffer Object (UBO) for zero driver-overhead uniform updates
    glGenBuffers(1, &frameUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, frameUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(FrameUniforms), NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    GLuint blockIndex = glGetUniformBlockIndex(quadShader, "FrameData");
    glUniformBlockBinding(quadShader, blockIndex, 0);

    uc_cursorX   = glGetUniformLocation(cursorShader, "uCursorX");
    uc_halfWidth = glGetUniformLocation(cursorShader, "uHalfWidthNDC");
    uc_yBottom   = glGetUniformLocation(cursorShader, "uYBottom");
    uc_yTop      = glGetUniformLocation(cursorShader, "uYTop");
    uc_lineColor = glGetUniformLocation(cursorShader, "uLineColor");

    glUseProgram(quadShader);
    glUniform1i(glGetUniformLocation(quadShader, "uPalette"), 0);
    glUseProgram(0);

    khdr_absid = calloc(TOTAL_KEYS, sizeof(int32_t));
    khdr_activecount = calloc(TOTAL_KEYS, sizeof(uint16_t));

    glGenVertexArrays(1, &vao);
    glGenTextures(1, &paletteTex);

    resize_ring(ringCapacity);
    isInitialized = 1;
}

void Renderer_InitForMIDI(void)
{
    isInitialized = 0;
    memset(khdr_activecount,0, TOTAL_KEYS * sizeof(uint16_t));
    memset(khdr_absid,0, TOTAL_KEYS * sizeof(int32_t));
    headIdx = 1;
    tailIdx = 1;
    lastSweepEnd = -1;
    lastWindowTicks = -1;
    paletteUploadPending = 1;
    isInitialized = 1;
    puts("renderer is ready");
}

void Renderer_ResetForUnload(void)
{
    isInitialized = 0;
    headIdx = 1;
    tailIdx = 1;
    lastSweepEnd = -1;
    lastWindowTicks = -1;
    if (ringCapacity != (1 << 23))
        deferredRingCap = 1 << 23;
}

void Renderer_Dispose(void)
{
    isInitialized = 0;
    free(khdr_absid);
    khdr_absid = NULL;
    free(khdr_activecount);
    khdr_activecount = NULL;
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
    if (quadShader != 0) { glDeleteProgram(quadShader); quadShader = 0; }
    glDeleteVertexArrays(1, &cursorVAO);
    glDeleteProgram(cursorShader);
}

static inline void process_one_event(uint8_t* messages, int64_t offset, uint8_t status, uint8_t key, uint8_t track,
    int32_t* khdr_absid, uint16_t* khdr_activecount, RenderNote* ringLocal, int maskLocal, int64_t tick, int* headLocal)
{
    uint32_t headerIdx = (track | (status & 0x0Fu)) << 7 | key;
    uint16_t count = khdr_activecount[headerIdx];
    uint32_t statusHigh = status & 0xF0u;

    if (statusHigh == 0x90)
    {
        if (count == 0)
        {
            khdr_absid[headerIdx] = *headLocal;
            uint32_t color = headerIdx >> 7;
            uint8_t velocity = messages[offset * 3 + 2];
            ringLocal[*headLocal & maskLocal] = (RenderNote){
                .StartTick = tick,
                .EndTick = 0,
                .PackedData = (color << 16) | ((uint32_t)key << 8) | velocity
            };
            (*headLocal)++;
        }
        count++;
    }
    else if (statusHigh == 0x80)
    {
        if (count > 0)
        {
            count--;
            if (count == 0)
            {
                int absid = khdr_absid[headerIdx];
                if (absid >= *headLocal - (maskLocal + 1))
                    ringLocal[absid & maskLocal].EndTick = tick;
            }
        }
    }
    khdr_activecount[headerIdx] = count;
}

#if HAVE_SSSE3
static const uint8_t StatusShuffleMaskBytes[16] __attribute__((aligned(16))) = {
    0, 3, 6, 9, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
};
static const uint8_t KeyShuffleMaskBytes[16] __attribute__((aligned(16))) = {
    1, 4, 7, 10, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
};
#endif

static int64_t process_run_events(uint8_t* messages, uint8_t track, int32_t* khdr_absid, uint16_t* khdr_activecount,
    RenderNote* ringLocal, int maskLocal, int64_t currentOffset, int64_t runEnd, int64_t tick, int* headLocal)
{
#if HAVE_SSSE3
    __m128i statusMask = _mm_load_si128((const __m128i*)StatusShuffleMaskBytes);
    __m128i keyMask    = _mm_load_si128((const __m128i*)KeyShuffleMaskBytes);

    while (runEnd - currentOffset >= 4)
    {
        uint8_t* synthev = messages + currentOffset * 3;
        __m128i raw = _mm_lddqu_si128((const __m128i*)synthev);

        __m128i statusVec = _mm_shuffle_epi8(raw, statusMask);
        __m128i keyVec    = _mm_shuffle_epi8(raw, keyMask);

        uint32_t statusPacked = (uint32_t)_mm_cvtsi128_si32(statusVec);
        uint32_t keyPacked    = (uint32_t)_mm_cvtsi128_si32(keyVec);

        for (int lane = 0; lane < 4; lane++)
        {
            uint8_t status = (uint8_t)(statusPacked >> (lane * 8));
            uint8_t key    = (uint8_t)(keyPacked >> (lane * 8));

            process_one_event(messages, currentOffset + lane, status, key, track,
                khdr_absid, khdr_activecount, ringLocal, maskLocal, tick, headLocal);
        }
        currentOffset += 4;
    }
#endif

    while (currentOffset < runEnd)
    {
        uint8_t* synthev = messages + currentOffset * 3;
        process_one_event(messages, currentOffset, synthev[0], synthev[1], track,
            khdr_absid, khdr_activecount, ringLocal, maskLocal, tick, headLocal);
        currentOffset++;
    }
    return currentOffset;
}

static int64_t find_active_tick_idx(int64_t targetTick)
{
    int64_t low = 0;
    int64_t high = (int64_t)activetickcount - 1;
    int64_t ans = activetickcount;

    while (low <= high)
    {
        int64_t mid = low + (high - low) / 2;
        if (timingArr[mid].tick >= targetTick)
        {
            ans = mid;
            high = mid - 1;
        }
        else
            low = mid + 1;
    }
    return ans;
}

static int64_t find_track_group_idx(int64_t targetOffset)
{
    int64_t low = 0;
    int64_t high = (int64_t)trackGroups.count - 1;
    int64_t ans = 0;

    while (low <= high)
    {
        int64_t mid = low + (high - low) / 2;
        if (trackGroups.data[mid].event_offset <= targetOffset)
        {
            ans = mid;
            low = mid + 1;
        }
        else
            high = mid - 1;
    }
    return ans;
}

static void sweep_range(int64_t fromTick, int64_t toTick)
{
    if (activetickcount == 0 || timingArr == NULL) return;

    uint8_t* messages = (uint8_t*)eventArr;

    int64_t limit = toTick < maxTick ? toTick : maxTick;
    int headLocal = headIdx;

    int64_t idx = find_active_tick_idx(fromTick);
    if (idx >= activetickcount) return;

    int64_t currentOffset = timingArr[idx].event_offset;
    int64_t groupIdx = (trackGroups.data && trackGroups.count > 0) ? find_track_group_idx(currentOffset) : 0;

    while ((groupIdx < trackGroups.count) && (trackGroups.data[groupIdx].event_offset + trackGroups.data[groupIdx].count <= currentOffset))
        groupIdx++;

    while (idx < activetickcount && timingArr[idx].tick <= limit)
    {
        int64_t tick = timingArr[idx].tick;
        int64_t nextOffset = timingArr[idx + 1].event_offset;

        while (headLocal - tailIdx + (nextOffset - currentOffset) >= mask + 1)
        {
            headIdx = headLocal;
            resize_ring((mask + 1) * 2);
        }

        if (trackGroups.data == NULL || trackGroups.count == 0)
        {
            currentOffset = process_run_events(messages, 0, khdr_absid, khdr_activecount, ring, mask,
                currentOffset, nextOffset, tick, &headLocal);
        }
        else
        {
            while (currentOffset < nextOffset)
            {
                if (groupIdx >= trackGroups.count)
                {
                    currentOffset = process_run_events(messages, 0, khdr_absid, khdr_activecount, ring, mask,
                        currentOffset, nextOffset, tick, &headLocal);
                    break;
                }

                TrackGroup* group = &trackGroups.data[groupIdx];
                int64_t runEnd = group->event_offset + group->count;
                if (runEnd > nextOffset) runEnd = nextOffset;

                uint8_t track = group->track;
                currentOffset = process_run_events(messages, track, khdr_absid, khdr_activecount, ring, mask,
                    currentOffset, runEnd, tick, &headLocal);

                if (currentOffset >= group->event_offset + group->count)
                    groupIdx++;
            }
        }
        idx++;
    }
    headIdx = headLocal;
}

static void advance_tail(int viewStart)
{
    int safeTail = headIdx - ringCapacity;
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

static void upload_palette(void)
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

static void draw_notes(int viewStart, int viewEnd, int32_t tick, float yBottom, float yTop, float pixelsPerTick)
{
    float yStep = (yTop - yBottom) / 128.0f;
    FrameUniforms uniforms = {
        .uMetrics = { pixelsPerTick, yBottom, yStep },
        .uGlowEnabled = (float)EnableGlow,
        .uViewStart = viewStart,
        .uViewEnd = viewEnd,
        .uCurrentTick = tick,
        .uTransparencyEnabled = EnableTransparency
    };

    glBindBuffer(GL_UNIFORM_BUFFER, frameUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(FrameUniforms), &uniforms);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameUBO);

    glUseProgram(quadShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_1D, paletteTex);
    glBindVertexArray(vao);

    int startIdx = tailIdx & mask;
    if (startIdx + NotesDrawnLastFrame <= ringCapacity)
    {
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, NotesDrawnLastFrame, startIdx);
    }
    else
    {
        int firstChunk = ringCapacity - startIdx;
        int secondChunk = NotesDrawnLastFrame - firstChunk;
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, firstChunk, startIdx);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, secondChunk, 0);
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_1D, 0);
    glUseProgram(0);
}

static void draw_cursor(float cursorX, int screenWidth, float yBottom, float yTop)
{
    glUseProgram(cursorShader);
    glUniform1f(uc_cursorX, cursorX);
    glUniform1f(uc_halfWidth, 1.0f / (float)screenWidth);
    glUniform1f(uc_yBottom, yBottom);
    glUniform1f(uc_yTop, yTop);
    glUniform3f(uc_lineColor, 1.0f, 0.0f, 0.0f);

    glBindVertexArray(cursorVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
}

void Renderer_Render(int screenWidth, int screenHeight, int32_t tick, int pad)
{
    if (!midiloaded || !isInitialized) return;

    if (paletteUploadPending)
        upload_palette();

    if (deferredRingCap > 0)
    {
        resize_ring(deferredRingCap);
        deferredRingCap = -1;
    }

    // zoom keys and dynamic scroll can push this to 0 (division by zero below) or overflow int math
    WindowTicks = MAX(1, MIN(WindowTicks, MAX_WINDOW_TICKS));

    int viewStart = stationarynotes ? (tick / WindowTicks) * WindowTicks : (tick - (WindowTicks >> 1));
    viewStart = MAX(0, MIN(viewStart, maxTick));
    int viewEnd = MIN(viewStart + WindowTicks, maxTick);

    if (WindowTicks != lastWindowTicks)
    {
        pixelsPerTick = 2.0f / WindowTicks;
        lastWindowTicks = WindowTicks;
        lookaheadTicks = WindowTicks / 2 < 250 ? WindowTicks / 2 : 250;
    }

    int sweepEnd = viewEnd + lookaheadTicks;
    int incremental = lastSweepEnd >= 0 && sweepEnd >= lastSweepEnd && (sweepEnd - lastSweepEnd) < WindowTicks;

    if (!incremental)
    {
        headIdx = 1;
        tailIdx = 1;
        memset(khdr_activecount,0, TOTAL_KEYS * sizeof(uint16_t));
        memset(khdr_absid,0, TOTAL_KEYS * sizeof(int32_t));
        sweep_range(MAX(viewStart - WindowTicks, 0), sweepEnd);
    }
    else
    {
        sweep_range(lastSweepEnd + 1, sweepEnd);
    }

    lastSweepEnd = sweepEnd;
    advance_tail(viewStart);

    NotesDrawnLastFrame = headIdx - tailIdx;
    RingCap = ringCapacity;

    float yBottom = -1.0f + 2.0f * pad / screenHeight;
    float yTop = 1.0f - 2.0f * pad / screenHeight;

    if (NotesDrawnLastFrame > 0)
        draw_notes(viewStart, viewEnd, tick, yBottom, yTop, pixelsPerTick);
    draw_cursor((float)(tick - viewStart) * pixelsPerTick - 1.0f, screenWidth, yBottom, yTop);
}
