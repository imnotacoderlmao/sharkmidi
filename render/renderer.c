#include "renderer.h"
#include "shaders.h"
#include "../parse/midistorage.h"
#include "../parse/parser.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../misc/util.h"
#include "../third_party/conmidi_digit_formatter.h"

typedef struct
{
    int32_t StartTick;
    int32_t EndTick;
    uint32_t PackedData; // track>>4:8 << 24 | colorIdx:8 << 16 (its high nibble is the track's low 4 bits) | key:8 << 8 | velocity:8
} RenderNote;

typedef struct {
    float uMetrics[3];
    int32_t uGlowEnabled;
    int32_t uViewStart;
    int32_t uViewEnd;
    int32_t uCurrentTick;
    int32_t uTransparencyEnabled;
    int32_t uDepthEnabled;
    int32_t uTrackCount; // tracks seen so far, the tie break of the depth is spread over them
    int32_t pad[2];      // the std140 block is 48 bytes
} FrameUniforms;

typedef struct { 
    int32_t openIdx; // while count > 0: this note's slot in openNotes
    int32_t count; 
} KeyHeader;

#define MAX_WINDOW_TICKS (1 << 27) // keeps viewStart + WindowTicks inside int range

static GLuint quadShader, cursorShader, frameUBO;
static GLint uc_cursorX, uc_halfWidth, uc_yBottom, uc_yTop, uc_lineColor;
static GLuint vao, cursorVAO, vboBuffer, paletteTex, openVao, openVbo;

// notes live in two places. a note that is sounding (its note off has not come) is in openNotes, a small array that is
// drawn from every frame with its end at the right edge. once the note off comes it is moved into the ring, which is a
// buffer in video memory the CPU only writes to: notes go in when they end, so the ring is in the order of end ticks,
// and its front can be dropped a chunk at a time with a small table (chunkEnd) instead of reading the notes back
#ifndef RENDER_RING_MIN
#define RENDER_RING_MIN (1 << 20)
#endif
#define BATCH_NOTES (1 << 16)
#define CHUNK_SHIFT 12
static RenderNote batch[BATCH_NOTES]; // ended notes on their way to the ring
static int batchN;
static RenderNote* openNotes;
static KeyHeader** openOwner;         // the key header of each open note
static int openCount, openCap;
static int openVboCap;
static int ringCapacity = RENDER_RING_MIN;
static int deferredRingCap = -1;
static int mask;
static int64_t ringHead, ringTail;    // absolute note numbers, the place in the buffer is the number & mask
static int32_t* chunkEnd;             // for each chunk of the ring: the end tick of the last note stored in it
static int chunkMask;
static int ringGrowFailed;

static int paletteUploadPending = 0;

// overlap tracking: every (track, channel) that plays gets a block of 128 key headers, so notes of different tracks
// or channels never share a header. a block is made when its (track, channel) first plays, from chunks of KB_CHUNK
// blocks, and kbTab, indexed by track << 4 | channel, finds it. only the track table and the used blocks cost memory
#define KB_CHUNK 1024
typedef struct { void* base; KeyHeader* blocks; } KbChunk;
static KeyHeader** kbTab;
static int64_t kbTracks;     // tracks kbTab has room for
#define MAX_RANK 4095         // tracks above this share the last rank when notes start on the same tick
static uint32_t maxTrackSeen; // highest track (capped at MAX_RANK) a sweep has met
static KbChunk* kbChunks;
static int kbChunkCount, kbChunkCap;
static int kbUsed;           // blocks used in the last chunk

static void kb_release(void)
{
    for (int i = 0; i < kbChunkCount; i++)
        free(kbChunks[i].base);
    free(kbChunks);
    free(kbTab);
    kbChunks = NULL;
    kbTab = NULL;
    kbChunkCount = kbChunkCap = kbUsed = 0;
    kbTracks = 0;
    maxTrackSeen = 0;
}

// forgets every note, keeps the blocks
static void kb_zero(void)
{
    for (int i = 0; i < kbChunkCount; i++)
        memset(kbChunks[i].blocks, 0, (i + 1 < kbChunkCount ? KB_CHUNK : kbUsed) * 128 * sizeof(KeyHeader));
}

