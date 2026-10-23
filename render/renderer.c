#include "renderer.h"
#include "shaders.h"
#include "../parse/midistorage.h"
#include "../parse/parser.h"
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
    uint32_t PackedData; // colorIdx:8 << 16 | key:8 << 8 | velocity:8
} RenderNote;

typedef struct {
    float uMetrics[3];
    int32_t uGlowEnabled;
    int32_t uViewStart;
    int32_t uViewEnd;
    int32_t uCurrentTick;
    int32_t uTransparencyEnabled;
    int32_t pad[4];      // the std140 block is 48 bytes
} FrameUniforms;

typedef struct { 
    uint32_t slot;   // while count > 0: the number of this note in the pages (low 32 bits, see close_note)
    int32_t count; 
} KeyHeader;

#define MAX_WINDOW_TICKS (1 << 27) // keeps viewStart + WindowTicks inside int range

static GLuint quadShader, cursorShader, frameUBO;
static GLint uc_cursorX, uc_halfWidth, uc_yBottom, uc_yTop, uc_lineColor;
static GLuint vao, cursorVAO, vboBuffer, paletteTex, scatterShader, scatterVao, scratchVbo, ovrVbo, ovrTex;

// the notes are in video memory in pages of PAGE_NOTES, which the CPU only writes to (glBufferSubData). a note goes into the
// next slot of the newest page when its note on is met, with EndTick 0 (still sounding, the shader draws it to the right
// edge), so the pages are in start order, and for notes of the same tick in the order of the tracks, which is the order
// the events are in: drawing them in page order puts the later start on top, no depth needed. the end is filled in when
// the note off comes: a note still in the batch gets it there, one that is in video memory goes in a patch list that
// is written a page at a time with transform feedback (flush_patches), so nothing is read back or mapped. a page is freed
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
static RenderNote* batch;             // BATCH_NOTES of them, allocated at run time (a static array of this size is stored in the exe on mingw)
// new notes on their way to video memory, the last batchN notes before noteHead
static int batchN;
typedef struct { int32_t phys; int32_t open; int32_t maxEnd; } PageInfo; // phys -1: freed
static PageInfo* pages;               // pages[i] is the page numbered pageOrigin + i, entries before pageLo are freed
static int64_t pageOrigin, pageLo, pageHi, pageCap, livePages;
static int64_t noteHead;              // number of the next note, a note's page is its number >> PAGE_SHIFT
static int poolPages;                 // pages the video memory buffer has room for
static int growFailed;
static int deferredRingCap = -1;
static int* freeQ;                    // MAX_POOL_PAGES entries, allocated at run time
static int freeHead, freeCount;       // free physical pages, oldest first
static inline void freeq_push(int phys) { freeQ[(freeHead + freeCount++) & (MAX_POOL_PAGES - 1)] = phys; }
typedef struct { uint32_t note, end; } Patch; // note: the place of the note in the buffer (physical page * PAGE_NOTES + its slot)
static Patch* patches;
static int64_t patchN, patchCap;
#define SCATTER_PAGES 64              // pages one scatter pass rewrites
static uint32_t* ovrCpu;              // the ends of the pages of this pass, a page after the other, 0: no patch
static int dirtyOrd[MAX_POOL_PAGES];  // physical page -> its place in this pass, -1: not in it
static int dirtyPhys[SCATTER_PAGES];
static int dirtyN;
static GLint u_shift;

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

// notes that are already sounding where a full sweep starts (see restore_sounding). tick is the start of the note,
// pos orders notes of one tick back into file order, unmatched is how many note ons of the key are still open
typedef struct { int64_t tick; int64_t pos; uint32_t track; uint32_t unmatched; uint8_t st, key, vel; } Sounding;
typedef struct { int64_t tick; uint32_t track; uint8_t st, key, third; } RevEvent;
static Sounding* snd;
static RevEvent* rev;
static int64_t sndN, sndCap, revCap;

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
int NotesDrawnLastFrame = 0;
int EnableGlow = 1;
int EnableTransparency = 0;
int stationarynotes = 0;
int StationaryBars = 1;

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

