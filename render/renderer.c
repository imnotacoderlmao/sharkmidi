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
    uint32_t slot;   // while count > 0: the number of this note in the pages (low 32 bits, see close_note)
    int32_t count; 
} KeyHeader;

#define MAX_WINDOW_TICKS (1 << 27) // keeps viewStart + WindowTicks inside int range

static GLuint quadShader, cursorShader, frameUBO;
static GLint uc_cursorX, uc_halfWidth, uc_yBottom, uc_yTop, uc_lineColor;
static GLuint vao, cursorVAO, vboBuffer, paletteTex, ringTex, patchVbo, patchVao, scatterShader;

// the notes are in video memory in pages of PAGE_NOTES, which the CPU only writes to (glBufferSubData). a note goes into the
// next slot of the newest page when its note on is met, with EndTick 0 (still sounding, the shader draws it to the right
// edge), so the pages are in start order, and for notes of the same tick in the order of the tracks, which is the order
// the events are in: drawing them in page order puts the later start on top, no depth needed. the end is filled in when
// the note off comes: a note still in the batch gets it there, one that is in video memory goes in a patch list that a
// vertex shader writes into the buffer in one draw (flush_patches), so nothing is read back or mapped. a page is freed
// when none of its notes is sounding and all have ended left of the view, so a stuck note only keeps its own page. free
// pages are reused oldest first, which keeps the live pages mostly in one piece for the draw
#ifndef RENDER_RING_MIN
#define RENDER_RING_MIN (1 << 20)
#endif
#define BATCH_NOTES (1 << 16)
#define PAGE_SHIFT 12
#define PAGE_NOTES (1 << PAGE_SHIFT)
#define PAGE_MASK (PAGE_NOTES - 1)
#define MAX_POOL_PAGES (1 << 17)      // 2^29 notes
static RenderNote batch[BATCH_NOTES]; // new notes on their way to video memory, the last batchN notes before noteHead
static int batchN;
typedef struct { int32_t phys; int32_t open; int32_t maxEnd; } PageInfo; // phys -1: freed
static PageInfo* pages;               // pages[i] is the page numbered pageOrigin + i, entries before pageLo are freed
static int64_t pageOrigin, pageLo, pageHi, pageCap, livePages;
static int64_t noteHead;              // number of the next note, a note's page is its number >> PAGE_SHIFT
static int poolPages;                 // pages the video memory buffer has room for
static int growFailed;
static int deferredRingCap = -1;
static int freeQ[MAX_POOL_PAGES], freeHead, freeCount; // free physical pages, oldest first
typedef struct { uint32_t word, end; } Patch; // word: where the EndTick is in the buffer, in 32 bit words
static Patch* patches;
static int64_t patchN, patchCap;
static int scatterOk;
static int64_t scatterWords;          // how much of the buffer the texture buffer view of the scatter reaches

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
int UseDepthOrder = 0; // 1: track priority, the higher track in front whatever the start. 0: start order, from the pages
int StationaryBars = 1;

