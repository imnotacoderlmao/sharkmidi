#include "parser.h"
#include "midistorage.h"
#include "../misc/typed-array.h"
#include "../misc/util.h"
#include "../playback/timer.h"
#include "../playback/playback_thread.h"
#include "../third_party/conmidi_digit_formatter.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdarg.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef _OPENMP
    #define ATOMIC_ADD(a, b) __atomic_fetch_add(&(a), (b), __ATOMIC_RELAXED)
#else
    #define ATOMIC_ADD(a, b) ((a) += (b))
#endif

typedef struct
{
    int64_t start;
    uint32_t length;
} trackProperties;

// one tick's worth of events in one track. 16 bytes instead of the old 24, because the global
// offset reuses the tick field once the timing merge no longer needs it
typedef struct
{
    union { int64_t tick; int64_t offset; };
    uint32_t count;     // events in the group
    uint32_t notecount; // note-ons in the group
} TrackTick;

static uint8_t* filePtr = NULL;
static uint64_t fileLen = 0;
static uint64_t filePos = 0;
static uint16_t trackAmount = 0;
int32_t midiloaded = 0, trackcolors = 1;
static uint64_t eventcount = 0; // channel events
static uint64_t bytecount = 0;  // bytes they take in eventArr (see midistorage.h)

// off unless the environment variable is set (and not "0"):
//   SHARKMIDI_TRACE=1          prints where the time between the counted phases goes
static int g_trace = 0;
// events are parsed in output order tiles (see ParseTiled). SHARKMIDI_TILE_KB sets the output size of one tile
// in KB (default 1024, or more for files with many tracks, see ParseTiled)
static int g_tile_kb = 1024;
static int g_tile_set = 0; // the size was asked for, so it is used as it is
typedef struct
{
    int64_t tile_kb;
    double mainalloc, rank_alloc, rank_bounds, rank_pass1, rank_scan, rank_pass2, rank_free, compact, evalloc, tail;
    int rank_used;
} TraceTimes;
static TraceTimes tr;
// adds the time since *mark to tr.field and restarts the mark. costs one branch when tracing is off
#define TR_LAP(field, mark) do { if (g_trace) { double now_ = get_time(); tr.field += now_ - (mark); (mark) = now_; } } while (0)
static int env_flag(const char* name)
{
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

DEFINE_ARRAY(trackProperties);
DEFINE_ARRAY(TrackTick);

// where a track can be picked up again without reading it from the start: every TILE_CP_STEP groups, Count notes
// the byte position (relative to the track start), the absolute tick and the running status. checkpoint c (1 based)
// is the start of group c * TILE_CP_STEP, always at a group boundary
#define TILE_CP_STEP 64
typedef struct
{
    int64_t abs;
    int64_t off;    // event offset of the group, filled in once the merge knows it (INT64_MAX: no such group)
    uint32_t pos;
    uint8_t prev;
} Checkpoint;
DEFINE_ARRAY(Checkpoint);
DEFINE_ARRAY(TempoEvent);
DEFINE_ARRAY(SysExEvent);

static const char* getfilename(const char* filedir) {
    int i = strlen(filedir) - 1;
    while (i >= 0) {
        if (filedir[i] == '/' || filedir[i] == '\\') {
            break;
        }
        i--;
    }

    const char* start_ptr = filedir + (i + 1);
    filename_len = strlen(start_ptr) + 1;
    char* filename = malloc((filename_len + 1) * sizeof(char));
    strcpy(filename, start_ptr);
    if(filename_len > 32)
        filename[filename_len-1] = ' ';
    filename[filename_len] = '\0';
    
    return filename;
}

static char static_filename_buf[512];
static void updatestatsandprint(const char* fmt, ...)
{
    va_list args;
    char temp[512];

    va_start(args, fmt);
    vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);

    fputs(temp, stdout);
    fflush(stdout);

    char *src = temp, *dst = static_filename_buf;
    while (*src)
    {
        if (*src != '\n' && *src != '\r')
            *dst++ = *src;
        src++;
    }
    *dst = '\0';

    filename = static_filename_buf;
}

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
int munmap(void* addr, size_t length) 
{
    return UnmapViewOfFile(addr) ? 0 : -1;
}
static int InitMMF(const char* filepath)
{
    HANDLE filehandle = CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER fileSize;
    if (filehandle == INVALID_HANDLE_VALUE || !GetFileSizeEx(filehandle, &fileSize) || fileSize.QuadPart == 0)
    {
        updatestatsandprint("error getting file properties, does the file even exist?");
        if (filehandle != INVALID_HANDLE_VALUE)
            CloseHandle(filehandle);
        return 0;
    }
    HANDLE mapfilehandle = CreateFileMapping(filehandle, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(filehandle);
    if (!mapfilehandle)
    {
        updatestatsandprint("error creating file mapping");
        return 0;
    }
    fileLen = fileSize.QuadPart;
    filePos = 0;
    filePtr = MapViewOfFile(mapfilehandle, FILE_MAP_READ, 0, 0, 0);
    WIN32_MEMORY_RANGE_ENTRY entry;
    entry.VirtualAddress = filePtr;
    entry.NumberOfBytes = fileLen;
    if (!PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, 0))
        puts("prefetching mapped file failed. your file might be slow to load if this is the first time you do");
    CloseHandle(mapfilehandle);
    if (!filePtr)
    {
        updatestatsandprint("error mapping file view");
        return 0;
    }
    printf("successfully initiated memory mapped file with %llu bytes in size\n", (unsigned long long)fileLen);
    return 1;
}
#else
#include <sys/mman.h>
static int InitMMF(const char* filepath)
{
    struct stat filestat;
    int filedesc = open(filepath, O_RDONLY);
    if (filedesc == -1 || fstat(filedesc, &filestat) == -1 || filestat.st_size == 0)
    {
        updatestatsandprint("error getting filesize, does the file even exist?\n");
        if (filedesc != -1)
            close(filedesc);
        return 0;
    }
    fileLen = filestat.st_size;
    filePos = 0;
    filePtr = mmap(NULL, fileLen, PROT_READ, MAP_PRIVATE, filedesc, 0);
    close(filedesc);
    if (filePtr == MAP_FAILED)
    {
        filePtr = NULL;
        updatestatsandprint("mmap failed\n");
        return 0;
    }
    if (madvise(filePtr, fileLen, MADV_WILLNEED) == -1)
        puts("prefetching mapped file failed. your file might be slow to load if this is the first time you do");
    printf("successfully initiated memory mapped file with %llu bytes in size\n", (unsigned long long)fileLen);
    return 1;
}
#endif

static int64_t __attribute__((noinline)) varlen_decode_slow(int64_t* len, uint8_t** trackPtr)
{
    uint8_t b = *(*trackPtr)++;
    int64_t val = ((*len & 0x7F) << 7) | (b & 0x7F);
    if (!(b & 0x80)) return (*len = val);

    b = *(*trackPtr)++;
    val = (val << 7) | (b & 0x7F);
    if (!(b & 0x80)) return (*len = val);

    b = *(*trackPtr)++;
    val = (val << 7) | (b & 0x7F);
    return (*len = val);
}

static uint32_t ReadUInt32(void)
{
    uint32_t val;
    memcpy(&val, filePtr + filePos, 4);
    filePos += 4;
    return __builtin_bswap32(val);
}

static uint16_t ReadUInt16(void)
{
    uint16_t val;
    memcpy(&val, filePtr + filePos, 2);
    filePos += 2;
    return __builtin_bswap16(val);
}

static int SearchText(char* text)
{
    for (int32_t i = 0; i < strlen(text); i++)
    {
        if (filePos >= fileLen) 
            return 0;
        if (filePtr[filePos++] != text[i])
        {
            printf("issue searching for %s\n", text);
            return 0;
        }
    }
    return 1;
}

static void VerifyHeader()
{
    if (SearchText("MThd"))
    {
        uint32_t headersize = ReadUInt32();
        uint32_t fmt = ReadUInt16();
        filePos += 2;
        ppq = ReadUInt16();
        if (fmt == 2) 
            puts("MIDI format 2 unsupported");
        if (headersize != 6) 
            printf("Incorrect header size of %d\n", headersize);
    }
    else
    {
        puts("Header issue");
    }
}