// ---- patch scatter ---------------------------------------------------------------------------------------------
// the ends of notes that are already in video memory are written with transform feedback. it can only write in order,
// not to places picked one by one, so it is done a page at a time: a vertex shader goes over all the notes of a page
// and writes them out as they are, only with the EndTick from a table of ends (ovrCpu) where there is one, into a
// scratch buffer, and the page is copied back over the old one. only the pages that have a patch are touched
static void scatter_init(void)
{
    scatterShader = build_shader(ScatterVertSrc, ScatterFragSrc, 1);
    glUseProgram(scatterShader);
    glUniform1i(glGetUniformLocation(scatterShader, "uOvr"), 1); // texture unit 1, the palette has 0
    u_shift = glGetUniformLocation(scatterShader, "uShift");
    glUseProgram(0);

    glGenVertexArrays(1, &scatterVao);
    glGenBuffers(1, &scratchVbo);
    glGenBuffers(1, &ovrVbo);
    glGenTextures(1, &ovrTex);
    glBindBuffer(GL_COPY_WRITE_BUFFER, scratchVbo);
    glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)SCATTER_PAGES * PAGE_NOTES * sizeof(RenderNote), NULL, GL_STREAM_COPY);
    glBindBuffer(GL_COPY_WRITE_BUFFER, ovrVbo);
    glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)SCATTER_PAGES * PAGE_NOTES * sizeof(uint32_t), NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    glBindTexture(GL_TEXTURE_BUFFER, ovrTex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, ovrVbo);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    ovrCpu = (uint32_t*)malloc((size_t)SCATTER_PAGES * PAGE_NOTES * sizeof(uint32_t));
    for (int i = 0; i < MAX_POOL_PAGES; i++)
        dirtyOrd[i] = -1;
}

// the notes of the buffer as the input of the scatter, called when the buffer is a new one
static void bind_scatter_attribs(void)
{
    glBindVertexArray(scatterVao);
    glBindBuffer(GL_ARRAY_BUFFER, vboBuffer);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 1, GL_INT, sizeof(RenderNote), (void*)offsetof(RenderNote, StartTick));
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_INT, sizeof(RenderNote), (void*)offsetof(RenderNote, EndTick));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(RenderNote), (void*)offsetof(RenderNote, PackedData));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ---- pages ----------------------------------------------------------------------------------------------------
// every page back to the free queue in order, nothing in the buffer is kept
static void ring_reset(void)
{
    freeHead = freeCount = 0;
    for (int p = 0; p < poolPages; p++)
        freeq_push(p);
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
    bind_scatter_attribs();
    if (keep)
    {
        for (int p = old; p < newPages; p++)
            freeq_push(p);
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

static inline void patch_add(uint32_t note, int32_t end)
{
    if (UNLIKELY(patchN == patchCap)) patch_grow();
    patches[patchN++] = (Patch){note, (uint32_t)end};
}

// rewrites the pages collected in dirtyPhys with the ends of ovrCpu
static void scatter_pass(void)
{
    if (dirtyN == 0) return;
    const GLsizeiptr pageBytes = (GLsizeiptr)PAGE_NOTES * sizeof(RenderNote);
    glBindBuffer(GL_COPY_WRITE_BUFFER, ovrVbo);
    glBufferSubData(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)dirtyN * PAGE_NOTES * sizeof(uint32_t), ovrCpu);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    glUseProgram(scatterShader);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_BUFFER, ovrTex);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_RASTERIZER_DISCARD);
    glBindVertexArray(scatterVao);
    for (int i = 0; i < dirtyN; i++)
    {
        glUniform1i(u_shift, (i - dirtyPhys[i]) * PAGE_NOTES); // note n of the buffer is entry n + shift of the table
        glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, scratchVbo, (GLintptr)i * pageBytes, pageBytes);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, dirtyPhys[i] * PAGE_NOTES, PAGE_NOTES);
        glEndTransformFeedback();
    }
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glBindVertexArray(0);
    glDisable(GL_RASTERIZER_DISCARD);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);

    glBindBuffer(GL_COPY_READ_BUFFER, scratchVbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, vboBuffer);
    for (int i = 0; i < dirtyN; i++)
    {
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)i * pageBytes, (GLintptr)dirtyPhys[i] * pageBytes, pageBytes);
        dirtyOrd[dirtyPhys[i]] = -1;
    }
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    dirtyN = 0;
}