static int shaderQuiet; // a failure returns 0 instead of ending the program
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
        if (!shaderQuiet) exit(1);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint build_shader(const char* vert, const char* frag)
{
    GLuint v = compile_stage(GL_VERTEX_SHADER, vert);
    GLuint f = compile_stage(GL_FRAGMENT_SHADER, frag);
    if (!v || !f)
    {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
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
        if (!shaderQuiet) exit(1);
        glDeleteShader(v);
        glDeleteShader(f);
        glDeleteProgram(prog);
        return 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return prog;
}

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

// ---- loader ---------------------------------------------------------------------------------------------------
// the scatter needs glBindImageTexture and glMemoryBarrier (4.2) and, for a buffer bigger than one texture buffer can
// reach, glTexBufferRange (4.3). the glad of the project may not have them, so they come through the loader given to
// Renderer_SetProcLoader (glfwGetProcAddress), call it before Renderer_Init. without them the patches are written one
// glBufferSubData at a time, which is correct but slow
typedef void (*ScBindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
typedef void (*ScMemoryBarrier)(GLbitfield);
typedef void (*ScTexBufferRange)(GLenum, GLenum, GLuint, GLintptr, GLsizeiptr);
static void* (*procLoader)(const char*);
static ScBindImageTexture scBindImageTexture;
static ScMemoryBarrier scMemoryBarrier;
static ScTexBufferRange scTexBufferRange;
#ifndef GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT
#define GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT 0x00000001
#endif
#ifndef GL_BUFFER_UPDATE_BARRIER_BIT
#define GL_BUFFER_UPDATE_BARRIER_BIT 0x00000200
#endif

void Renderer_SetProcLoader(void* (*loader)(const char*))
{
    procLoader = loader;
}

static void scatter_init(void)
{
    if (!procLoader) return;
    scBindImageTexture = (ScBindImageTexture)procLoader("glBindImageTexture");
    scMemoryBarrier = (ScMemoryBarrier)procLoader("glMemoryBarrier");
    scTexBufferRange = (ScTexBufferRange)procLoader("glTexBufferRange");
    if (!scBindImageTexture || !scMemoryBarrier) return;
    shaderQuiet = 1;
    scatterShader = build_shader(ScatterVertSrc, ScatterFragSrc);
    shaderQuiet = 0;
    if (!scatterShader)
    {
        puts("patch scatter is not available on this GPU, using slow buffer updates");
        return;
    }
    glGenTextures(1, &ringTex);
    glGenVertexArrays(1, &patchVao);
    glGenBuffers(1, &patchVbo);
    glBindVertexArray(patchVao);
    glBindBuffer(GL_ARRAY_BUFFER, patchVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(Patch), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(Patch), (void*)4);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// the view of the buffer the scatter writes through. a texture buffer reaches at most GL_MAX_TEXTURE_BUFFER_SIZE texels,
// a patch past that goes the slow way
static void bind_ring_texture(void)
{
    scatterOk = 0;
    scatterWords = 0;
    if (!scatterShader) return;
    GLint maxTexels = 0;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTexels);
    int64_t words = (int64_t)poolPages * PAGE_NOTES * 3;
    glBindTexture(GL_TEXTURE_BUFFER, ringTex);
    if (words <= maxTexels)
    {
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, vboBuffer);
        scatterWords = words;
    }
    else if (scTexBufferRange && maxTexels > 0)
    {
        scTexBufferRange(GL_TEXTURE_BUFFER, GL_R32UI, vboBuffer, 0, (GLsizeiptr)maxTexels * 4);
        scatterWords = maxTexels;
    }
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    scatterOk = scatterWords > 0;
}

// ---- pages ----------------------------------------------------------------------------------------------------
// every page back to the free queue in order, nothing in the buffer is kept
static void ring_reset(void)
{
    freeHead = freeCount = 0;
    for (int p = 0; p < poolPages; p++)
        freeQ[freeCount++] = p;
    pageOrigin = pageLo = pageHi = livePages = 0;
    noteHead = 0;
    batchN = 0;
    patchN = 0;
    growFailed = 0;
}

// a buffer of newPages pages. keep: the pages in use are copied over on the GPU and the new pages are free, otherwise the
// buffer starts empty. returns 0, and changes nothing, when there is no memory for it
static int pool_resize(int newPages, int keep)
{
    GLuint newBuffer;
    glGenBuffers(1, &newBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, newBuffer);
    while (glGetError() != GL_NO_ERROR) {}
    glBufferStorage(GL_ARRAY_BUFFER, (GLsizeiptr)newPages * PAGE_NOTES * sizeof(RenderNote), NULL, GL_DYNAMIC_STORAGE_BIT); // not mapped: lives in video memory
    if (glGetError() != GL_NO_ERROR)
    {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &newBuffer);
        return 0;
    }
    int old = poolPages;
    if (keep && vboBuffer != 0)
    {
        glBindBuffer(GL_COPY_READ_BUFFER, vboBuffer);
        glBindBuffer(GL_COPY_WRITE_BUFFER, newBuffer);
        int copyPages = old < newPages ? old : newPages;
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (GLsizeiptr)copyPages * PAGE_NOTES * sizeof(RenderNote));
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (vboBuffer != 0)
        glDeleteBuffers(1, &vboBuffer);
    vboBuffer = newBuffer;
    poolPages = newPages;
    bind_vertex_attribs(vao, vboBuffer);
    bind_ring_texture();
    if (keep)
    {
        for (int p = old; p < newPages; p++)
            freeQ[(freeHead + freeCount++) & (MAX_POOL_PAGES - 1)] = p;
    }
    else
        ring_reset();
    return 1;
}