static int IndexTrack(trackProperties_arr* tracklistptr)
{
    if (SearchText("MTrk"))
    {
        if (filePos + 4 > fileLen)
            return 0;
        uint32_t size = ReadUInt32();
        if (filePos + size > fileLen) // truncated file, clamp so the parser can't run off the mapping
            size = (uint32_t)(fileLen - filePos);
        trackProperties track = {filePos, size};
        trackProperties_arr_push(tracklistptr, track);
        filePos += size;
        trackAmount++;
        return 1;
    }
    return 0;
}

static int cmp_tempo(const void* a, const void* b)
{
    int64_t posa = ((const TempoEvent*)a)->tick;
    int64_t posb = ((const TempoEvent*)b)->tick;
    return (posa > posb) - (posa < posb);
}

static int cmp_sysex(const void* a, const void* b)
{
    int64_t posa = ((const SysExEvent*)a)->tick;
    int64_t posb = ((const SysExEvent*)b)->tick;
    return (posa > posb) - (posa < posb);
}

// the two hot parsers are only called from LoadMIDI. noipa keeps them out of line and free of the interprocedural
// tweaks (cloning, register allocation) that being static would allow, which measured a few percent slower
#if defined(__GNUC__) && !defined(__clang__)
    #define NOIPA __attribute__((noipa))
#else
    #define NOIPA __attribute__((noinline))
#endif

// cold path of the group array growth (same policy as TrackTick_arr_push)
__attribute__((noinline, cold))
static void tg_grow(TrackTick_arr* a)
{
    a->cap = a->cap < 1024 ? 1024 : a->cap * 2;
    a->data = realloc(a->data, a->cap * sizeof(TrackTick));
}

// peeks at the next event and says whether it is a plain channel event with a 1 or 2 byte delta,
// which is what the fast loops handle. nothing is consumed or modified
static inline __attribute__((always_inline))
int peek_channel_event(const uint8_t* p, uint32_t* delta, const uint8_t** q, uint32_t* ev)
{
    uint32_t b0 = p[0], b1 = p[1];
    if (b0 < 0x80) 
    { 
        *delta = b0; 
        *q = p + 1; 
    }
    else if (b1 < 0x80) 
    { 
        *delta = ((b0 & 0x7F) << 7) | b1; 
        *q = p + 2; 
    }
    else 
        return 0;
    *ev = **q;
    return *ev < 0xF0;
}

// eventArr holds runs (see midistorage.h): status, third byte, then one byte per event. the key of an event is
// (status << 8 | third byte) as stored, a note on with velocity 0 is stored as a note off with velocity 64. a run
// is a stretch of events of one group with the same key, Count has to know exactly how many bytes that makes
#define RSB_NOKEY 0xFFFFFFFFu
static inline __attribute__((always_inline)) uint32_t rsb_key(uint32_t st, uint32_t d2)
{
    uint32_t vel0 = ((st & 0xF0) == 0x90) & (d2 == 0);
    uint32_t off = vel0 | ((st & 0xF0) == 0x80);
    return ((st ^ (vel0 << 4)) << 8) | (off ? 64u : (d2 & 0x7F));
}

// cps / cpstep: checkpoints every cpstep groups are appended to cps. cpstep 0 records none.
// returns the number of channel events, *bytes_out gets the bytes they take
static NOIPA int64_t CountTrackEvents(uint8_t* trackPtr, uint8_t* trackEnd, TrackTick_arr* tickgroup, Checkpoint_arr* cps, int64_t cpstep, int64_t* bytes_out)
{
    const uint8_t* trackBase = trackPtr;
    int64_t nextcp = cpstep > 0 ? cpstep : INT64_MAX;
    // n only grows when a group closes, so this fires right after the group boundary that starts group n. the
    // state recorded is the one just before the event that follows the boundary
    #define CP_CHECK(P) do { if (UNLIKELY(n >= nextcp)) { Checkpoint cp_ = {.abs = absolutetime, .off = 0, .pos = (uint32_t)((P) - trackBase), .prev = prevEvent}; Checkpoint_arr_push(cps, cp_); nextcp += cpstep; } } while (0)
    int64_t totalnotes_local = 0;
    int64_t eventcount_local = 0;
    int64_t bytes_local = 0;
    int64_t absolutetime = 0;
    uint8_t prevEvent = 0;
    uint32_t count = 0;
    uint32_t notecount = 0;
    uint32_t runs = 0;              // runs in the open group
    uint32_t lastkey = RSB_NOKEY;   // key of the last event of the open group
    // the group array lives in locals for the whole function and is written back on exit
    TrackTick* data = tickgroup->data;
    int64_t n = tickgroup->count, cap = tickgroup->cap;
    #define TG_ENSURE() do { if (UNLIKELY(n >= cap)) { tickgroup->count = n; tg_grow(tickgroup); data = tickgroup->data; cap = tickgroup->cap; } } while (0)

    while (trackPtr < trackEnd)
    {
        // fast path: plain channel events with a 1 or 2 byte delta. 8 bytes of margin cover every byte it peeks at.
        // anything unusual breaks out BEFORE touching any state, so the generic code below sees the event untouched
        while (trackEnd - trackPtr >= 8)
        {
            uint32_t delta, ev;
            const uint8_t* q;
            if (UNLIKELY(!peek_channel_event(trackPtr, &delta, &q, &ev))) break;

            // close the current group when time advances, without a branch: the slot is always written,
            // the index only moves if the group really ended
            TG_ENSURE();
            data[n] = (TrackTick){.tick = absolutetime, .count = count + 2 * runs, .notecount = notecount};
            uint32_t flush = (delta != 0) & (count != 0);
            uint32_t keep = flush - 1;
            n += flush;
            eventcount_local += count & ~keep;
            bytes_local += (count + 2 * runs) & ~keep;
            totalnotes_local += notecount & ~keep;
            count &= keep;
            notecount &= keep;
            runs &= keep;
            lastkey |= ~keep; // a new group starts a new run
            CP_CHECK(trackPtr);
            absolutetime += delta;
            uint32_t st = ev;
            if (ev >= 0x80) { prevEvent = ev; q++; }
            else st = prevEvent;
            count++;
            // program change / channel pressure have one data byte. the length stays a predicted branch so the
            // next event's address doesn't wait on this event's bytes
            uint32_t key;
            if (UNLIKELY((st & 0xE0) == 0xC0))
            {
                key = rsb_key(st, 0);
                runs += key != lastkey;
                lastkey = key;
                trackPtr = (uint8_t*)q + 1;
                continue;
            }
            uint32_t d2 = q[1];
            notecount += ((st & 0xF0) == 0x90) & (d2 != 0);
            key = rsb_key(st, d2);
            runs += key != lastkey;
            lastkey = key;
            trackPtr = (uint8_t*)q + 2;
        }
        if (trackPtr >= trackEnd) break;

        // generic path: one event, anything goes (tail of the track, meta, sysex, long deltas)
        uint8_t* evstart = trackPtr;
        int64_t delta = *trackPtr++;
        if (delta >= 0x80)
            delta = varlen_decode_slow(&delta, &trackPtr);
        if(delta > 0)
        {
            if (count > 0)
            {
                TG_ENSURE();
                data[n++] = (TrackTick){.tick = absolutetime, .count = count + 2 * runs, .notecount = notecount};
                eventcount_local += count;
                bytes_local += count + 2 * runs;
                totalnotes_local += notecount;
                notecount = 0;
                count = 0;
                runs = 0;
                lastkey = RSB_NOKEY;
                CP_CHECK(evstart);
            }
            absolutetime += delta;
        }
        
        uint8_t readEvent = *trackPtr++;
        if (readEvent >= 0x80)
        {
            if (readEvent < 0xF0)
            {
                prevEvent = readEvent;
                uint32_t d2 = ((readEvent & 0xE0) == 0xC0) ? 0 : trackPtr[1];
                if ((readEvent & 0xF0) == 0x90 && d2 != 0)
                    notecount++;
                count++;
                uint32_t key = rsb_key(readEvent, d2);
                runs += key != lastkey;
                lastkey = key;
                trackPtr += ((readEvent & 0xE0) == 0xC0) ? 1 : 2;
            }
            else
            {
                switch (readEvent)
                {
                    case 0xF0:
                    {
                        int64_t len = *trackPtr++;
                        if (len >= 0x80)
                            varlen_decode_slow(&len, &trackPtr);
                        trackPtr += len;
                        continue;
                    }
                    case 0xF2: 
                        trackPtr += 2; 
                        continue;
                    case 0xF1:
                    case 0xF3: 
                        trackPtr += 1; 
                        continue;
                    case 0xFF:
                        readEvent = *trackPtr++;
                        if (readEvent != 0x2F)
                        {
                            int64_t len2 = *trackPtr++;
                            if (len2 >= 0x80)
                                varlen_decode_slow(&len2, &trackPtr);
                            trackPtr += len2;
                        }
                        else
                            goto finalize;
                        continue;
                }        
           }
        }
        else
        {
            uint32_t d2 = ((prevEvent & 0xE0) == 0xC0) ? 0 : *trackPtr;
            if ((prevEvent & 0xF0) == 0x90 && d2 != 0)
                notecount++;
            count++;
            uint32_t key = rsb_key(prevEvent, d2);
            runs += key != lastkey;
            lastkey = key;
            trackPtr += ((prevEvent & 0xE0) == 0xC0) ? 0 : 1;
        }
    }
finalize:;
    #ifdef _OPENMP
        #pragma omp critical
    #endif
    if (absolutetime > maxTick)
        maxTick = absolutetime;
    if (count > 0)
    {
        TG_ENSURE();
        data[n++] = (TrackTick){.tick = absolutetime, .count = count + 2 * runs, .notecount = notecount};
        eventcount_local += count;
        bytes_local += count + 2 * runs;
        totalnotes_local += notecount;
    }
    tickgroup->count = n;
    ATOMIC_ADD(totalnotes, totalnotes_local);
    *bytes_out = bytes_local;
    return eventcount_local;
    #undef CP_CHECK
    #undef TG_ENSURE
}