static KeyHeader* kb_alloc(void)
{
    if (kbChunkCount == 0 || kbUsed == KB_CHUNK)
    {
        if (kbChunkCount == kbChunkCap)
        {
            kbChunkCap = kbChunkCap ? kbChunkCap * 2 : 16;
            kbChunks = realloc(kbChunks, kbChunkCap * sizeof(KbChunk));
        }
        void* base = calloc((size_t)KB_CHUNK * 128 * sizeof(KeyHeader) + 64, 1);
        kbChunks[kbChunkCount].base = base;
        kbChunks[kbChunkCount].blocks = (KeyHeader*)(((uintptr_t)base + 63) & ~(uintptr_t)63); // 64 byte aligned: a block is 16 cache lines
        kbChunkCount++;
        kbUsed = 0;
    }
    return &kbChunks[kbChunkCount - 1].blocks[(size_t)(kbUsed++) * 128];
}

static void kb_grow(uint32_t track)
{
    int64_t n = kbTracks ? kbTracks : 16;
    while (n <= (int64_t)track) n *= 2;
    kbTab = realloc(kbTab, (size_t)n * 16 * sizeof(KeyHeader*));
    memset(kbTab + kbTracks * 16, 0, (size_t)(n - kbTracks) * 16 * sizeof(KeyHeader*));
    kbTracks = n;
}

static int lookaheadTicks = 500;
static float pixelsPerTick;
static int lastWindowTicks = -1;
static int lastSweepEnd = -1;
static int lastViewStart;
static int isInitialized = 0;

int WindowTicks = 2000;
int RingCap = 0;
int NotesDrawnLastFrame = 0;
int UseForceCull = 1;
int EnableGlow = 1;
int EnableTransparency = 0;
int stationarynotes = 0;
int UseDepthOrder = 1;
int StationaryBars = 1;

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

static void bind_vertex_attribs(GLuint array, GLuint buffer)
{
    glBindVertexArray(array);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);

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

// a new ring of newCap notes (a power of two, at least one chunk), the notes in the old one are copied over on the GPU.
// returns 0, and changes nothing, when there is no memory for it
static int gpu_ring_resize(int newCap)
{
    GLuint newBuffer;
    glGenBuffers(1, &newBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, newBuffer);
    while (glGetError() != GL_NO_ERROR) {}
    glBufferStorage(GL_ARRAY_BUFFER, (GLsizeiptr)newCap * sizeof(RenderNote), NULL, GL_DYNAMIC_STORAGE_BIT); // not mapped: lives in video memory
    int32_t* newChunks = (int32_t*)malloc((size_t)(newCap >> CHUNK_SHIFT) * sizeof(int32_t));
    if (glGetError() != GL_NO_ERROR || !newChunks)
    {
        free(newChunks);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &newBuffer);
        return 0;
    }
    int newMask = newCap - 1, newChunkMask = (newCap >> CHUNK_SHIFT) - 1;

    if (vboBuffer != 0 && ringHead > ringTail)
    {
        glBindBuffer(GL_COPY_READ_BUFFER, vboBuffer);
        glBindBuffer(GL_COPY_WRITE_BUFFER, newBuffer);
        int64_t remaining = ringHead - ringTail, absId = ringTail;
        while (remaining > 0)
        {
            int oldIdx = (int)(absId & mask);
            int newIdx = (int)(absId & newMask);
            int64_t chunk = remaining;
            if (ringCapacity - oldIdx < chunk) chunk = ringCapacity - oldIdx;
            if (newCap - newIdx < chunk) chunk = newCap - newIdx;
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)oldIdx * sizeof(RenderNote), (GLintptr)newIdx * sizeof(RenderNote), (GLsizeiptr)chunk * sizeof(RenderNote));
            absId += chunk;
            remaining -= chunk;
        }
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        for (int64_t c = ringTail >> CHUNK_SHIFT; c <= (ringHead - 1) >> CHUNK_SHIFT; c++)
            newChunks[c & newChunkMask] = chunkEnd[c & chunkMask];
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (vboBuffer != 0)
        glDeleteBuffers(1, &vboBuffer);
    free(chunkEnd);
    vboBuffer = newBuffer;
    chunkEnd = newChunks;
    chunkMask = newChunkMask;
    mask = newMask;
    ringCapacity = newCap;
    bind_vertex_attribs(vao, vboBuffer);
    return 1;
}