static void flush_batch(void)
{
    int64_t n = batchN;
    if (n == 0) return;
    int64_t s = noteHead - n;
    const RenderNote* src = batch;
    glBindBuffer(GL_ARRAY_BUFFER, vboBuffer);
    while (n > 0)
    {
        int64_t i = (s >> PAGE_SHIFT) - pageOrigin;
        int64_t seg = PAGE_NOTES - (s & PAGE_MASK);
        if (seg > n) seg = n;
        if (i >= pageLo && i < pageHi && pages[i].phys >= 0) // a page dropped for lack of memory has nowhere to go
            glBufferSubData(GL_ARRAY_BUFFER, ((GLintptr)pages[i].phys * PAGE_NOTES + (s & PAGE_MASK)) * sizeof(RenderNote), (GLsizeiptr)seg * sizeof(RenderNote), src);
        s += seg;
        src += seg;
        n -= seg;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    batchN = 0;
}

static void __attribute__((noinline)) patch_grow(void)
{
    patchCap = patchCap ? patchCap * 2 : 4096;
    patches = (Patch*)realloc(patches, (size_t)patchCap * sizeof(Patch));
}

static inline void patch_add(uint32_t word, int32_t end)
{
    if (UNLIKELY(patchN == patchCap)) patch_grow();
    patches[patchN++] = (Patch){word, (uint32_t)end};
}

// writes the ends of the notes that are in video memory
static void flush_patches(void)
{
    if (patchN == 0) return;
    int64_t n = 0;
    glBindBuffer(GL_ARRAY_BUFFER, vboBuffer);
    for (int64_t i = 0; i < patchN; i++)
    {
        if (scatterOk && patches[i].word < scatterWords)
            patches[n++] = patches[i];
        else
            glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)patches[i].word * 4, 4, &patches[i].end);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (n > 0)
    {
        glUseProgram(scatterShader);
        scBindImageTexture(0, ringTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
        glEnable(GL_RASTERIZER_DISCARD);
        glBindVertexArray(patchVao);
        for (int64_t at = 0; at < n; at += (1 << 22))
        {
            int64_t part = n - at < (1 << 22) ? n - at : (1 << 22);
            glBindBuffer(GL_ARRAY_BUFFER, patchVbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)part * sizeof(Patch), patches + at, GL_STREAM_DRAW);
            glDrawArrays(GL_POINTS, 0, (GLsizei)part);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        glDisable(GL_RASTERIZER_DISCARD);
        glUseProgram(0);
        scMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }
    patchN = 0;
}

static void page_free(int64_t i)
{
    freeQ[(freeHead + freeCount++) & (MAX_POOL_PAGES - 1)] = pages[i].phys;
    pages[i].phys = -1;
    livePages--;
}

// moves pageLo over the freed pages at the front, the newest page always stays
static void pages_trim(void)
{
    while (pageLo < pageHi - 1 && pages[pageLo].phys < 0)
        pageLo++;
    if (pageLo >= 1024 && pageLo * 2 >= pageHi)
    {
        memmove(pages, pages + pageLo, (size_t)(pageHi - pageLo) * sizeof(PageInfo));
        pageOrigin += pageLo;
        pageHi -= pageLo;
        pageLo = 0;
    }
}

// a free physical page. when there is none the buffer doubles, and when there is no video memory for that the oldest
// page makes room, its notes are gone
static int phys_alloc(int* bn)
{
    if (freeCount == 0 && !growFailed && poolPages < MAX_POOL_PAGES)
    {
        int np = poolPages * 2 < MAX_POOL_PAGES ? poolPages * 2 : MAX_POOL_PAGES;
        printf("\nexpected vbo size limit to be reached! pre-doubling size to %s elements (%s MB of video memory used)\n", 
            AddCommas((int64_t)np * PAGE_NOTES), 
            AddCommas((int64_t)np * PAGE_NOTES * sizeof(RenderNote) >> 20));
        if (!pool_resize(np, 1))
        {
            growFailed = 1;
            puts("cannot allocate more elements! dropping oldest notes");
        }
    }
    if (freeCount == 0)
    {
        batchN = *bn; // the patches and the batch still point at the pages, write them before one is reused
        flush_batch();
        *bn = 0;
        flush_patches();
        int64_t i = pageLo;
        while (pages[i].phys < 0) i++;
        page_free(i);
        pages_trim();
    }
    freeCount--;
    return freeQ[freeHead++ & (MAX_POOL_PAGES - 1)];
}

static void page_push(int* bn)
{
    int phys = phys_alloc(bn);
    if (pageHi == pageCap)
    {
        pageCap = pageCap ? pageCap * 2 : 1024;
        pages = (PageInfo*)realloc(pages, (size_t)pageCap * sizeof(PageInfo));
    }
    pages[pageHi++] = (PageInfo){phys, 0, 0};
    livePages++;
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
    glGenTextures(1, &paletteTex);

    scatter_init();
    pool_resize(RENDER_RING_MIN >> PAGE_SHIFT, 0);
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
    if ((int64_t)poolPages * PAGE_NOTES > RENDER_RING_MIN)
        deferredRingCap = RENDER_RING_MIN;
}

void Renderer_Dispose(void)
{
    isInitialized = 0;
    kb_release();
    if (vboBuffer != 0) { glDeleteBuffers(1, &vboBuffer); vboBuffer = 0; }
    free(pages);
    free(patches);
    pages = NULL;
    patches = NULL;
    pageCap = patchCap = pageLo = pageHi = pageOrigin = livePages = 0;
    if (patchVbo != 0) { glDeleteBuffers(1, &patchVbo); patchVbo = 0; }
    if (patchVao != 0) { glDeleteVertexArrays(1, &patchVao); patchVao = 0; }
    if (ringTex != 0) { glDeleteTextures(1, &ringTex); ringTex = 0; }
    if (scatterShader != 0) { glDeleteProgram(scatterShader); scatterShader = 0; }
    if (frameUBO != 0) { glDeleteBuffers(1, &frameUBO); frameUBO = 0; }
    if (paletteTex != 0) { glDeleteTextures(1, &paletteTex); paletteTex = 0; }
    if (vao != 0) { glDeleteVertexArrays(1, &vao); vao = 0; }
    if (quadShader != 0) { glDeleteProgram(quadShader); quadShader = 0; }
    glDeleteVertexArrays(1, &cursorVAO);
    glDeleteProgram(cursorShader);
}

static void __attribute__((noinline)) batch_full(int* bn)
{
    batchN = *bn;
    flush_batch();
    *bn = 0;
}

// the note off of the note at slot32: its end goes in the batch if the note is still there, else in a patch
static void __attribute__((noinline)) close_note(uint32_t slot32, int64_t tick, int bn)
{
    int64_t slot = noteHead - (int64_t)((uint32_t)noteHead - slot32); // the notes sounding are fewer than 2^32
    int32_t end = tick ? (int32_t)tick : 1; // 0 means "still sounding"
    int64_t i = (slot >> PAGE_SHIFT) - pageOrigin;
    if (UNLIKELY(i < pageLo || i >= pageHi || pages[i].phys < 0)) return; // dropped when the video memory ran out
    PageInfo* p = &pages[i];
    p->open--;
    if (end > p->maxEnd) p->maxEnd = end;
    int64_t inBatch = slot - (noteHead - bn);
    if (inBatch >= 0)
        batch[inBatch].EndTick = end;
    else
        patch_add((uint32_t)(((int64_t)p->phys * PAGE_NOTES + (slot & PAGE_MASK)) * 3 + 1), end);
}

static inline __attribute__((always_inline))
void process_one_event(KeyHeader* k, uint32_t statusHigh, uint32_t key, uint32_t velocity, uint32_t color, uint32_t hi, int64_t tick, int* bn)
{
    if (statusHigh == 0x90)
    {
        if (k->count == 0)
        {
            if (UNLIKELY((noteHead & PAGE_MASK) == 0)) page_push(bn);
            pages[pageHi - 1].open++;
            k->slot = (uint32_t)noteHead++;
            batch[(*bn)++] = (RenderNote){.StartTick = (int32_t)tick, .EndTick = 0, .PackedData = (hi << 24) | (color << 16) | (key << 8) | velocity};
            if (UNLIKELY(*bn == BATCH_NOTES)) batch_full(bn);
        }
        k->count++;
    }
    else if (statusHigh == 0x80)
    {
        if (k->count > 0 && --k->count == 0)
            close_note(k->slot, tick, *bn);
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

// frees the pages that have nothing left to show: no note of them is sounding and all ended left of the view
static void release_pages(int viewStart)
{
    for (int64_t i = pageLo; i < pageHi - 1; i++)
    {
        PageInfo* p = &pages[i];
        if (p->phys >= 0 && p->open == 0 && p->maxEnd < viewStart)
            page_free(i);
    }
    pages_trim();
}

static int64_t live_notes(void)
{
    if (livePages == 0) return 0;
    int64_t newestStart = (pageOrigin + pageHi - 1) << PAGE_SHIFT;
    return (livePages - 1) * PAGE_NOTES + (noteHead - newestStart);
}

// the live pages in order, a run of pages that are also next to each other in the buffer is one draw
static void draw_pages(void)
{
    glBindVertexArray(vao);
    int64_t i = pageLo;
    while (i < pageHi)
    {
        if (pages[i].phys < 0) { i++; continue; }
        int64_t j = i;
        int64_t notes = PAGE_NOTES;
        while (j + 1 < pageHi && pages[j + 1].phys == pages[j].phys + 1)
            j++, notes += PAGE_NOTES;
        if (j == pageHi - 1) // the newest page is not full
            notes -= PAGE_NOTES - (noteHead - (((pageOrigin + j) << PAGE_SHIFT)));
        else if (i == pageHi - 1)
            notes = noteHead - ((pageOrigin + i) << PAGE_SHIFT);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)notes, (GLuint)((int64_t)pages[i].phys * PAGE_NOTES));
        i = j + 1;
    }
}

static void draw_notes(int viewStart, int viewEnd, int32_t tick, float yBottom, float yTop, float pixelsPerTick)
{
    float yStep = (yTop - yBottom) / 128.0f;
    // the pages are in start order, so overlapping notes are already in the right order: the later start is drawn over the
    // earlier one, and for the same tick the higher track. that is also what blending needs. the depth buffer is only for
    // UseDepthOrder, the track priority mode, where the higher track is in front whatever the start
    int useDepth = UseDepthOrder && !EnableTransparency;
    FrameUniforms uniforms = {
        .uMetrics = { pixelsPerTick, yBottom, yStep },
        .uGlowEnabled = EnableGlow,
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

    draw_pages();

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
        pool_resize(deferredRingCap >> PAGE_SHIFT, 0);
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
    // the pages that ended left of the previous view are gone, so a view that moved back needs a new sweep,
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
    flush_patches();
    release_pages(viewStart);

    NotesDrawnLastFrame = (int)live_notes();
    RingCap = poolPages * PAGE_NOTES;

    float yBottom = -1.0f + 2.0f * pad / fbHeight;
    float yTop = 1.0f - 2.0f * pad / fbHeight;

    if (NotesDrawnLastFrame > 0)
        draw_notes(viewStart, viewEnd, tick, yBottom, yTop, pixelsPerTick);
    draw_cursor((float)(tick - viewStart) * pixelsPerTick - 1.0f, fbWidth, yBottom, yTop);
}