// writes one event of the open group. a different key than the last event's closes that run (its last note byte
// gets bit 7) and opens a new one with its two header bytes
static inline __attribute__((always_inline))
void rsb_emit(uint8_t* out, int64_t* pos, uint32_t* lastkey, uint32_t key, uint32_t note)
{
    int64_t p = *pos;
    if (key != *lastkey)
    {
        if (*lastkey != RSB_NOKEY)
            out[p - 1] |= 0x80;
        out[p] = (uint8_t)(key >> 8);
        out[p + 1] = (uint8_t)key;
        p += 2;
        *lastkey = key;
    }
    out[p++] = (uint8_t)(note & 0x7F);
    *pos = p;
}

// the open group is over (the next one, or the end of the track): close its last run
static inline __attribute__((always_inline))
void rsb_close(uint8_t* out, int64_t pos, uint32_t* lastkey)
{
    if (*lastkey != RSB_NOKEY)
    {
        out[pos - 1] |= 0x80;
        *lastkey = RSB_NOKEY;
    }
}

// decodes one channel event (q points at its status or, with running status, its first data byte),
// stores it and returns the pointer past it
static inline __attribute__((always_inline))
const uint8_t* decode_emit(const uint8_t* q, uint32_t ev, uint8_t* prevEvent, uint8_t* out, int64_t* pos, uint32_t* lastkey, int64_t* notecount)
{
    uint32_t st = ev;
    if (ev >= 0x80)
    {
        *prevEvent = (uint8_t)ev;
        q++;
    }
    else st = *prevEvent;
    uint32_t d1 = q[0];
    // the event length stays a predicted branch (not arithmetic on loaded data) so the next event's address
    // doesn't wait on this event's bytes. program change / channel pressure have a single data byte
    if (UNLIKELY((st & 0xE0) == 0xC0))
    {
        rsb_emit(out, pos, lastkey, rsb_key(st, 0), d1);
        return q + 1;
    }
    uint32_t d2 = q[1];
    *notecount += ((st & 0xF0) == 0x90) & (d2 != 0);
    rsb_emit(out, pos, lastkey, rsb_key(st, d2), d1);
    return q + 2;
}

// ---- tiled parse -------------------------------------------------------------------------------------------
// walking one track from tick 0 to the end would make all threads write at the same tick region of the event
// array at the same time, each into groups interleaved with the others. so, every thread
// owns a slice of the output instead and fills it tile (a few MB, small enough to stay in cache) at a time, visiting every
// track for each tile. a track is resumed where it stopped through a "cursor", and a slice starts from the nearest
// Count checkpoint of each track, skimming the few groups up to the slice start.
// events belong to the group that is open when they are read; a call handles the groups that start below event
// offset endOff and stops, before reading anything of the first group that doesn't, at the event that would open
// it. the next call resumes there, so every event, meta events included, is handled exactly once
typedef struct
{
    uint8_t* ptr;
    int64_t abs;
    int64_t offset;    // event offset of group (where its first event goes)
    uint32_t group;
    uint8_t prev;
} Cursor;

static inline __attribute__((always_inline))
const uint8_t* decode_skip(const uint8_t* q, uint32_t ev, uint8_t* prevEvent)
{
    uint32_t st = ev;
    if (ev >= 0x80)
    {
        *prevEvent = (uint8_t)ev;
        q++;
    }
    else st = *prevEvent;
    return q + (UNLIKELY((st & 0xE0) == 0xC0) ? 1 : 2);
}

