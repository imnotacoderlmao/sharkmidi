// bench_sweep: times the renderer's event sweep on a real file, with the GL calls stubbed out, and compares variants of
// process_run_events. see bench_sweep.sh to build it. usage: ./bench_sweep [-r reps] [-c chunk_ticks] file.mid [more.mid ...]
// the file is swept in chunks of chunk_ticks (default 2000, a WindowTicks-sized step, like the incremental path does it),
// patchN is reset after every chunk as flush_patches would. the note state carries over from chunk to chunk.
#define _GNU_SOURCE
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../render/renderer.h"
static int64_t (*runfn)(uint8_t*, uint32_t, int64_t, int64_t, int64_t, int*);
#include "tools/.bench/renderer_h.c"   // renderer.c with process_run_events renamed, see bench_sweep.sh
#include "playback/playback_thread.h"
#define LIKELY(x) __builtin_expect(!!(x), 1)
int64_t playednotes = 0, notespersec = 0;
void Playback_StopAndWait(void) {}

static uint64_t sink, ck, uck; static int verify;
static inline uint64_t mix(uint64_t x) { x *= 0x9E3779B97F4A7C15ull; x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32; return x; }
#define MIXEND(e) mix((uint64_t)(uint32_t)(e) ^ 0xABCDEF0123ull)
static void s_bind(GLenum a, GLuint b) { (void)a; (void)b; }
static void s_sub(GLenum t, GLintptr o, GLsizeiptr s, const void* d)
{
    (void)t; (void)o;
    if (verify)
    {
        const uint32_t* p = d; for (GLsizeiptr i = 0; i < s / 4; i++) ck = ck * 1000003u + p[i];
        // order independent: the notes (start, colour, key, velocity) and the ends, wherever the end went (row or patch)
        const RenderNote* r = d; for (GLsizeiptr i = 0; i < s / (GLsizeiptr)sizeof(RenderNote); i++) { uck += mix(((uint64_t)(uint32_t)r[i].StartTick << 32) | r[i].PackedData); if (r[i].EndTick) uck += MIXEND(r[i].EndTick); }
    }
}

// decode only: the runs and bytes are walked, nothing else is done. the most "skip the decode" could ever save
static int64_t v_decode(uint8_t* m, uint32_t track, int64_t cur, int64_t runEnd, int64_t tick, int* bn)
{
    uint32_t s = 0, h = 0; (void)track; (void)tick; (void)bn;
    EV_RUNS(m + cur, m + runEnd, st, third, key, h += st * 3 + third;, { s += key; });
    sink += s + h;
    return runEnd;
}
// decode + key header lookup + count update only (no batch write, no close_note)
static int64_t v_count(uint8_t* m, uint32_t track, int64_t cur, int64_t runEnd, int64_t tick, int* bn)
{
    (void)tick; (void)bn;
    if (UNLIKELY(track >= (uint64_t)kbTracks)) kb_grow(track);
    uint32_t z = 0;
    EV_RUNS(m + cur, m + runEnd, st, third, key,
        KeyHeader** slot = &kbTab[((int64_t)track << 4) | (st & 0x0F)];
        KeyHeader* kb = *slot; if (UNLIKELY(!kb)) kb = *slot = kb_alloc();
        const uint32_t hi = st & 0xF0; (void)third;
        ,
    { KeyHeader* k = &kb[key];
      if (hi == 0x90) { z += (k->count == 0); k->count++; }
      else if (hi == 0x80) { if (k->count > 0) k->count--; } });
    sink += z;
    return runEnd;
}
// split by the run's status, bn in a register, close_note's common case inline
static int64_t v_spec(uint8_t* m, uint32_t track, int64_t cur, int64_t runEnd, int64_t tick, int* bnp)
{
    if (UNLIKELY(track >= (uint64_t)kbTracks)) kb_grow(track);
    const uint8_t* p = m + cur; const uint8_t* end = m + runEnd;
    int bn = *bnp; const int32_t endT = tick ? (int32_t)tick : 1;
    while (p < end)
    {
        const uint32_t st = p[0], third = p[1];
        p += 2;
        KeyHeader** slot = &kbTab[((int64_t)track << 4) | (st & 0x0F)];
        KeyHeader* kb = *slot;
        if (UNLIKELY(!kb)) kb = *slot = kb_alloc();
        const uint32_t hi = st & 0xF0;
        uint8_t b;
        if (hi == 0x90)
        {
            const uint32_t pk = (((track & 15) << 4 | (st & 0x0F)) << 16) | third;
            do {
                b = *p++;
                const uint32_t key = b & 0x7F;
                KeyHeader* k = &kb[key];
                if (k->count == 0)
                {
                    if (UNLIKELY((noteHead & PAGE_MASK) == 0)) { *bnp = bn; page_push(bnp); bn = *bnp; }
                    pages[pageHi - 1].open++;
                    k->slot = (uint32_t)noteHead++;
                    RenderNote* d = &batch[bn++];
                    d->StartTick = (int32_t)tick; d->EndTick = 0; d->PackedData = pk | (key << 8);
                    if (UNLIKELY(bn == BATCH_NOTES)) { *bnp = bn; batch_full(bnp); bn = *bnp; }
                }
                k->count++;
            } while (!(b & 0x80));
        }
        else if (hi == 0x80)
        {
            do {
                b = *p++;
                KeyHeader* k = &kb[b & 0x7F];
                if (k->count > 0 && --k->count == 0)
                {
                    const int64_t sl = noteHead - (int64_t)((uint32_t)noteHead - k->slot);
                    const int64_t inBatch = sl - (noteHead - bn);
                    const int64_t pi = (sl >> PAGE_SHIFT) - pageOrigin;
                    if (LIKELY(inBatch >= 0 && pi >= pageLo && pi < pageHi && pages[pi].phys >= 0))
                    {
                        PageInfo* pg = &pages[pi];
                        pg->open--;
                        if (endT > pg->maxEnd) pg->maxEnd = endT;
                        batch[inBatch].EndTick = endT;
                    }
                    else close_note(k->slot, tick, bn);
                }
            } while (!(b & 0x80));
        }
        else
            do { b = *p++; } while (!(b & 0x80));
    }
    *bnp = bn;
    return runEnd;
}