static void ring_reset(void)
{
    ringHead = ringTail = 0;
    batchN = 0;
    openCount = 0;
    ringGrowFailed = 0;
}

// room for n more notes. when there is no video memory left for a bigger ring the oldest notes make room
static void ring_reserve(int n)
{
    while (ringHead - ringTail + n > ringCapacity)
    {
        if (!ringGrowFailed && ringCapacity < (1 << 29))
        {
            printf("\nexpected vbo size limit to be reached! pre-doubling size to %s elements(%s MB of video memory used)\n", AddCommas((int64_t)ringCapacity * 2), AddCommas((int64_t)ringCapacity * 2 * sizeof(RenderNote) >> 20));
            if (gpu_ring_resize(ringCapacity * 2))
                continue;
            ringGrowFailed = 1;
            puts("cannot allocate more elements! dropping oldest notes");
        }
        ringTail = ringHead + n - ringCapacity;
        break;
    }
}

static void flush_batch(void)
{
    int n = batchN;
    if (n == 0) return;
    ring_reserve(n);
    int64_t pos = ringHead;
    for (int64_t c = pos >> CHUNK_SHIFT; c <= (pos + n - 1) >> CHUNK_SHIFT; c++)
    {
        int64_t lastInChunk = (((c + 1) << CHUNK_SHIFT) < pos + n ? ((c + 1) << CHUNK_SHIFT) : pos + n) - 1;
        chunkEnd[c & chunkMask] = batch[lastInChunk - pos].EndTick; // notes come in end order, so the last is the latest
    }
    int at = (int)(pos & mask);
    int first = ringCapacity - at < n ? ringCapacity - at : n;
    glBindBuffer(GL_ARRAY_BUFFER, vboBuffer);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)at * sizeof(RenderNote), (GLsizeiptr)first * sizeof(RenderNote), batch);
    if (first < n)
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(n - first) * sizeof(RenderNote), batch + first);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    ringHead += n;
    batchN = 0;
}