// store 0 only advances the cursor: nothing is written, counted or collected
// gdv: the track's group distances (from one group's start to the next one's), 4 bytes each or, when wide, 8 bytes.
// gdv NULL is a track whose groups are back to back from cur->offset (no other track interleaves, so there is
// nothing to look up) or that has no channel events at all and is only read for its tempo and sysex events;
// endOff is then unused and one call parses the whole track
static NOIPA int64_t ParseRange(Cursor* cur, uint8_t* trackEnd, uint8_t* msgPtr, const void* gdv, int64_t endOff, TempoEvent_arr* tempo, SysExEvent_arr* sysex, const int store, const int wide)
{
    #define GD(i) (wide ? ((const int64_t*)gdv)[i] : (int64_t)((const uint32_t*)gdv)[i])
    uint8_t* trackPtr = cur->ptr;
    int64_t absolutetime = cur->abs;
    uint8_t prevEvent = cur->prev;
    uint32_t group_idx = cur->group;
    int64_t notecount = 0;
    uint32_t lastkey = RSB_NOKEY; // NOKEY while the open group has no events. a skim only marks "has events" with 0
    int64_t write_pos = cur->offset;
    int64_t next_off = gdv ? write_pos + GD(group_idx) : 0; // two spare entries behind the last group make this safe

    while (trackPtr < trackEnd)
    {
        while (trackEnd - trackPtr >= 8)
        {
            uint32_t delta, ev;
            const uint8_t* q;
            if (UNLIKELY(!peek_channel_event(trackPtr, &delta, &q, &ev))) break;
            uint32_t newgroup = (delta != 0) & (lastkey != RSB_NOKEY);
            if (newgroup)
            {
                if (store) rsb_close(msgPtr, write_pos, &lastkey);
                lastkey = RSB_NOKEY;
                group_idx++;
                if (gdv)
                {
                    write_pos = next_off;
                    if (UNLIKELY(write_pos >= endOff)) goto stop; // nothing of this event consumed yet
                    next_off = write_pos + GD(group_idx);
                }
            }
            absolutetime += delta;
            if (store) trackPtr = (uint8_t*)decode_emit(q, ev, &prevEvent, msgPtr, &write_pos, &lastkey, &notecount);
            else
            {
                trackPtr = (uint8_t*)decode_skip(q, ev, &prevEvent);
                lastkey = 0;
            }
        }
        if (trackPtr >= trackEnd) break;

        uint8_t* evstart = trackPtr;
        int64_t delta = *trackPtr++;
        if (delta >= 0x80)
            delta = varlen_decode_slow(&delta, &trackPtr);
        if (delta > 0 && lastkey != RSB_NOKEY)
        {
            if (store) rsb_close(msgPtr, write_pos, &lastkey);
            lastkey = RSB_NOKEY;
            group_idx++;
            if (gdv)
            {
                write_pos = next_off;
                if (write_pos >= endOff)
                {
                    trackPtr = evstart;
                    goto stop;
                }
                next_off = write_pos + GD(group_idx);
            }
        }

        absolutetime += delta;
        uint8_t readEvent = *trackPtr++;
        if (readEvent >= 0x80)
        {
            if (readEvent < 0xF0)
            {
                prevEvent = readEvent;
                uint32_t data1 = *trackPtr++;
                uint32_t data2 = 0;
                if ((readEvent & 0xE0) != 0xC0)
                    data2 = *trackPtr++;
                if (!store)
                {
                    lastkey = 0;
                    continue;
                }
                notecount += ((readEvent & 0xF0) == 0x90) & (data2 != 0);
                rsb_emit(msgPtr, &write_pos, &lastkey, rsb_key(readEvent, data2), data1);
            }
            else
            {
                switch (readEvent)
                {
                    case 0xF0:
                    {
                        int64_t size = *trackPtr++;
                        if (size >= 0x80)
                            size = varlen_decode_slow(&size, &trackPtr);
                        if (size > trackEnd - trackPtr) // a length running past the end of the track is corrupt, nothing there to read
                        {
                            trackPtr = trackEnd;
                            continue;
                        }
                        if (!store)
                        {
                            trackPtr += size;
                            continue;
                        }
                        uint8_t* data = malloc(size + 1);
                        data[0] = readEvent;
                        for (uint32_t i = 1; i < (uint32_t)(size + 1); i++)
                            data[i] = *trackPtr++;

                        uint8_t sysextoignore[] = {0xF0,0x7E,0x7F,0x09,0x01,0xF7};
                        if (size + 1 >= (int64_t)sizeof(sysextoignore) && memcmp(data, sysextoignore, sizeof(sysextoignore)) == 0)
                        {
                            free(data);
                            continue;
                        }
                        SysExEvent sex = {absolutetime, size + 1, data};
                        #ifdef _OPENMP
                            #pragma omp critical
                        #endif
                        SysExEvent_arr_push(sysex, sex);
                        continue;
                    }
                    case 0xF2:
                        trackPtr += 2;
                        continue;
                    case 0xF1:
                    case 0xF3:
                        trackPtr += 1;
                        continue;
                    case 0xFF:
                    {
                        readEvent = *trackPtr++;
                        if (readEvent == 0x51)
                        {
                            int64_t len = *trackPtr++;
                            if (len >= 0x80)
                                len = varlen_decode_slow(&len, &trackPtr);
                            if (len > trackEnd - trackPtr) // runs past the end of the track: corrupt
                            {
                                trackPtr = trackEnd;
                                continue;
                            }
                            uint32_t tempoVal = 0; // 3 bytes in a valid file, a longer one is read for its first 4
                            for (int i = 0; i < len && i < 4; i++)
                                tempoVal = (tempoVal << 8) | trackPtr[i];
                            trackPtr += len;
                            if (store)
                            {
                                TempoEvent tev = {absolutetime, (int32_t)tempoVal};
                                #ifdef _OPENMP
                                    #pragma omp critical
                                #endif
                                TempoEvent_arr_push(tempo, tev);
                            }
                        }
                        else if (readEvent == 0x2F)
                        {
                            trackPtr = trackEnd;
                            goto stop;
                        }
                        else
                        {
                            int64_t len = *trackPtr++;
                            if (len >= 0x80)
                                len = varlen_decode_slow(&len, &trackPtr);
                            trackPtr += len;
                        }
                        continue;
                    }
                }
            }
        }
        else
        {
            // running status: readEvent is the first data byte
            uint32_t data2 = 0;
            if ((prevEvent & 0xE0) != 0xC0)
                data2 = *trackPtr++;
            if (!store)
            {
                lastkey = 0;
                continue;
            }
            notecount += ((prevEvent & 0xF0) == 0x90) & (data2 != 0);
            rsb_emit(msgPtr, &write_pos, &lastkey, rsb_key(prevEvent, data2), readEvent);
        }
    }
stop:
    if (store) rsb_close(msgPtr, write_pos, &lastkey); // a no-op when the stop was at a group boundary
    cur->ptr = trackPtr;
    cur->abs = absolutetime;
    cur->prev = prevEvent;
    cur->group = group_idx;
    cur->offset = write_pos;
    return notecount;
    #undef GD
}

// a track goes through ParseTiled when it has group distances to walk (data is NULL when its groups are contiguous)
static int track_is_tiled(const TrackTick_arr* h)
{
    return h->data != NULL && h->count > 0;
}

// the group tables (4 or 8 bytes per group, see compact_offsets) are only read from front to back, and every slice
// of ParseTiled starts at a Count checkpoint. so once all slices below some point are done, nothing reads the front
// of a table anymore and its pages can go back to the system, while the event array fills up. this keeps the peak
// at what the loaded file takes in the end instead of that plus all of the tables. a no-op on Windows.
#if !defined(_WIN32) && !defined(_WIN64)
#ifndef TABLE_RELEASE_MIN
#define TABLE_RELEASE_MIN (128 * 1024) // give back at least this much at a time, every call is a system call
#endif
static void release_tables(TrackTick_arr* hist, const uint8_t* wide, const int32_t* tl, int32_t nt, const Checkpoint_arr* cps, int64_t cpstep, int64_t t0, uintptr_t* freed_to)
{
    static long pagesz = 0;
    if (!pagesz) pagesz = sysconf(_SC_PAGESIZE);
    for (int32_t k = 0; k < nt; k++)
    {
        int32_t i = tl[k];
        // the last checkpoint that starts before t0: a slice that starts at t0 or later never reads below its group
        int64_t lo = 0, hi = cps[i].count;
        while (lo < hi)
        {
            int64_t mid = lo + (hi - lo + 1) / 2;
            if (cps[i].data[mid - 1].off < t0) lo = mid; else hi = mid - 1;
        }
        uintptr_t end = ((uintptr_t)hist[i].data + (size_t)(lo * cpstep) * (wide[i] ? 8 : 4)) & ~(uintptr_t)(pagesz - 1);
        if (end > freed_to[k] && end - freed_to[k] >= TABLE_RELEASE_MIN)
        {
            madvise((void*)freed_to[k], end - freed_to[k], MADV_DONTNEED);
            freed_to[k] = end;
        }
    }
}
#endif