// ---- other layouts of eventArr, built in memory from the loaded one (the parser is not touched) -----------------------------
// 1) "sorted bytes": the same bytes as now, but the keys of a run in ascending order (copies side by side). same size.
// 2) "nibbles": header (status, third), then the lowest key as a byte (bit 7 set: the run is that one key and nothing follows),
//    else a stream of nibbles, low nibble first: 0-11 next distinct key = previous + 1 + v, 12 = the gap follows in two
//    nibbles (low, high) for gaps over 11, 13 = one more copy of the key before, 14 = end of the run, 15 = more copies of the key before, their number (1-255) in the
//    next two nibbles (low, high). an odd nibble count is
//    padded with a 14, so the next run starts on a byte
typedef struct { uint8_t* ev; TickGroup* tim; uint32_t* runs; int64_t* anchors; int64_t bytes; } Layout;
typedef struct { uint8_t* o; int64_t pos; int half; } NW;
static inline void nw_put(NW* w, uint32_t v) { if (!w->half) { w->o[w->pos] = (uint8_t)v; w->half = 1; } else { w->o[w->pos] |= (uint8_t)(v << 4); w->pos++; w->half = 0; } }

static inline void nw_copies(NW* w, uint32_t extra)
{
    if (extra <= 3) { while (extra--) nw_put(w, 13); return; }
    while (extra) { uint32_t c = extra > 255 ? 255 : extra; nw_put(w, 15); nw_put(w, c & 15); nw_put(w, c >> 4); extra -= c; }
}

static int64_t encode_run(uint8_t* o, uint32_t st, uint32_t third, const uint32_t* cnt, int mode)
{
    int64_t pos = 0; o[pos++] = (uint8_t)st; o[pos++] = (uint8_t)third;
    int distinct = 0; uint32_t total = 0; int first = -1;
    for (int k = 0; k < 128; k++) if (cnt[k]) { distinct++; total += cnt[k]; if (first < 0) first = k; }
    if (mode == 1)
    {
        uint32_t left = total;
        for (int k = 0; k < 128; k++) for (uint32_t c = 0; c < cnt[k]; c++) o[pos++] = (uint8_t)(k | (--left == 0 ? 0x80 : 0));
        return pos;
    }
    if (total == 1) { o[pos++] = (uint8_t)(first | 0x80); return pos; }
    o[pos++] = (uint8_t)first;
    NW w = {o, pos, 0};
    nw_copies(&w, cnt[first] - 1);
    int prev = first;
    for (int k = first + 1; k < 128; k++)
    {
        if (!cnt[k]) continue;
        int gap = k - prev - 1;
        if (gap <= 11) nw_put(&w, (uint32_t)gap); else { nw_put(&w, 12); nw_put(&w, (uint32_t)gap & 15); nw_put(&w, (uint32_t)gap >> 4); }
        nw_copies(&w, cnt[k] - 1);
        prev = k;
    }
    nw_put(&w, 14);
    if (w.half) nw_put(&w, 14);
    return w.pos;
}