void Renderer_Init(void)
{
    if (isInitialized) return;

    quadShader = build_shader(QuadVertSrc, QuadFragSrc);
    cursorShader = build_shader(CursorVertSrc, CursorFragSrc);
    glGenVertexArrays(1, &cursorVAO); 

    glGenBuffers(1, &frameUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, frameUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(FrameUniforms), NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    uc_cursorX   = glGetUniformLocation(cursorShader, "uCursorX");
    uc_halfWidth = glGetUniformLocation(cursorShader, "uHalfWidthNDC");
    uc_yBottom   = glGetUniformLocation(cursorShader, "uYBottom");
    uc_yTop      = glGetUniformLocation(cursorShader, "uYTop");
    uc_lineColor = glGetUniformLocation(cursorShader, "uLineColor");

    glUseProgram(quadShader);
    glUniform1i(glGetUniformLocation(quadShader, "uPalette"), 0);
    glUseProgram(0);

    glGenVertexArrays(1, &vao);
    glGenVertexArrays(1, &openVao);
    glGenBuffers(1, &openVbo);
    bind_vertex_attribs(openVao, openVbo);
    glGenTextures(1, &paletteTex);

    ring_reset();
    gpu_ring_resize(ringCapacity);
    isInitialized = 1;
}

void Renderer_InitForMIDI(void)
{
    isInitialized = 0;
    kb_release(); // the new file has other tracks
    ring_reset();
    lastSweepEnd = -1;
    lastWindowTicks = -1;
    paletteUploadPending = 1;
    isInitialized = 1;
    puts("renderer is ready");
}

void Renderer_ResetForUnload(void)
{
    isInitialized = 0;
    ring_reset();
    lastSweepEnd = -1;
    lastWindowTicks = -1;
    if (ringCapacity > RENDER_RING_MIN)
        deferredRingCap = RENDER_RING_MIN;
}

void Renderer_Dispose(void)
{
    isInitialized = 0;
    kb_release();
    if (vboBuffer != 0) { glDeleteBuffers(1, &vboBuffer); vboBuffer = 0; }
    if (openVbo != 0) { glDeleteBuffers(1, &openVbo); openVbo = 0; }
    if (openVao != 0) { glDeleteVertexArrays(1, &openVao); openVao = 0; }
    free(chunkEnd);
    chunkEnd = NULL;
    free(openNotes);
    free(openOwner);
    openNotes = NULL;
    openOwner = NULL;
    openCount = openCap = 0;
    if (frameUBO != 0) { glDeleteBuffers(1, &frameUBO); frameUBO = 0; }
    if (paletteTex != 0) { glDeleteTextures(1, &paletteTex); paletteTex = 0; }
    if (vao != 0) { glDeleteVertexArrays(1, &vao); vao = 0; }
    if (quadShader != 0) { glDeleteProgram(quadShader); quadShader = 0; }
    glDeleteVertexArrays(1, &cursorVAO);
    glDeleteProgram(cursorShader);
}

static void __attribute__((noinline)) open_grow(void)
{
    openCap = openCap ? openCap * 2 : 1024;
    openNotes = (RenderNote*)realloc(openNotes, (size_t)openCap * sizeof(RenderNote));
    openOwner = (KeyHeader**)realloc(openOwner, (size_t)openCap * sizeof(KeyHeader*));
}

static void __attribute__((noinline)) batch_full(int* bn)
{
    batchN = *bn;
    flush_batch();
    *bn = 0;
}

static inline __attribute__((always_inline))
void process_one_event(KeyHeader* k, uint32_t statusHigh, uint32_t key, uint32_t velocity, uint32_t color, uint32_t hi, int64_t tick, int* bn)
{
    if (statusHigh == 0x90)
    {
        if (k->count == 0)
        {
            if (UNLIKELY(openCount == openCap)) open_grow();
            int i = openCount++;
            k->openIdx = i;
            openNotes[i] = (RenderNote){.StartTick = (int32_t)tick, .EndTick = 0, .PackedData = (hi << 24) | (color << 16) | (key << 8) | velocity};
            openOwner[i] = k;
        }
        k->count++;
    }
    else if (statusHigh == 0x80)
    {
        if (k->count > 0 && --k->count == 0)
        {
            int i = k->openIdx;
            RenderNote done = openNotes[i];
            done.EndTick = tick ? (int32_t)tick : 1; // 0 means "still sounding"
            int last = --openCount;
            if (i != last)
            {
                openNotes[i] = openNotes[last];
                KeyHeader* moved = openOwner[last];
                openOwner[i] = moved;
                moved->openIdx = i;
            }
            batch[(*bn)++] = done;
            if (UNLIKELY(*bn == BATCH_NOTES)) batch_full(bn);
        }
    }
}

// [currentOffset, runEnd) are whole runs of eventArr (see midistorage.h), offsets are in bytes, all of one track.
// a run has one status, so its channel, and with it the key block, is looked up once per run. the colour only has
// room for 16 tracks (the palette), so tracks past that repeat colours, but never share key headers
static int64_t process_run_events(uint8_t* messages, uint32_t track, int64_t currentOffset, int64_t runEnd, int64_t tick, int* bn)
{
    if (UNLIKELY(track >= (uint64_t)kbTracks)) kb_grow(track);
    if (UNLIKELY(track > maxTrackSeen)) maxTrackSeen = track < MAX_RANK ? track : MAX_RANK;
    EV_RUNS_EX(messages + currentOffset, messages + runEnd, st, third, key,
        KeyHeader** slot = &kbTab[((int64_t)track << 4) | (st & 0x0F)];
        KeyHeader* kb = *slot;
        if (UNLIKELY(!kb)) kb = *slot = kb_alloc();
        const uint32_t color = ((track & 15) << 4) | (st & 0x0F);
        const uint32_t hi = track < MAX_RANK ? track >> 4 : MAX_RANK >> 4; // with the track's low 4 bits in the colour: the rank that breaks ties between notes that start on the same tick, see the depth in the shader
        const uint32_t statusHigh = st & 0xF0;
        ,
    { process_one_event(&kb[key], statusHigh, key, third, color, hi, tick, bn); });
    return runEnd;
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


static void sweep_range(int64_t fromTick, int64_t toTick)
{
    if (activetickcount == 0 || timingArr == NULL) return;

    uint8_t* messages = (uint8_t*)eventArr;

    int64_t limit = toTick < maxTick ? toTick : maxTick;
    int bn = batchN;

    int64_t idx = find_active_tick_idx(fromTick);
    if (idx >= activetickcount) return;

    int64_t currentOffset = timingArr[idx].event_offset;
    int64_t groupStart = 0; // event offset where run groupIdx starts
    int64_t groupIdx = (trackGroups.runs && trackGroups.count > 0) ? tg_find(&trackGroups, currentOffset, &groupStart) : 0;

    while ((groupIdx < trackGroups.count) && (groupStart + trackGroups.runs[groupIdx] <= currentOffset))
        groupStart += trackGroups.runs[groupIdx++];
    while (idx < activetickcount && timingArr[idx].tick <= limit)
    {
        int64_t tick = timingArr[idx].tick;
        int64_t nextOffset = timingArr[idx + 1].event_offset;

        if (trackGroups.runs == NULL || trackGroups.count == 0)
        {
            currentOffset = process_run_events(messages, 0, currentOffset, nextOffset, tick, &bn);
        }
        else
        {
            while (currentOffset < nextOffset)
            {
                if (groupIdx >= trackGroups.count)
                {
                    currentOffset = process_run_events(messages, 0, currentOffset, nextOffset, tick, &bn);
                    break;
                }

                int64_t groupEnd = groupStart + trackGroups.runs[groupIdx];
                int64_t runEnd = groupEnd;
                if (runEnd > nextOffset) runEnd = nextOffset;

                currentOffset = process_run_events(messages, trackGroups.tracks[groupIdx], currentOffset, runEnd, tick, &bn);

                if (currentOffset >= groupEnd)
                {
                    groupStart = groupEnd;
                    groupIdx++;
                }
            }
        }
        idx++;
    }
    batchN = bn;
    flush_batch();
}

// drops the notes that ended left of the view, a whole chunk at a time. notes are in end order, so the last note of a
// chunk is its latest end, and a chunk that ends before the view has nothing left to show
static void advance_tail(int viewStart)
{
    while (ringTail < ringHead)
    {
        int64_t c = ringTail >> CHUNK_SHIFT;
        if (chunkEnd[c & chunkMask] >= viewStart) break;
        int64_t chunkTop = (c + 1) << CHUNK_SHIFT;
        ringTail = chunkTop < ringHead ? chunkTop : ringHead;
    }
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

static void draw_instances(GLuint array, int64_t from, int64_t count)
{
    glBindVertexArray(array);
    int startIdx = (int)(from & mask);
    if (startIdx + count <= ringCapacity)
    {
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)count, startIdx);
    }
    else
    {
        int firstChunk = ringCapacity - startIdx;
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, firstChunk, startIdx);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)(count - firstChunk), 0);
    }
}