static void ParseTiled(const trackProperties_arr* tracks, TrackTick_arr* hist, const uint8_t* wide, const int64_t* startOffsets, const Checkpoint_arr* cps, int64_t cpstep, int32_t ntracks, int64_t total, TempoEvent_arr* tempo, SysExEvent_arr* sysex)
{
    int32_t* tl = malloc((size_t)ntracks * sizeof(int32_t));
    int32_t nt = 0;
    for (int32_t i = 0; i < ntracks; i++)
        if (track_is_tiled(&hist[i]))
            tl[nt++] = i;
    if (nt == 0 || total <= 0)
    {
        free(tl);
        return;
    }
    int threads = 1;
    #ifdef _OPENMP
        threads = omp_get_max_threads();
    #endif
    int64_t bev = (int64_t)g_tile_kb * 1024; // bytes of eventArr per tile
    if (!g_tile_set)
    {
        // a tile visits every track, and every visit costs a call and a cache miss or two on that track's input
        // before it does any work. with many tracks a tile has to be bigger so each visit gets real work, about
        // 2 KB of output per track, but never so big that a thread has fewer than 4 tiles to share out
        int64_t want = (int64_t)nt * 2048, cap = total / ((int64_t)threads * 4);
        if (want > cap) want = cap;
        if (want > bev) bev = want;
    }
    if (bev < 1) bev = 1;
    tr.tile_kb = bev >> 10;
    int64_t nb = (total + bev - 1) / bev;
    int64_t ns = (int64_t)threads * 8; // several slices per thread, they are claimed dynamically
    if (ns > nb) ns = nb;
    #if !defined(_WIN32) && !defined(_WIN64)
    uint8_t* sdone = calloc((size_t)ns, 1);      // slices that are finished
    uintptr_t* freed_to = malloc((size_t)nt * sizeof(uintptr_t)); // per track: the table is given back up to here
    long pg = sysconf(_SC_PAGESIZE);
    for (int32_t k = 0; k < nt; k++)
        freed_to[k] = ((uintptr_t)hist[tl[k]].data + pg - 1) & ~(uintptr_t)(pg - 1);
    int relbusy = 0;
    int64_t relwm = 0; // all slices below this are done, only touched by the thread holding relbusy
    #endif

    #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1)
    #endif
    for (int64_t s = 0; s < ns; s++)
    {
        int64_t b0 = nb * s / ns, b1 = nb * (s + 1) / ns;
        Cursor* cur = malloc((size_t)nt * sizeof(Cursor));
        int64_t notes = 0;
        for (int32_t k = 0; k < nt; k++)
        {
            int32_t i = tl[k];
            const void* gd = hist[i].data;
            const int w = wide[i];
            uint8_t* ts = filePtr + tracks->data[i].start;
            uint8_t* te = ts + tracks->data[i].length;
            cur[k] = (Cursor){ts, 0, startOffsets[i], 0, 0};
            if (b0 > 0)
            {
                // the last checkpoint whose group starts before the slice does, then skim up to the slice start
                int64_t t0 = b0 * bev, lo = 0, hi = cps ? cps[i].count : 0;
                while (lo < hi)
                {
                    int64_t mid = lo + (hi - lo + 1) / 2;
                    if (cps[i].data[mid - 1].off < t0) lo = mid; else hi = mid - 1;
                }
                if (lo > 0)
                {
                    Checkpoint cp = cps[i].data[lo - 1];
                    cur[k] = (Cursor){ts + cp.pos, cp.abs, cp.off, (uint32_t)(lo * cpstep), cp.prev};
                }
                if (cur[k].offset < t0) // a track whose first group is already in the slice has nothing to skim
                    ParseRange(&cur[k], te, NULL, gd, t0, NULL, NULL, 0, w);
            }
        }
        for (int64_t b = b0; b < b1; b++)
        {
            int last = b == nb - 1; // the last tile also takes whatever follows the final group (trailing meta events)
            for (int32_t k = 0; k < nt; k++)
            {
                int32_t i = tl[k];
                uint8_t* te = filePtr + tracks->data[i].start + tracks->data[i].length;
                if (cur[k].ptr >= te) continue;
                int64_t e = last ? INT64_MAX : (b + 1) * bev;
                if (cur[k].offset >= e) continue; // the next group of this track starts in a later tile
                notes += ParseRange(&cur[k], te, eventArr, hist[i].data, e, tempo, sysex, 1, wide[i]);
            }
        }
        ATOMIC_ADD(playednotes, notes);
        free(cur);
        #if !defined(_WIN32) && !defined(_WIN64)
        __atomic_store_n(&sdone[s], 1, __ATOMIC_RELEASE);
        if (!__atomic_exchange_n(&relbusy, 1, __ATOMIC_ACQUIRE))
        {
            int64_t w = relwm;
            while (w < ns && __atomic_load_n(&sdone[w], __ATOMIC_ACQUIRE)) w++;
            if (w > relwm && w < ns)
                release_tables(hist, wide, tl, nt, cps, cpstep, (nb * w / ns) * bev, freed_to);
            relwm = w;
            __atomic_store_n(&relbusy, 0, __ATOMIC_RELEASE);
        }
        #endif
        
    }
    #if !defined(_WIN32) && !defined(_WIN64)
    free(sdone);
    free(freed_to);
    #endif
    free(tl);
}


static double counttime = 0.0;
static double mergetime = 0.0;
static double tickcounttime = 0.0;
static double parsetime = 0.0;
static void printparsestatistics(void)
{
    char parsestatistics[] =
    "=============== PARSE STATISTICS ===============\n"
    "   MIDI Name:              %s\n"
    "   Filesize:               %s Bytes\n"
    "   Took:\n"
    "       Count:              %lfs (%s notes/s)\n"
    "       Merge (count):      %lfs (%s ticks/s)\n"
    "       Merge (rank):       %lfs (%s ticks/s)\n"
    "       Parse:              %lfs (%s notes/s)\n"
    "   Properties:\n"
    "       PPQ / Ticks:        %s / %s (%s has events)\n"
    "       Tracks:             %s\n"
    "       Channel Events:     %s\n"
    "       Note ON Events:     %s\n"
    "       Tempo Events:       %s\n"
    "       SysEx Events:       %s\n"
    "   Memory Usage:\n"
    "       Timing:             %s Bytes (24 bytes/entry)\n"
    "       Events:             %s Bytes (%.2f bytes/event)\n"
    "       Track Index (RLE):  %s Bytes (%s groups)\n"
    "       Total:              %s Bytes\n"
    "===============================================\n";

    uint64_t trackRLEBytes = trackGroups.runs ? trackGroups.count * (sizeof(uint32_t) + sizeof(uint16_t)) + ((trackGroups.count + TG_ANCHOR - 1) / TG_ANCHOR) * sizeof(int64_t) : 0;
    printf(parsestatistics, filename, 
        AddCommas(fileLen), counttime, AddCommas((totalnotes / counttime)), tickcounttime, AddCommas((maxTick / tickcounttime)), mergetime, AddCommas((maxTick / mergetime)), parsetime,
        AddCommas((totalnotes / parsetime)), AddCommas(ppq), AddCommas(maxTick), AddCommas(activetickcount), AddCommas(trackAmount),
        AddCommas(eventcount), AddCommas(totalnotes), AddCommas(tempoCount - 1), AddCommas(sysexCount - 1),
        AddCommas(activetickcount * sizeof(TickGroup)), AddCommas(bytecount), (double)bytecount / (double)(eventcount ? eventcount : 1),
        AddCommas(trackRLEBytes), AddCommas(trackGroups.runs ? trackGroups.count : 0),
        AddCommas((activetickcount * sizeof(TickGroup)) + bytecount + trackRLEBytes));
}

static void printtrace(void)
{
    int threads = 1;
    #ifdef _OPENMP
        threads = omp_get_max_threads();
    #endif
    printf("=============== TRACE (SHARKMIDI_TRACE) ===============\n");
    printf("   OpenMP threads:        %d\n", threads);
    printf("   merge path:            %s\n", tr.rank_used ? "rank" : "heap (merge_timing)");
    printf("   parse:                 tiled, %lld KB output per tile\n", (long long)tr.tile_kb);
    printf("   timing/track alloc:    %lfs\n", tr.mainalloc);
    if (tr.rank_used)
    {
        printf("   rank: alloc:           %lfs\n", tr.rank_alloc);
        printf("   rank: prefix+bounds:   %lfs\n", tr.rank_bounds);
        printf("   rank: pass 1:          %lfs\n", tr.rank_pass1);
        printf("   rank: prefix sums:     %lfs\n", tr.rank_scan);
        printf("   rank: pass 2:          %lfs\n", tr.rank_pass2);
        printf("   rank: free:            %lfs\n", tr.rank_free);
    }
    printf("   offsets compaction:    %lfs\n", tr.compact);
    printf("   event array alloc:     %lfs\n", tr.evalloc);
    printf("   free + unmap:          %lfs\n", tr.tail);
    printf("   (calloc'd memory is only touched later, its page-fault cost lands in the phase that first writes it)\n");
    printf("=======================================================\n");
}

static TempoEvent_arr tempos = {0};
static SysExEvent_arr sysexs = {0};

typedef struct
{
    int64_t tick;
    uint32_t track;
} MergeNode;

// ties go to the lower track so the result is deterministic (the old qsort was not stable)
static inline int merge_less(MergeNode a, MergeNode b)
{
    return a.tick < b.tick || (a.tick == b.tick && a.track < b.track);
}

static void merge_sift_down(MergeNode* heap, int64_t n, int64_t i)
{
    MergeNode node = heap[i];
    for (;;)
    {
        int64_t child = 2 * i + 1;
        if (child >= n) 
            break;
        if (child + 1 < n && merge_less(heap[child + 1], heap[child])) 
            child++;
        if (!merge_less(heap[child], node)) 
            break;
        heap[i] = heap[child];
        i = child;
    }
    heap[i] = node;
}