// rewrites the pages collected in dirtyPhys with the ends of ovrCpu
static void flush_patches(void)
{
    if (patchN == 0) return;
    for (int64_t i = 0; i < patchN; i++)
    {
        uint32_t phys = patches[i].note >> PAGE_SHIFT;
        int ord = dirtyOrd[phys];
        if (ord < 0)
        {
            if (dirtyN == SCATTER_PAGES)
                scatter_pass();
            ord = dirtyN++;
            dirtyOrd[phys] = ord;
            dirtyPhys[ord] = (int)phys;
            memset(ovrCpu + (size_t)ord * PAGE_NOTES, 0, PAGE_NOTES * sizeof(uint32_t));
        }
        ovrCpu[(size_t)ord * PAGE_NOTES + (patches[i].note & PAGE_MASK)] = patches[i].end;
    }
    scatter_pass();
    patchN = 0;
}

static void page_free(int64_t i)
{
    freeq_push(pages[i].phys);
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
            puts("\ncannot allocate more elements! dropping oldest notes");
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

    quadShader = build_shader(QuadVertSrc, QuadFragSrc, 0);
    cursorShader = build_shader(CursorVertSrc, CursorFragSrc, 0);
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

    freeQ = malloc(MAX_POOL_PAGES * sizeof(int));
    batch = malloc(BATCH_NOTES * sizeof(RenderNote));

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
    free(pages);
    free(patches);
    free(snd);
    free(rev);
    pages = NULL;
    patches = NULL;
    snd = NULL;
    rev = NULL;
    sndN = sndCap = revCap = 0;
    pageCap = patchCap = pageLo = pageHi = pageOrigin = livePages = 0;
    glDeleteBuffers(1, &vboBuffer);
    glDeleteBuffers(1, &scratchVbo);
    glDeleteBuffers(1, &ovrVbo);
    free(ovrCpu);
    ovrCpu = NULL;
    glDeleteBuffers(1, &frameUBO);
    glDeleteTextures(1, &ovrTex);
    glDeleteTextures(1, &paletteTex);
    glDeleteVertexArrays(1, &vao);
    glDeleteVertexArrays(1, &scatterVao);
    glDeleteVertexArrays(1, &cursorVAO);
    glDeleteProgram(scatterShader);
    glDeleteProgram(quadShader);
    glDeleteProgram(cursorShader);
    free(batch);
    free(freeQ);
    batch = NULL;
    freeQ = NULL;
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
        patch_add((uint32_t)((int64_t)p->phys * PAGE_NOTES + (slot & PAGE_MASK)), end);
}