static void draw_notes(int viewStart, int viewEnd, int32_t tick, float yBottom, float yTop, float pixelsPerTick)
{
    float yStep = (yTop - yBottom) / 128.0f;
    // with the notes in end order, which of two overlapping notes is on top would depend on the order they ended in.
    // the depth buffer puts the one that started later in front, like when the notes were in start order. blending
    // needs the draw order, so with transparency the notes are drawn in end order and without a depth test
    int useDepth = UseDepthOrder && !EnableTransparency;
    FrameUniforms uniforms = {
        .uMetrics = { pixelsPerTick, yBottom, yStep },
        .uGlowEnabled = (float)EnableGlow,
        .uViewStart = viewStart,
        .uViewEnd = viewEnd,
        .uCurrentTick = tick,
        .uTransparencyEnabled = EnableTransparency,
        .uDepthEnabled = useDepth,
        .uTrackCount = (int32_t)maxTrackSeen + 1
    };

    glBindBuffer(GL_UNIFORM_BUFFER, frameUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(FrameUniforms), &uniforms);

    glUseProgram(quadShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_1D, paletteTex);
    if (useDepth)
    {
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    }

    if (ringHead > ringTail)
        draw_instances(vao, ringTail, ringHead - ringTail);
    if (openCount > 0)
    {
        glBindBuffer(GL_ARRAY_BUFFER, openVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)openCount * sizeof(RenderNote), openNotes, GL_STREAM_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(openVao);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, openCount);
    }

    if (useDepth)
        glDisable(GL_DEPTH_TEST);
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