typedef struct
{
    int64_t curtick; // tick of the timing entry being built, -1 before the first
    int64_t out;     // index of that entry
    int64_t offset;  // events emitted so far
    int64_t notes;   // note-ons emitted so far
    int64_t entry;   // groups emitted so far
} MergeState;

// stores one run of the track index (a track is below 65536, a group is within one track chunk, below 4 GB)
static inline void tg_put(int64_t idx, int64_t offset, uint32_t count, uint32_t track)
{
    trackGroups.runs[idx] = count;
    trackGroups.tracks[idx] = (uint16_t)track;
    if ((idx & (TG_ANCHOR - 1)) == 0)
        trackGroups.anchors[idx / TG_ANCHOR] = offset;
}

static void tg_drop(const char* why)
{
    printf("track colors off for this file: %s\n", why);
    free(trackGroups.runs);
    free(trackGroups.tracks);
    free(trackGroups.anchors);
    trackGroups.runs = NULL;
    trackGroups.tracks = NULL;
    trackGroups.anchors = NULL;
    trackGroups.count = 0;
}

static inline void merge_emit(MergeState* m, TrackTick* g, uint32_t track, int fill)
{
    if (g->tick != m->curtick)
    {
        m->curtick = g->tick;
        m->out++;
        if (fill)
            timingArr[m->out] = (TickGroup){.tick = m->curtick, .notecount = m->notes, .event_offset = m->offset};
    }
    if (fill)
    {
        timingArr[m->out].notecount += g->notecount;
        if (trackGroups.runs)
            tg_put(m->entry, m->offset, g->count, track);
        g->offset = m->offset; // the track parser reads its write position from here
    }
    m->notes += g->notecount;
    m->offset += g->count;
    m->entry++;
}

// every track's groups are already sorted by tick, so a k-way merge yields the global order directly,
// no flat copy of all groups and no qsort. fill=0 only counts distinct ticks so timingArr can be
// allocated exactly, fill=1 builds timingArr/trackGroups and stores each group's eventArr offset.
static int64_t merge_timing(TrackTick_arr* hist, int32_t tracks, int fill)
{
    MergeState m = {.curtick = -1, .out = -1};
    MergeNode* heap = malloc((tracks + 1) * sizeof(MergeNode));
    int64_t* pos = calloc(tracks + 1, sizeof(int64_t));
    int64_t n = 0;
    for (int32_t t = 0; t < tracks; t++)
        if (hist[t].count > 0)
            heap[n++] = (MergeNode){hist[t].data[0].tick, (uint32_t)t};
    for (int64_t i = n / 2 - 1; i >= 0; i--)
        merge_sift_down(heap, n, i);

    while (n > 1)
    {
        uint32_t t = heap[0].track;
        TrackTick* g = &hist[t].data[pos[t]];
        merge_emit(&m, g, t, fill);
        if (++pos[t] < hist[t].count)
            heap[0].tick = g[1].tick;
        else
            heap[0] = heap[--n];
        merge_sift_down(heap, n, 0);
    }
    if (n == 1) // one track left (or only ever one): plain walk, no heap
    {
        uint32_t t = heap[0].track;
        for (int64_t j = pos[t]; j < hist[t].count; j++)
            merge_emit(&m, &hist[t].data[j], t, fill);
    }
    free(heap);
    free(pos);
    return m.out + 1;
}

// distinct tick count without a merge pass: a single track's ticks are strictly increasing, otherwise mark
// every group's tick in a bitmap and popcount it. returns -1 when the bitmap would be big next to the
// groups themselves (very sparse, very long files), the caller then counts with the merge instead.
// the bitmap is handed back through bitsOut (NULL if there is none) because the rank merge needs it
static int64_t count_distinct_ticks(TrackTick_arr* hist, int32_t tracks, int64_t total_entries, uint64_t** bitsOut)
{
    *bitsOut = NULL;
    int32_t used = 0, only = 0;
    for (int32_t t = 0; t < tracks; t++)
        if (hist[t].count > 0) { used++; only = t; }
    if (used == 0) return 0;
    if (used == 1) return hist[only].count;

    int64_t words = (maxTick >> 6) + 1;
    if (words * 8 > total_entries * 4) return -1;
    uint64_t* bits = calloc(words, sizeof(uint64_t));
    if (!bits) return -1;
    #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1)
    #endif
    for (int32_t t = 0; t < tracks; t++)
        for (int64_t j = 0; j < hist[t].count; j++)
        {
            int64_t tick = hist[t].data[j].tick;
            uint64_t mask = 1ull << (tick & 63);
            // most ticks are already marked by another track, a plain read keeps the cache line shared
            if (!(__atomic_load_n(&bits[tick >> 6], __ATOMIC_RELAXED) & mask))
                __atomic_fetch_or(&bits[tick >> 6], mask, __ATOMIC_RELAXED);
        }
    int64_t distinct = 0;
    for (int64_t w = 0; w < words; w++)
        distinct += __builtin_popcountll(bits[w]);
    *bitsOut = bits;
    return distinct;
}

// ---- merge by counting sort ---------------------------------------------------------------------------------
// the heap merge is serial, and with hundreds of tracks every group costs log2(tracks) compares plus a cache miss.
// the tick bitmap gives each tick a dense rank in O(1) (popcount of the bits before it), which turns the merge into
// a counting sort: count what lands on each tick, prefix sum, place every group. the ticks are cut into ranges
// and every range belongs to one thread, so there are no atomics, and the result is exactly what the heap merge
// produces: ticks in order, groups of one tick ordered by track
#ifndef MERGE_RANK_MIN_TRACKS
#define MERGE_RANK_MIN_TRACKS 8 // below this the heap is shallow and about as fast (measured: break-even at 4, rank wins from 8)
#endif
#define RANK_CHUNKS 32

static inline int64_t tick_rank(const uint64_t* bits, const uint32_t* wprefix, int64_t tick)
{
    return wprefix[tick >> 6] + __builtin_popcountll(bits[tick >> 6] & ((1ull << (tick & 63)) - 1));
}

// the tick that has the given rank (r < number of distinct ticks)
static int64_t tick_of_rank(const uint64_t* bits, const uint32_t* wprefix, int64_t words, int64_t r)
{
    int64_t lo = 0, hi = words - 1;
    while (lo < hi)
    {
        int64_t mid = lo + (hi - lo + 1) / 2;
        if (wprefix[mid] <= r) lo = mid; else hi = mid - 1;
    }
    uint64_t x = bits[lo];
    for (int64_t k = r - wprefix[lo]; k > 0; k--)
        x &= x - 1; // drop the lowest set bit
    return (lo << 6) + __builtin_ctzll(x);
}