static inline __attribute__((always_inline))
void process_one_event(KeyHeader* k, uint32_t statusHigh, uint32_t key, uint32_t velocity, uint32_t color, int64_t tick, int* bn)
{
    if (statusHigh == 0x90)
    {
        if (k->count == 0)
        {
            if (UNLIKELY((noteHead & PAGE_MASK) == 0)) page_push(bn);
            pages[pageHi - 1].open++;
            k->slot = (uint32_t)noteHead++;
            batch[(*bn)++] = (RenderNote){.StartTick = (int32_t)tick, .EndTick = 0, .PackedData = (color << 16) | (key << 8) | velocity};
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
    EV_RUNS(messages + currentOffset, messages + runEnd, st, third, key,
        KeyHeader** slot = &kbTab[((int64_t)track << 4) | (st & 0x0F)];
        KeyHeader* kb = *slot;
        if (UNLIKELY(!kb)) kb = *slot = kb_alloc();
        const uint32_t color = ((track & 15) << 4) | (st & 0x0F);
        const uint32_t statusHigh = st & 0xF0;
        ,
    { process_one_event(&kb[key], statusHigh, key, third, color, tick, bn); });
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


// ---- sounding notes at the start of a full sweep ------------------------------------------------------------------
// a full sweep (seek, zoom, jump back) used to start one window before the view, so a note that started earlier and is
// still sounding was never loaded. restore_sounding rebuilds those notes by reading the events before idx0 backwards:
// per key, a note off is a pending off, a note on with a pending off cancels it, a note on with nothing pending is
// still sounding (with nested ons the earliest unmatched one started the note, the number of them is the key's count,
// as in process_one_event). the notes go into the pages first, in start order, then the normal sweep goes on from the view.
// only RENDER_BACK_BYTES of events (or twice the swept window, if bigger) are read back: a note that is still sounding
// from before that is missed
#ifndef RENDER_BACK_BYTES
#define RENDER_BACK_BYTES (16 << 20)
#endif
#ifndef BACK_CHUNK
#define BACK_CHUNK (256 << 10)
#endif

static inline KeyHeader* kb_block(uint32_t track, uint32_t channel)
{
    if (UNLIKELY(track >= (uint64_t)kbTracks)) kb_grow(track);
    KeyHeader** slot = &kbTab[((int64_t)track << 4) | channel];
    if (UNLIKELY(!*slot)) *slot = kb_alloc();
    return *slot;
}

static void __attribute__((noinline)) rev_grow(void)
{
    revCap = revCap ? revCap * 2 : (1 << 16);
    rev = (RevEvent*)realloc(rev, (size_t)revCap * sizeof(RevEvent));
}

static int cmp_sounding(const void* a, const void* b)
{
    const Sounding* x = (const Sounding*)a;
    const Sounding* y = (const Sounding*)b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    if (x->track != y->track) return x->track < y->track ? -1 : 1;
    return (x->pos < y->pos) - (x->pos > y->pos); // earlier in the file = larger pos, first
}

static void restore_sounding(int64_t idx0, int64_t idxEnd)
{
    kb_zero();
    sndN = 0;
    if (idx0 <= 0 || timingArr == NULL || eventArr == NULL) return;
    const uint8_t* messages = (const uint8_t*)eventArr;
    int64_t budget = RENDER_BACK_BYTES;
    if (idxEnd > idx0)
    {
        int64_t window = 2 * (timingArr[idxEnd].event_offset - timingArr[idx0].event_offset);
        if (window > budget) budget = window;
    }
    const int64_t endOff = timingArr[idx0].event_offset;
    int64_t pos = 0;
    for (int64_t ce = idx0; ce > 0;)
    {
        int64_t cs = ce - 1;
        while (cs > 0 && timingArr[ce].event_offset - timingArr[cs - 1].event_offset <= BACK_CHUNK) cs--;
        if (endOff - timingArr[cs].event_offset > budget) break;

        // the chunk, decoded forward
        int64_t n = 0;
        int64_t cur = timingArr[cs].event_offset;
        int64_t gs = 0, gi = 0;
        const int indexed = trackGroups.runs && trackGroups.count > 0;
        if (indexed)
        {
            gi = tg_find(&trackGroups, cur, &gs);
            while ((gi < trackGroups.count) && (gs + trackGroups.runs[gi] <= cur))
                gs += trackGroups.runs[gi++];
        }
        for (int64_t idx = cs; idx < ce; idx++)
        {
            const int64_t tick = timingArr[idx].tick;
            const int64_t nextOffset = timingArr[idx + 1].event_offset;
            while (cur < nextOffset)
            {
                uint32_t track = 0;
                int64_t groupEnd = nextOffset;
                if (indexed && gi < trackGroups.count)
                {
                    track = trackGroups.tracks[gi];
                    groupEnd = gs + trackGroups.runs[gi];
                }
                int64_t runEnd = groupEnd < nextOffset ? groupEnd : nextOffset;
                // the body is one parenthesised expression: a macro argument splits at every comma outside ()
                EV_RUNS(messages + cur, messages + runEnd, st, third, key, ,
                {
                    if (UNLIKELY(n == revCap)) rev_grow();
                    rev[n++] = ((RevEvent){tick, track, (uint8_t)st, (uint8_t)key, (uint8_t)third});
                });
                cur = runEnd;
                if (indexed && gi < trackGroups.count && cur >= groupEnd)
                {
                    gs = groupEnd;
                    gi++;
                }
            }
        }

        // the same events, last first
        for (int64_t i = n - 1; i >= 0; i--, pos++)
        {
            const RevEvent* e = &rev[i];
            const uint32_t statusHigh = e->st & 0xF0u;
            if (statusHigh != 0x90 && statusHigh != 0x80) continue;
            // count: note offs still to be matched. slot: 1 + the index of its sounding note
            KeyHeader* k = &kb_block(e->track, e->st & 0x0Fu)[e->key];
            if (statusHigh == 0x80)
                k->count++;
            else if (k->count > 0)
                k->count--;
            else
            {
                if (k->slot == 0)
                {
                    if (UNLIKELY(sndN == sndCap))
                    {
                        sndCap = sndCap ? sndCap * 2 : 1024;
                        snd = (Sounding*)realloc(snd, (size_t)sndCap * sizeof(Sounding));
                    }
                    snd[sndN] = (Sounding){0};
                    k->slot = (uint32_t)++sndN;
                }
                Sounding* sn = &snd[k->slot - 1];
                sn->unmatched++;
                sn->tick = e->tick; // an earlier one than the last, it is the one that started the note
                sn->pos = pos;
                sn->track = e->track;
                sn->st = e->st;
                sn->key = e->key;
                sn->vel = e->third;
            }
        }
        ce = cs;
    }

    kb_zero();
    if (sndN == 0) return;
    qsort(snd, (size_t)sndN, sizeof(Sounding), cmp_sounding);
    int bn = batchN;
    for (int64_t i = 0; i < sndN; i++)
    {
        const Sounding* sn = &snd[i];
        KeyHeader* k = &kb_block(sn->track, sn->st & 0x0Fu)[sn->key];
        const uint32_t color = ((sn->track & 15) << 4) | (sn->st & 0x0F);
        process_one_event(k, 0x90, sn->key, sn->vel, color, sn->tick, &bn);
        k->count = (int32_t)sn->unmatched;
    }
    batchN = bn;
    flush_batch();
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
    FrameUniforms uniforms = {
        .uMetrics = { pixelsPerTick, yBottom, yStep },
        .uGlowEnabled = EnableGlow,
        .uViewStart = viewStart,
        .uViewEnd = viewEnd,
        .uCurrentTick = tick,
        .uTransparencyEnabled = EnableTransparency,
    };

    glBindBuffer(GL_UNIFORM_BUFFER, frameUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(FrameUniforms), &uniforms);

    glUseProgram(quadShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_1D, paletteTex);
    draw_pages();
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
        restore_sounding(find_active_tick_idx(viewStart), find_active_tick_idx((int64_t)sweepEnd + 1)); // notes already sounding at the view
        sweep_range(viewStart, sweepEnd);
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

    float yBottom = -1.0f + 2.0f * pad / fbHeight;
    float yTop = 1.0f - 2.0f * pad / fbHeight;

    if (NotesDrawnLastFrame > 0)
        draw_notes(viewStart, viewEnd, tick, yBottom, yTop, pixelsPerTick);
    draw_cursor((float)(tick - viewStart) * pixelsPerTick - 1.0f, fbWidth, yBottom, yTop);
}