static Layout build_layout(int mode)
{
    Layout L = {0};
    const int64_t oldBytes = timingArr[activetickcount].event_offset;
    L.ev = malloc((size_t)(oldBytes + oldBytes / 2 + 1024));
    L.tim = malloc((size_t)(activetickcount + 1) * sizeof(TickGroup));
    memcpy(L.tim, timingArr, (size_t)(activetickcount + 1) * sizeof(TickGroup));
    L.runs = malloc((size_t)trackGroups.count * sizeof(uint32_t));
    L.anchors = malloc((size_t)((trackGroups.count + TG_ANCHOR - 1) / TG_ANCHOR + 1) * sizeof(int64_t));
    uint32_t cnt[128] = {0}; uint8_t keys[128];
    int64_t g = 0, oldOff = 0, newOff = 0;
    for (int64_t idx = 0; idx < activetickcount; idx++)
    {
        L.tim[idx].event_offset = newOff;
        const int64_t target = timingArr[idx + 1].event_offset;
        while (oldOff < target)
        {
            if ((g % TG_ANCHOR) == 0) L.anchors[g / TG_ANCHOR] = newOff;
            const uint8_t* p = eventArr + oldOff; const uint8_t* end = p + trackGroups.runs[g];
            int64_t w = 0;
            while (p < end)
            {
                const uint32_t st = p[0], third = p[1]; p += 2;
                uint8_t b; int n = 0;
                do { b = *p++; uint32_t k = b & 0x7F; if (!cnt[k]) keys[n++] = (uint8_t)k; cnt[k]++; } while (!(b & 0x80));
                w += encode_run(L.ev + newOff + w, st, third, cnt, mode);
                for (int i = 0; i < n; i++) cnt[keys[i]] = 0;
            }
            L.runs[g] = (uint32_t)w; newOff += w; oldOff += trackGroups.runs[g]; g++;
        }
    }
    L.tim[activetickcount].event_offset = newOff;
    L.bytes = newOff;
    return L;
}

#define NIB_NEXT(v) do { (v) = (*q >> sh) & 15; q += sh >> 2; sh ^= 4; } while (0)
#define ON_KEY(KEY) do { \
    KeyHeader* k = &kb[KEY]; \
    if (k->count == 0) \
    { \
        if (UNLIKELY((noteHead & PAGE_MASK) == 0)) { *bnp = bn; page_push(bnp); bn = *bnp; } \
        pages[pageHi - 1].open++; \
        k->slot = (uint32_t)noteHead++; \
        batch[bn++] = (RenderNote){.StartTick = (int32_t)tick, .EndTick = 0, .PackedData = packed | ((KEY) << 8)}; \
        if (UNLIKELY(bn == BATCH_NOTES)) { *bnp = bn; batch_full(bnp); bn = *bnp; } \
    } \
    k->count++; } while (0)
#define CLOSE_KEY(K) do { \
    const int64_t n = noteHead - (int64_t)((uint32_t)noteHead - (K)->slot); \
    const int64_t inBatch = n - (noteHead - bn); \
    const int64_t i = (n >> PAGE_SHIFT) - pageOrigin; \
    if (UNLIKELY(inBatch < 0 || i < pageLo || i >= pageHi || pages[i].phys < 0)) close_note((K)->slot, tick, bn); \
    else { PageInfo* pg = &pages[i]; pg->open--; if (endTick > pg->maxEnd) pg->maxEnd = endTick; batch[inBatch].EndTick = endTick; } } while (0)
#define OFF_KEY(KEY) do { \
    KeyHeader* k = &kb[KEY]; \
    if (k->count > 0 && --k->count == 0) CLOSE_KEY(k); } while (0)

#define OFF_KEYS(KEY, C) do { \
    KeyHeader* k = &kb[KEY]; \
    if (k->count > 0) { const int32_t c_ = (C); if (c_ >= k->count) { k->count = 0; CLOSE_KEY(k); } else k->count -= c_; } } while (0)