// returns 0 if it could not run (memory, too many ticks for 32 bit ranks), then nothing was modified
// except timingArr/trackGroups contents, which the heap merge overwrites completely
static int merge_by_rank(TrackTick_arr* hist, int32_t tracks, const uint64_t* bits, int64_t distinct)
{
    int64_t words = (maxTick >> 6) + 1;
    if (distinct <= 0 || distinct >= UINT32_MAX) return 0;
    double trm = g_trace ? get_time() : 0.0;
    int chunks = distinct >= 4096 ? RANK_CHUNKS : 1;
    uint32_t* wprefix = malloc((words + 1) * sizeof(uint32_t));
    uint32_t* evcur = calloc(distinct, sizeof(uint32_t));                  // events already placed on each tick
    uint32_t* ecur = trackGroups.runs ? calloc(distinct, sizeof(uint32_t)) : NULL; // next group index on each tick
    int64_t* tickbound = malloc((chunks + 1) * sizeof(int64_t));
    int64_t* bnd = malloc((size_t)tracks * (chunks + 1) * sizeof(int64_t));
    if (!wprefix || !evcur || (trackGroups.runs && !ecur) || !tickbound || !bnd)
    {
        free(wprefix); free(evcur); free(ecur); free(tickbound); free(bnd);
        return 0;
    }
    TR_LAP(rank_alloc, trm);
    uint32_t run = 0;
    for (int64_t w = 0; w < words; w++)
    {
        wprefix[w] = run;
        run += (uint32_t)__builtin_popcountll(bits[w]);
    }

    // ranges holding about the same number of distinct ticks
    tickbound[0] = 0;
    tickbound[chunks] = maxTick + 1;
    for (int c = 1; c < chunks; c++)
        tickbound[c] = tick_of_rank(bits, wprefix, words, distinct * c / chunks);

    // where every track's groups cross those tick bounds
    #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1)
    #endif
    for (int32_t t = 0; t < tracks; t++)
    {
        const TrackTick* d = hist[t].data;
        int64_t n = hist[t].count, lo = 0;
        int64_t* b = &bnd[(size_t)t * (chunks + 1)];
        b[0] = 0;
        for (int c = 1; c < chunks; c++)
        {
            int64_t hi = n;
            while (lo < hi)
            {
                int64_t mid = lo + (hi - lo) / 2;
                if (d[mid].tick < tickbound[c]) lo = mid + 1; else hi = mid;
            }
            b[c] = lo;
        }
        b[chunks] = n;
    }

    TR_LAP(rank_bounds, trm);

    // pass 1: what lands on each tick (events, notes and, for the track index, groups)
    #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1)
    #endif
    for (int c = 0; c < chunks; c++)
        for (int32_t t = 0; t < tracks; t++)
        {
            const TrackTick* d = hist[t].data;
            const int64_t* b = &bnd[(size_t)t * (chunks + 1)];
            for (int64_t j = b[c]; j < b[c + 1]; j++)
            {
                int64_t r = tick_rank(bits, wprefix, d[j].tick);
                timingArr[r].tick = d[j].tick;
                timingArr[r].event_offset += d[j].count; // totals for now, turned into start offsets below
                timingArr[r].notecount += d[j].notecount;
                if (ecur) ecur[r]++;
            }
        }

    TR_LAP(rank_pass1, trm);

    // prefix sums: start offset of each tick in eventArr, notes up to and including it, first group index on it
    int64_t events = 0, notes = 0;
    uint32_t groups = 0;
    for (int64_t r = 0; r < distinct; r++)
    {
        int64_t tickevents = timingArr[r].event_offset;
        notes += timingArr[r].notecount;
        timingArr[r].event_offset = events;
        timingArr[r].notecount = notes;
        events += tickevents;
        if (ecur)
        {
            uint32_t tickgroups = ecur[r];
            ecur[r] = groups;
            groups += tickgroups;
        }
    }

    TR_LAP(rank_scan, trm);

    // pass 2: place every group. tracks are visited in order, so groups of one tick end up ordered by track
    #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1)
    #endif
    for (int c = 0; c < chunks; c++)
        for (int32_t t = 0; t < tracks; t++)
        {
            TrackTick* d = hist[t].data;
            const int64_t* b = &bnd[(size_t)t * (chunks + 1)];
            for (int64_t j = b[c]; j < b[c + 1]; j++)
            {
                int64_t r = tick_rank(bits, wprefix, d[j].tick);
                int64_t offset = timingArr[r].event_offset + evcur[r];
                evcur[r] += d[j].count;
                if (ecur)
                    tg_put(ecur[r]++, offset, d[j].count, t);
                d[j].offset = offset; // the track parser reads its write position from here
            }
        }

    TR_LAP(rank_pass2, trm);
    free(wprefix); free(evcur); free(ecur); free(tickbound); free(bnd);
    TR_LAP(rank_free, trm);
    tr.rank_used = 1;
    return 1;
}

// parsing needs each group's start offset in eventArr, so squeeze the 16 byte groups down in place: a 4 byte
// distance to the next group's start per group (the first start goes to *start). a track whose groups end up
// back to back (nothing from another track lands between them, always true for a single track file) doesn't
// need them at all: only its first offset is kept and the array is released. a gap of 4 GB or more, which takes
// a track that sits out that many bytes of the others' events, gets 8 byte distances instead (*wide)
#ifndef COMPACT_FORCE_WIDE
#define COMPACT_FORCE_WIDE 0 // test hook: 1 makes every track wide, 2 every odd one
#endif
static void compact_offsets(TrackTick_arr* a, int64_t* start, uint8_t* wide, Checkpoint_arr* cps, int32_t idx)
{
    *start = 0;
    *wide = 0;
    if (a->count == 0)
        return;
    TrackTick* src = a->data;
    *start = src[0].offset;
    int contiguous = 1;
    int64_t maxgap = 0;
    // the gaps can't reach 4G when the whole file has fewer events than that, then the scan may stop early
    int needgap = bytecount >= (uint64_t)UINT32_MAX;
    for (int64_t j = 0; j + 1 < a->count; j++)
    {
        int64_t gap = src[j + 1].offset - src[j].offset;
        if (gap != (int64_t)src[j].count)
        {
            contiguous = 0;
            if (!needgap)
                break;
        }
        if (gap > maxgap) maxgap = gap;
    }
    if (contiguous)
    {
        free(a->data);
        a->data = NULL;
        return;
    }
    int usewide = maxgap > (int64_t)UINT32_MAX || COMPACT_FORCE_WIDE == 1 || (COMPACT_FORCE_WIDE == 2 && (idx & 1));
    *wide = (uint8_t)usewide;
    int64_t ncp = cps ? cps->count : 0;
    for (int64_t c = 0; c < ncp; c++)
        cps->data[c].off = INT64_MAX; // a checkpoint at a group that doesn't exist (the end of the track) is never used
    // the table is written over the TrackTicks it is made from, which is safe: entry j is only written after
    // the groups up to j + 1 were read, and an entry (4 or 8 bytes) is smaller than a TrackTick
    int64_t cur = src[0].offset;
    if (usewide)
    {
        int64_t* dst = (int64_t*)a->data;
        for (int64_t j = 0; j < a->count; j++)
        {
            int64_t nxt = j + 1 < a->count ? src[j + 1].offset : cur; // read before dst[j] can overwrite it
            if (j > 0 && j % TILE_CP_STEP == 0 && j / TILE_CP_STEP <= ncp)
                cps->data[j / TILE_CP_STEP - 1].off = cur;
            dst[j] = nxt - cur;
            cur = nxt;
        }
        void* p = realloc(a->data, (a->count + 2) * sizeof(int64_t)); // +2: spare entries the parse reads ahead
        if (p) a->data = p;
        dst = (int64_t*)a->data;
        dst[a->count - 1] = dst[a->count] = dst[a->count + 1] = 0;
    }
    else
    {
        uint32_t* dst = (uint32_t*)a->data;
        for (int64_t j = 0; j < a->count; j++)
        {
            int64_t nxt = j + 1 < a->count ? src[j + 1].offset : cur;
            if (j > 0 && j % TILE_CP_STEP == 0 && j / TILE_CP_STEP <= ncp)
                cps->data[j / TILE_CP_STEP - 1].off = cur;
            dst[j] = (uint32_t)(nxt - cur);
            cur = nxt;
        }
        void* p = realloc(a->data, (a->count + 2) * sizeof(uint32_t));
        if (p) a->data = p;
        dst = (uint32_t*)a->data;
        dst[a->count - 1] = dst[a->count] = dst[a->count + 1] = 0;
    }
}