int fbWidth = 0, fbHeight = 0;
void Renderer_Render(int32_t tick, int pad)
{
    if (deferredRingCap > 0)
    {
        gpu_ring_resize(deferredRingCap);
        deferredRingCap = -1;
    }

    if (!midiloaded || !isInitialized) return;

    if (paletteUploadPending)
        upload_palette();

    int viewStart;
    if (stationarynotes)
    {
        // a page is StationaryBars whole measures, counted from measure 0, so it flips on a measure line whatever the time
        // signature is. a signature change inside a page makes that page longer or shorter, so the window follows the page
        int64_t bars = MAX(1, MIN(StationaryBars, 1 << 20));
        int64_t first = (TimeSig_MeasureAt(tick) / bars) * bars;
        int64_t pageStart = TimeSig_MeasureStart(first);
        WindowTicks = (int)MAX(1, MIN(TimeSig_MeasureStart(first + bars) - pageStart, MAX_WINDOW_TICKS));
        viewStart = (int)MIN(pageStart, maxTick);
    }
    else
    {
        // zoom keys and dynamic scroll can push this to 0 (division by zero below) or overflow int math
        WindowTicks = MAX(1, MIN(WindowTicks, MAX_WINDOW_TICKS));
        viewStart = (tick - (WindowTicks >> 1));
    }
    viewStart = MAX(0, MIN(viewStart, maxTick));
    int viewEnd = MIN(viewStart + WindowTicks, maxTick);

    if (WindowTicks != lastWindowTicks)
    {
        pixelsPerTick = 2.0f / WindowTicks;
        lastWindowTicks = WindowTicks;
        lookaheadTicks = WindowTicks / 2 < 250 ? WindowTicks / 2 : 250;
    }

    int sweepEnd = viewEnd + lookaheadTicks;
    // the notes that ended left of the previous view are gone from the ring, so a view that moved back needs a new sweep,
    // even when its right end did not move (it stops at the end of the song)
    int incremental = lastSweepEnd >= 0 && sweepEnd >= lastSweepEnd && (sweepEnd - lastSweepEnd) < WindowTicks && viewStart >= lastViewStart;

    if (!incremental)
    {
        ring_reset();
        kb_zero();
        sweep_range(MAX(viewStart - WindowTicks, 0), sweepEnd);
    }
    else
    {
        sweep_range(lastSweepEnd + 1, sweepEnd);
    }

    lastSweepEnd = sweepEnd;
    lastViewStart = viewStart;
    advance_tail(viewStart);

    NotesDrawnLastFrame = (int)(ringHead - ringTail) + openCount;
    RingCap = ringCapacity;

    float yBottom = -1.0f + 2.0f * pad / fbHeight;
    float yTop = 1.0f - 2.0f * pad / fbHeight;

    if (NotesDrawnLastFrame > 0)
        draw_notes(viewStart, viewEnd, tick, yBottom, yTop, pixelsPerTick);
    draw_cursor((float)(tick - viewStart) * pixelsPerTick - 1.0f, fbWidth, yBottom, yTop);
}