// the nibble layout, same work per event as v_spec
static int64_t v_nib(uint8_t* m, uint32_t track, int64_t cur, int64_t runEnd, int64_t tick, int* bnp)
{
    if (UNLIKELY(track >= (uint64_t)kbTracks)) kb_grow(track);
    const uint8_t* p = m + cur; const uint8_t* end = m + runEnd;
    int bn = *bnp; const int32_t endTick = tick ? (int32_t)tick : 1;
    while (p < end)
    {
        const uint32_t st = p[0], third = p[1];
        p += 2;
        KeyHeader** slot = &kbTab[((int64_t)track << 4) | (st & 0x0F)];
        KeyHeader* kb = *slot;
        if (UNLIKELY(!kb)) kb = *slot = kb_alloc();
        const uint32_t hi = st & 0xF0;
        const uint32_t b = *p++;
        uint32_t key = b & 0x7F;
        const uint8_t* q = p; unsigned sh = 0; uint32_t v;
        if (hi == 0x90)
        {
            const uint32_t packed = ((((track & 15) << 4) | (st & 0x0F)) << 16) | third;
            ON_KEY(key);
            if (!(b & 0x80))
            {
                for (;;)
                {
                    NIB_NEXT(v);
                    if (v < 12) { key += v + 1; ON_KEY(key); }
                    else if (v == 13) kb[key].count++;
                    else if (v == 15) { uint32_t lo, h2; NIB_NEXT(lo); NIB_NEXT(h2); kb[key].count += (int32_t)(lo | (h2 << 4)); }
                    else if (v == 12) { uint32_t lo, h2; NIB_NEXT(lo); NIB_NEXT(h2); key += (lo | (h2 << 4)) + 1; ON_KEY(key); }
                    else break;
                }
                p = q + (sh >> 2);
            }
        }
        else if (hi == 0x80)
        {
            OFF_KEY(key);
            if (!(b & 0x80))
            {
                for (;;)
                {
                    NIB_NEXT(v);
                    if (v < 12) { key += v + 1; OFF_KEY(key); }
                    else if (v == 13) OFF_KEY(key);
                    else if (v == 15) { uint32_t lo, h2; NIB_NEXT(lo); NIB_NEXT(h2); OFF_KEYS(key, (int32_t)(lo | (h2 << 4))); }
                    else if (v == 12) { uint32_t lo, h2; NIB_NEXT(lo); NIB_NEXT(h2); key += (lo | (h2 << 4)) + 1; OFF_KEY(key); }
                    else break;
                }
                p = q + (sh >> 2);
            }
        }
        else if (!(b & 0x80))
        {
            for (;;)
            {
                NIB_NEXT(v);
                if (v == 12 || v == 15) { NIB_NEXT(v); NIB_NEXT(v); }
                else if (v == 14) break;
            }
            p = q + (sh >> 2);
        }
    }
    *bnp = bn;
    return runEnd;
}

typedef struct { const char* name; int64_t (*fn)(uint8_t*, uint32_t, int64_t, int64_t, int64_t, int*); } V;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void sweep_all(int64_t chunk, uint64_t* pcOut)
{
    uint64_t pc = 0;
    for (int64_t from = 0; from <= maxTick; from += chunk)
    {
        sweep_range(from == 0 ? 0 : from, from + chunk - 1 < maxTick ? from + chunk - 1 : maxTick);
        if (verify) for (int64_t i = 0; i < patchN; i++) { pc = pc * 1000003u + patches[i].note * 31u + patches[i].end; uck += MIXEND(patches[i].end); }
        patchN = 0;
    }
    *pcOut = pc;
}