int LoadMIDI(const char* filepath)
{
    UnloadMIDI();
    trackAmount = 0, totalnotes = 0, eventcount = 0, bytecount = 0, playednotes = 0, maxTick = 0;
    g_trace = env_flag("SHARKMIDI_TRACE");
    g_tile_set = 0;
    if (getenv("SHARKMIDI_TILE_KB") && atoi(getenv("SHARKMIDI_TILE_KB")) > 0)
    {
        g_tile_kb = atoi(getenv("SHARKMIDI_TILE_KB"));
        g_tile_set = 1;
    }
    tr = (TraceTimes){0};
    updatestatsandprint("initiating memory mapped file\n");
    
    if (!InitMMF(filepath)) 
        return midiloaded;
        
    VerifyHeader();
    trackProperties_arr tracks = {0};
    while(filePos < fileLen)
    {
        if (!IndexTrack(&tracks))
            break;
        updatestatsandprint("\rfound %d tracks", trackAmount);
    }
    updatestatsandprint("\ncounting events\n");

    TrackTick_arr* histogram = calloc(trackAmount, sizeof(TrackTick_arr));
    Checkpoint_arr* cps = calloc(trackAmount + 1, sizeof(Checkpoint_arr));
    double start = get_time();
    int32_t loadedtracks = 0;
    
    #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1)
    #endif
    for(int32_t i = 0; i < trackAmount; i++)
    {
        trackProperties* currtrack = &tracks.data[i];
        uint8_t* trackstart = filePtr + currtrack->start;
        int64_t trackbytes = 0;
        int64_t trackev = CountTrackEvents(trackstart, trackstart + currtrack->length, &histogram[i], &cps[i], TILE_CP_STEP, &trackbytes); // checkpoint positions are 32 bit, like a track chunk length
        Checkpoint_arr_shrink(&cps[i]);
        ATOMIC_ADD(eventcount, trackev);
        ATOMIC_ADD(bytecount, trackbytes);
        TrackTick_arr_shrink(&histogram[i]); // drop the doubling slack, only tracks still being counted keep any
        ATOMIC_ADD(loadedtracks, 1);
        printf("(%s/%s) tracks scanned, %s notes counted\r", AddCommas(loadedtracks), AddCommas(trackAmount), AddCommas(totalnotes));
        fflush(stdout);
    }
    double end = get_time();
    counttime = end - start;

    updatestatsandprint("\ncounting active ticks\n");
    int64_t total_entries = 0;
    for (int32_t i = 0; i < trackAmount; i++)
        total_entries += histogram[i].count;
    
    uint64_t* tickbits = NULL;
    double tickcountstart = get_time();
    activetickcount = count_distinct_ticks(histogram, trackAmount, total_entries, &tickbits);
    if (activetickcount < 0)
        activetickcount = merge_timing(histogram, trackAmount, 0);
    tickcounttime = get_time() - tickcountstart;
    
    int32_t usedtracks = 0;
    for (int32_t i = 0; i < trackAmount; i++)
        usedtracks += histogram[i].count > 0;

    updatestatsandprint("merging per track timing into main timing array and track RLE\n");
    double trmark = g_trace ? get_time() : 0.0;
    timingArr = calloc(activetickcount + 1, sizeof(TickGroup));
    if (trackcolors)
    {
        trackGroups.runs = malloc((size_t)total_entries * sizeof(uint32_t));
        trackGroups.tracks = malloc((size_t)total_entries * sizeof(uint16_t));
        trackGroups.anchors = malloc((size_t)((total_entries + TG_ANCHOR - 1) / TG_ANCHOR + 1) * sizeof(int64_t));
        trackGroups.count = total_entries;
        if (!trackGroups.runs || !trackGroups.tracks || !trackGroups.anchors)
            tg_drop("out of memory for the track index");
    }
    TR_LAP(mainalloc, trmark);
    int merged = 0;
    #ifndef MERGE_RANK_FORCE
        #define MERGE_RANK_FORCE 0
    #endif
    // the rank merge needs 4 (8 with track colors) bytes per distinct tick on top, only worth it when ticks are shared
    double mergestart = get_time();
    if (tickbits && usedtracks >= MERGE_RANK_MIN_TRACKS && (MERGE_RANK_FORCE || activetickcount * 8 <= total_entries * 4))
        merged = merge_by_rank(histogram, trackAmount, tickbits, activetickcount);
    if (!merged)
        merge_timing(histogram, trackAmount, 1);
    free(tickbits);
    mergetime = get_time() - mergestart;
    trmark = g_trace ? get_time() : 0.0;

    int64_t* startOffsets = malloc((trackAmount + 1) * sizeof(int64_t));
    uint8_t* wide = calloc(trackAmount + 1, 1); // tracks that keep 8 byte offsets
    #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1)
    #endif
    for (int32_t i = 0; i < trackAmount; i++)
        compact_offsets(&histogram[i], &startOffsets[i], &wide[i], &cps[i], i);
    TR_LAP(compact, trmark);

    updatestatsandprint("creating main event array\n");
    eventArr = calloc(bytecount + 8, 1); // 8 spare bytes: nothing reads them, they keep a decoder's lookahead inside the allocation
    TR_LAP(evalloc, trmark);

    loadedtracks = 0;
    updatestatsandprint("actually parsing events this time\n");
    start = get_time();
    
    #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1)
    #endif
    for(int32_t i = 0; i < trackAmount; i++)
    {
        if (track_is_tiled(&histogram[i]))
            continue; // done by ParseTiled below. what is left (a track without channel events, but with tempo or
                      // sysex events, or one with a gap of 4 GB or more between groups) is parsed here, track by track
        trackProperties* currtrack = &tracks.data[i];
        uint8_t* trackstart = filePtr + currtrack->start;
        Cursor cur = {trackstart, 0, startOffsets[i], 0, 0};
        ATOMIC_ADD(playednotes, ParseRange(&cur, trackstart + currtrack->length, eventArr, NULL, INT64_MAX, &tempos, &sysexs, 1, 0));
        TrackTick_arr_free(&histogram[i]); // memory comes back progressively, and in parallel
        ATOMIC_ADD(loadedtracks, 1);
        printf("(%s/%s) tracks parsed, %s notes parsed\r", AddCommas(loadedtracks), AddCommas(trackAmount), AddCommas(playednotes));
        fflush(stdout);
    }
    ParseTiled(&tracks, histogram, wide, startOffsets, cps, TILE_CP_STEP, trackAmount, (int64_t)bytecount, &tempos, &sysexs);
    for (int32_t i = 0; i < trackAmount; i++)
    {
        TrackTick_arr_free(&histogram[i]);
        Checkpoint_arr_free(&cps[i]);
    }
    free(cps);
    end = get_time();    

    trmark = g_trace ? get_time() : 0.0;
    free(histogram);
    free(startOffsets);
    free(wide);
    munmap(filePtr, fileLen);
    updatestatsandprint("\nfinalizing events and sorting metadata\n");
    timingArr[activetickcount] = (TickGroup){INT64_MAX, (int64_t)totalnotes, (int64_t)bytecount};
    TempoEvent tev = {INT64_MAX, 500000};
    TempoEvent_arr_push(&tempos, tev);
    SysExEvent sex = {INT64_MAX};
    SysExEvent_arr_push(&sysexs, sex);

    parsetime = end - start;
    qsort(tempos.data, tempos.count, sizeof(TempoEvent), cmp_tempo);
    qsort(sysexs.data, sysexs.count, sizeof(SysExEvent), cmp_sysex);
    tempoArr = tempos.data;
    sysexArr = sysexs.data;
    tempoCount = tempos.count;
    sysexCount = sysexs.count;
    trackProperties_arr_free(&tracks);
    filePtr = NULL;
    midiloaded = 1;
    filename = getfilename(filepath);
    TR_LAP(tail, trmark);
    printparsestatistics();
    if (g_trace)
        printtrace();
    return midiloaded;
}

void UnloadMIDI(void)
{
    if (!midiloaded) return;
    midiloaded = 0;
    Playback_StopAndWait(); // the playback thread reads eventArr/timingArr, don't free them under it
    trackAmount = 0;
    totalnotes = 0;
    eventcount = 0;
    bytecount = 0;
    activetickcount = 0;
    filename = "no midi loaded";
    filename_len = 0;
    
    for (uint32_t i = 0; i < sysexCount; i++)
        free(sysexArr[i].message);
    
    TempoEvent_arr_free(&tempos);
    SysExEvent_arr_free(&sysexs);
    free(eventArr);
    free(timingArr);
    // trackcolors can be toggled between load and unload, so key off the pointer
    free(trackGroups.runs);
    free(trackGroups.tracks);
    free(trackGroups.anchors);
    trackGroups.runs = NULL;
    trackGroups.tracks = NULL;
    trackGroups.anchors = NULL;
    trackGroups.count = 0;
    sysexArr = NULL;
    tempoArr = NULL;
    eventArr = NULL;
    timingArr = NULL;
    puts("successfully freed midi");
}