int main(int argc, char** argv)
{
    const char* repsArg = "3"; const char* chunkArg = "2000"; const char* files[1024]; int nf = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) repsArg = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) chunkArg = argv[++i];
        else if (nf < 1024) files[nf++] = argv[i];
    }
    if (nf == 0) { puts("usage: bench_sweep [-r reps] [-c chunk_ticks] file.mid [more.mid ...]"); return 1; }
    if (nf > 1)
    {
        // several files: one run of this program per file, so each starts from a clean state and gives its memory back
        for (int i = 0; i < nf; i++)
        {
            printf("\n-- %s --\n", files[i]); fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) { execl("/proc/self/exe", argv[0], "-r", repsArg, "-c", chunkArg, files[i], (char*)NULL); _exit(127); }
            int status = 0; waitpid(pid, &status, 0);
            if (status != 0) printf("(failed: status %d)\n", status);
        }
        return 0;
    }
    glad_glBindBuffer = s_bind; glad_glBufferSubData = s_sub;
    LoadMIDI(files[0]);
    if (!eventArr || activetickcount == 0) { puts("load failed"); return 1; }
    int reps = atoi(repsArg);
    int64_t chunk = atoll(chunkArg);
    batch = malloc(BATCH_NOTES * sizeof(RenderNote));
    freeQ = malloc(MAX_POOL_PAGES * sizeof(int));
    poolPages = MAX_POOL_PAGES;           // pages are never released here, so the pool must hold every note
    int64_t evbytes = timingArr[activetickcount].event_offset;
    uint64_t evs = 0; EV_RUNS(eventArr, eventArr + evbytes, st, third, key, (void)st; (void)third;, { evs++; (void)key; });
    printf("\nsweeping %lu events (%ld event bytes) in chunks of %ld ticks, %d reps, best time reported\n", (unsigned long)evs, (long)evbytes, (long)chunk, reps);
    V vs[] = {{"current code", process_run_events_orig}, {"decode only (floor)", v_decode}, {"decode + key count only", v_count}, {"split by status + inline close", v_spec}};
    uint64_t ref_ck = 0, ref_pc = 0, ref_head = 0, ref_uck = 0;
    for (int vi = 0; vi < 4; vi++)
    {
        runfn = vs[vi].fn;
        double best = 1e9;
        for (int r = 0; r < reps + 1; r++)
        {
            ring_reset(); kb_zero(); patchN = 0;
            verify = (r == 0 && (vi == 0 || vi == 3)); ck = 0; uck = 0;
            uint64_t pc = 0;
            double t0 = now();
            sweep_all(chunk, &pc);
            double t1 = now();
            if (verify)
            {
                if (vi == 0) { ref_ck = ck; ref_pc = pc; ref_head = noteHead; ref_uck = uck; }
                printf("   [check %s] notes %ld, same notes: %s, same ends: %s\n", vs[vi].name, (long)noteHead, (ck == ref_ck && (uint64_t)noteHead == ref_head) ? "yes" : "NO", pc == ref_pc ? "yes" : "NO");
            }
            else if (t1 - t0 < best) best = t1 - t0;
        }
        printf("%-32s %9.1f ms   %5.2f ns/event\n", vs[vi].name, best * 1e3, best * 1e9 / evs);
    }

    // ---- layouts ----
    uint8_t* oEv = eventArr; TickGroup* oTim = timingArr; uint32_t* oRuns = trackGroups.runs; int64_t* oAnch = trackGroups.anchors;
    if (!oRuns || trackGroups.count == 0) { puts("no track index, layouts skipped"); return 0; }
    printf("\nother layouts (built in memory from the loaded events; sizes are the whole event array):\n");
    for (int mode = 1; mode <= 2; mode++)
    {
        double tb = now();
        Layout L = build_layout(mode);
        printf("%-32s %9.1f MB (%.1f%% of the current %.1f MB), built in %.1f s\n", mode == 1 ? "sorted bytes" : "sorted, nibble coded", L.bytes / 1048576.0, 100.0 * L.bytes / evbytes, evbytes / 1048576.0, now() - tb);
        eventArr = L.ev; timingArr = L.tim; trackGroups.runs = L.runs; trackGroups.anchors = L.anchors;
        runfn = mode == 1 ? v_spec : v_nib;
        double best = 1e9;
        for (int r = 0; r < reps + 1; r++)
        {
            ring_reset(); kb_zero(); patchN = 0;
            verify = (r == 0); ck = 0; uck = 0;
            uint64_t pc = 0;
            double t0 = now();
            sweep_all(chunk, &pc);
            double t1 = now();
            if (verify) printf("   [check] notes %ld, same notes and ends as the current layout: %s\n", (long)noteHead, (uck == ref_uck && (uint64_t)noteHead == ref_head) ? "yes" : "NO");
            else if (t1 - t0 < best) best = t1 - t0;
        }
        printf("%-32s %9.1f ms   %5.2f ns/event\n", mode == 1 ? "sorted bytes, split sweep" : "nibbles, nibble sweep", best * 1e3, best * 1e9 / evs);
        eventArr = oEv; timingArr = oTim; trackGroups.runs = oRuns; trackGroups.anchors = oAnch;
        free(L.ev); free(L.tim); free(L.runs); free(L.anchors);
    }
    (void)sink;
    return 0;
}
