#include "parser.h"
#include "midistorage.h"
#include "../misc/typed-array.h"
#include "../playback/timer.h"
#include "../playback/playback_thread.h"
#include "../third_party/conmidi_digit_formatter.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdatomic.h>
#include <stdarg.h>

#ifdef _OPENMP
    #include <omp.h>
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

uint8_t* filePtr = NULL;
uint64_t fileLen = 0;
uint64_t filePos = 0;
uint32_t headersize = 0; 
uint16_t trackAmount = 0;
uint32_t fmt = 0;
int32_t midiloaded = 0, trackcolors = 1;
uint64_t eventcount = 0;

DEFINE_ARRAY(trackProperties);
DEFINE_ARRAY(TrackTick);
DEFINE_ARRAY(TempoEvent);
DEFINE_ARRAY(SysExEvent);

const char* getfilename(const char* filedir) {
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

static char status_buf[512];
static char static_filename_buf[512];
void updatestatsandprint(const char* fmt, ...)
{
    va_list args;
    char temp[512];

    va_start(args, fmt);
    vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);

    fputs(temp, stdout);
    fflush(stdout);

    char *src = temp, *dst = status_buf;
    while (*src)
    {
        if (*src != '\n' && *src != '\r')
            *dst++ = *src;
        src++;
    }
    *dst = '\0';

    snprintf(static_filename_buf, sizeof(static_filename_buf), "%s", status_buf);
    filename = static_filename_buf;
}

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
int munmap(void* addr, size_t length) 
{
    return UnmapViewOfFile(addr) ? 0 : -1;
}
int InitMMF(const char* filepath)
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
int InitMMF(const char* filepath)
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
    filePtr = mmap(NULL, filestat.st_size, PROT_READ, MAP_PRIVATE, filedesc, 0);
    close(filedesc);
    if (filePtr == MAP_FAILED)
    {
        filePtr = NULL;
        updatestatsandprint("mmap failed\n");
        return 0;
    }
    printf("successfully initiated memory mapped file with %llu bytes in size\n", (unsigned long long)fileLen);
    return 1;
}
#endif

int64_t __attribute__((noinline)) varlen_decode_slow(int64_t* len, uint8_t** trackPtr)
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
        headersize = ReadUInt32();
        fmt = ReadUInt16();
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

int IndexTrack(trackProperties_arr* tracklistptr)
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

#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// cold path of the group array growth (same policy as TrackTick_arr_push)
__attribute__((noinline, cold))
static void tg_grow(TrackTick_arr* a)
{
    a->cap = a->cap < 1024 ? 1024 : a->cap * 2;
    a->data = realloc(a->data, a->cap * sizeof(TrackTick));
}

// HOW THE FAST PATH WORKS
// the common case in a big MIDI is a channel event with a 1 or 2 byte delta, with or without a status byte.
// CountTrackEvents and ParseTrackEvents each run a tight loop for exactly that and nothing else. before it
// changes any state it peeks at the next event; if that is anything unusual (3+ byte delta, meta, sysex,
// less than 8 bytes left in the track) it leaves the loop and the original, fully general code handles that
// one event, then the fast loop resumes. so rare cases keep their old behaviour, and the fast loop never has
// to know about them. the two fast loops must stay in lockstep (same events, same group boundaries)

// peeks at the next event and says whether it is a plain channel event with a 1 or 2 byte delta,
// which is what the fast loops handle. nothing is consumed or modified
static inline __attribute__((always_inline))
int peek_channel_event(const uint8_t* p, uint32_t* delta, const uint8_t** q, uint32_t* ev)
{
    uint32_t b0 = p[0], b1 = p[1];
    if (b0 < 0x80) { *delta = b0; *q = p + 1; }
    else if (b1 < 0x80) { *delta = ((b0 & 0x7F) << 7) | b1; *q = p + 2; }
    else return 0;
    *ev = **q;
    return *ev < 0xF0;
}

int64_t CountTrackEvents(uint8_t* trackPtr, uint8_t* trackEnd, TrackTick_arr* tickgroup)
{
    int64_t totalnotes_local = 0;
    int64_t eventcount_local = 0;
    int64_t absolutetime = 0;
    uint8_t prevEvent = 0;
    uint32_t count = 0;
    uint32_t notecount = 0;
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
            data[n] = (TrackTick){.tick = absolutetime, .count = count, .notecount = notecount};
            uint32_t flush = (delta != 0) & (count != 0);
            uint32_t keep = flush - 1;
            n += flush;
            eventcount_local += count & ~keep;
            totalnotes_local += notecount & ~keep;
            count &= keep;
            notecount &= keep;
            absolutetime += delta;

            uint32_t st = ev;
            if (ev >= 0x80) { prevEvent = ev; q++; } // new status byte, otherwise running status
            else st = prevEvent;
            count++;
            // program change / channel pressure have one data byte. the length stays a predicted branch so the
            // next event's address doesn't wait on this event's bytes
            if (UNLIKELY((st & 0xE0) == 0xC0)) { trackPtr = (uint8_t*)q + 1; continue; }
            notecount += ((st & 0xF0) == 0x90) & (q[1] != 0);
            trackPtr = (uint8_t*)q + 2;
        }
        if (trackPtr >= trackEnd) break;

        // generic path: one event, anything goes (tail of the track, meta, sysex, long deltas)
        int64_t delta = *trackPtr++;
        if (delta >= 0x80)
            delta = varlen_decode_slow(&delta, &trackPtr);
        if(delta > 0)
        {
            if (count > 0)
            {
                TG_ENSURE();
                data[n++] = (TrackTick){.tick = absolutetime, .count = count, .notecount = notecount};
                eventcount_local += count;
                totalnotes_local += notecount;
                notecount = 0;
                count = 0;
            }
            absolutetime += delta;
        }
        
        uint8_t readEvent = *trackPtr++;
        if (readEvent >= 0x80)
        {
            if (readEvent < 0xF0)
            {
                prevEvent = readEvent;
                if ((readEvent & 0xF0) == 0x90 && *(trackPtr + 1) != 0)
                    notecount++;
                count++;
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
            if ((prevEvent & 0xF0) == 0x90 && *trackPtr != 0)
                notecount++;
            count++;
            trackPtr += ((prevEvent & 0xE0) == 0xC0) ? 0 : 1;
        }
    }
    finalize:
        #ifdef _OPENMP
            #pragma omp critical
        #endif
        if (absolutetime > maxTick)
            maxTick = absolutetime;
        if (count > 0)
        {
            TG_ENSURE();
            data[n++] = (TrackTick){.tick = absolutetime, .count = count, .notecount = notecount};
            eventcount_local += count;
            totalnotes_local += notecount;
        }
        tickgroup->count = n;
        ATOMIC_ADD(totalnotes, totalnotes_local);
        return eventcount_local;
    #undef TG_ENSURE
}

// decodes one channel event (q points at its status or, with running status, its first data byte),
// stores it and returns the pointer past it. only the status byte check and the rare one-data-byte case are
// branches, the note-on / vel-0 handling is plain arithmetic
static inline __attribute__((always_inline))
const uint8_t* decode_store(const uint8_t* q, uint32_t ev, uint8_t* prevEvent, uint24_t* dst, int64_t* notecount)
{
    uint32_t st = ev;
    if (ev >= 0x80) { *prevEvent = (uint8_t)ev; q++; }
    else st = *prevEvent;
    uint32_t d1 = q[0];
    // the event length stays a predicted branch (not arithmetic on loaded data) so the next event's address
    // doesn't wait on this event's bytes. program change / channel pressure have a single data byte
    if (UNLIKELY((st & 0xE0) == 0xC0))
    {
        uint24_set(dst, (int32_t)(st | (d1 << 8)));
        return q + 1;
    }
    uint32_t d2 = q[1];
    uint32_t ison = (st & 0xF0) == 0x90;
    uint32_t vel0 = ison & (d2 == 0);         // note on with velocity 0 is stored as note off with velocity 64
    *notecount += ison & (d2 != 0);
    uint32_t d2out = vel0 ? 64u : d2;
    uint24_set(dst, (int32_t)((st ^ (vel0 << 4)) | (d1 << 8) | (d2out << 16)));
    return q + 2;
}

// groupOffsets: start offset in msgPtr of each tick group of this track. NULL means the track's groups are
// back to back in msgPtr (no other track interleaves), then they start at startOffset and nothing needs looking up
int64_t ParseTrackEvents(uint8_t* trackPtr, uint8_t* trackEnd, uint24_t* msgPtr, const int64_t* groupOffsets, int64_t groupCount, int64_t startOffset, TempoEvent_arr* tempo, SysExEvent_arr* sysex, uint8_t track)
{
    int64_t absolutetime = 0;
    int64_t notecount = 0;
    uint8_t prevEvent = 0;

    uint32_t group_idx = 0;
    int64_t write_pos = startOffset;
    uint32_t events_in_group = 0;

    int64_t next_off = 0; // start of the following group, preloaded so a group switch doesn't wait on a load
    if (groupOffsets && groupCount > 0)
    {
        write_pos = groupOffsets[0];
        next_off = groupOffsets[1];
    }

    while (trackPtr < trackEnd)
    {
        // fast loops: plain channel events only, 8 bytes of margin cover everything they peek at. unusual events
        // break out before any state changes, so the generic code below sees them untouched
        if (!groupOffsets)
        {
            while (trackEnd - trackPtr >= 8)
            {
                uint32_t delta, ev;
                const uint8_t* q;
                if (UNLIKELY(!peek_channel_event(trackPtr, &delta, &q, &ev))) break;
                absolutetime += delta;
                trackPtr = (uint8_t*)decode_store(q, ev, &prevEvent, &msgPtr[write_pos++], &notecount);
            }
        }
        else
        {
            while (trackEnd - trackPtr >= 8)
            {
                uint32_t delta, ev;
                const uint8_t* q;
                if (UNLIKELY(!peek_channel_event(trackPtr, &delta, &q, &ev))) break;
                // time advanced and the group has events: continue at the next group's start
                uint32_t newgroup = (delta != 0) & (events_in_group != 0);
                if (newgroup)
                {
                    group_idx++;
                    write_pos = next_off;
                    next_off = groupOffsets[group_idx + 1]; // two spare entries, so this is readable up to the end
                }
                events_in_group = 1;
                absolutetime += delta;
                trackPtr = (uint8_t*)decode_store(q, ev, &prevEvent, &msgPtr[write_pos++], &notecount);
            }
        }
        if (trackPtr >= trackEnd) break;

        // generic path: one event, anything goes
        int64_t delta = *trackPtr++;
        if (delta >= 0x80)
            delta = varlen_decode_slow(&delta, &trackPtr);

        if (groupOffsets && delta > 0 && events_in_group > 0)
        {
            group_idx++;
            if (group_idx < groupCount)
                write_pos = groupOffsets[group_idx];
            next_off = groupOffsets[group_idx + 1];
            events_in_group = 0;
        }

        absolutetime += delta;
        uint8_t readEvent = *trackPtr++;
        if (readEvent >= 0x80)
        {
            if (readEvent < 0xF0)
            {
                prevEvent = readEvent;
                long pos = write_pos++;
                events_in_group++;

                if ((readEvent & 0xE0) != 0xC0)
                {
                    uint8_t data1 = *trackPtr++;
                    uint8_t data2 = *trackPtr++;
                    if ((readEvent & 0xF0) == 0x90)
                    {
                        if (data2 != 0)
                            notecount++;
                        else 
                        {
                            msgPtr[pos] = uint24_from(0x80 | (readEvent & 0x0F) | (data1 << 8) | (64 << 16));
                            continue;
                        }
                    }
                    msgPtr[pos] = uint24_from(readEvent | (data1 << 8) | (data2 << 16));
                }
                else
                {
                    uint8_t data1 = *trackPtr++;
                    msgPtr[pos] = uint24_from(readEvent | (data1 << 8));
                }
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
                            
                            int32_t tempoVal = 0;
                            for (int i = 0; i < len; i++) 
                                tempoVal = (tempoVal << 8) | *trackPtr++;
                            TempoEvent tev = {absolutetime, tempoVal};
                            #ifdef _OPENMP
                                #pragma omp critical
                            #endif
                            TempoEvent_arr_push(tempo, tev);
                        }
                        else if (readEvent == 0x2F)
                        {
                            return notecount;
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
            long pos = write_pos++;
            events_in_group++;

            if ((prevEvent & 0xE0) != 0xC0)
            {
                uint8_t data2 = *trackPtr++;
                if ((prevEvent & 0xF0) == 0x90)
                {
                    if (data2 != 0)
                        notecount++;
                    else 
                    {
                        msgPtr[pos] = uint24_from(0x80 | (prevEvent & 0x0F) | (readEvent << 8) | (64 << 16));
                        continue;
                    }
                }
                msgPtr[pos] = uint24_from(prevEvent | (readEvent << 8) | (data2 << 16));
            }
            else
            {
                msgPtr[pos] = uint24_from(prevEvent | (readEvent << 8));
            }
        }
    }
    return notecount;
}

double counttime = 0.0;
double mergetime = 0.0;
double loadtime = 0.0;
double parsetime = 0.0;
// ---- big array allocation ------------------------------------------------------------------------------------
// eventArr, timingArr and the track index are hundreds of MB up to several GB of brand new memory. the kernel has
// to zero every 4K page and take a page fault on first touch, which is a large share of the parse (and merge) time
// on big files. the environment variable SHARKMIDI_PAGES (Linux only, ignored elsewhere) chooses what to do:
//   unset      plain calloc
//   huge       2MB aligned + MADV_HUGEPAGE: one fault per 2MB instead of 512 (needs transparent hugepages set to
//              "madvise" or "always" in /sys/kernel/mm/transparent_hugepage/enabled)
//   prefault   fault the whole array in up front, in parallel, so the parse threads don't fault page by page
//   both       both of the above
#if defined(__linux__)
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23 // Linux 5.14+, older kernels fall back to touching the pages
#endif
#define BIG_HUGE 1
#define BIG_PREFAULT 2
#define BIG_MIN_BYTES (4u << 20)
#define BIG_HUGE_ALIGN ((size_t)2 << 20)
#define BIG_MAX_BLOCKS 16
static struct { void* base; size_t size; } bigblocks[BIG_MAX_BLOCKS];
static int bigmode = -1;

static void big_mode_init(void)
{
    const char* e = getenv("SHARKMIDI_PAGES");
    bigmode = 0;
    if (e)
    {
        if (!strcmp(e, "huge")) bigmode = BIG_HUGE;
        else if (!strcmp(e, "prefault")) bigmode = BIG_PREFAULT;
        else if (!strcmp(e, "both")) bigmode = BIG_HUGE | BIG_PREFAULT;
        else if (strcmp(e, "default") && *e) fprintf(stderr, "SHARKMIDI_PAGES: unknown value '%s' (huge, prefault, both)\n", e);
    }
}

static void big_prefault(char* p, size_t size)
{
    const size_t chunk = (size_t)8 << 20;
    int64_t chunks = (int64_t)((size + chunk - 1) / chunk);
    #ifdef _OPENMP
        #pragma omp parallel for schedule(static)
    #endif
    for (int64_t c = 0; c < chunks; c++)
    {
        char* a = p + (size_t)c * chunk;
        size_t len = (size_t)c * chunk + chunk <= size ? chunk : size - (size_t)c * chunk;
        if (madvise(a, len, MADV_POPULATE_WRITE) != 0)
            for (size_t o = 0; o < len; o += 4096)
                *(volatile char*)(a + o) = 0; // writing zero keeps the page zero
    }
}

// zeroed memory like calloc(1, bytes)
static void* big_alloc(size_t bytes)
{
    if (bigmode < 0) big_mode_init();
    if (!bigmode || bytes < BIG_MIN_BYTES) return calloc(1, bytes);
    size_t size = (bytes + BIG_HUGE_ALIGN - 1) & ~(BIG_HUGE_ALIGN - 1);
    int slot = -1;
    for (int i = 0; i < BIG_MAX_BLOCKS && slot < 0; i++)
        if (!bigblocks[i].base) slot = i;
    if (slot < 0) return calloc(1, bytes);
    char* raw = mmap(NULL, size + BIG_HUGE_ALIGN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return calloc(1, bytes);
    char* p = (char*)(((uintptr_t)raw + BIG_HUGE_ALIGN - 1) & ~(uintptr_t)(BIG_HUGE_ALIGN - 1));
    if (p > raw) munmap(raw, (size_t)(p - raw));                                  // give back the alignment slack
    size_t tail = (size_t)((raw + size + BIG_HUGE_ALIGN) - (p + size));
    if (tail) munmap(p + size, tail);
    if (bigmode & BIG_HUGE) madvise(p, size, MADV_HUGEPAGE);
    if (bigmode & BIG_PREFAULT) big_prefault(p, size);
    bigblocks[slot].base = p;
    bigblocks[slot].size = size;
    return p;
}

static void big_free(void* p)
{
    if (!p) return;
    for (int i = 0; i < BIG_MAX_BLOCKS; i++)
        if (bigblocks[i].base == p)
        {
            munmap(p, bigblocks[i].size);
            bigblocks[i].base = NULL;
            return;
        }
    free(p);
}

// for the statistics: which mode is active and how much of the process really is on huge pages
static const char* big_mode_report(void)
{
    static char text[96];
    long kb = -1;
    FILE* f = fopen("/proc/self/smaps_rollup", "r");
    char line[128];
    while (f && fgets(line, sizeof(line), f))
        if (sscanf(line, "AnonHugePages: %ld kB", &kb) == 1) break;
    if (f) fclose(f);
    // also in default mode: with transparent hugepages set to "always" the kernel already uses them for these arrays
    snprintf(text, sizeof(text), "%s%s%s, %s MB on huge pages", bigmode ? "" : "default (calloc)", (bigmode & BIG_HUGE) ? "huge" : "",
        (bigmode & BIG_PREFAULT) ? (bigmode & BIG_HUGE ? "+prefault" : "prefault") : "", kb < 0 ? "?" : AddCommas(kb / 1024));
    return text;
}
#else
#define big_alloc(bytes) calloc(1, (bytes))
#define big_free(p) free(p)
static const char* big_mode_report(void) { return "default (calloc)"; }
#endif

void printparsestatistics(void)
{
    char parsestatistics[] =
    "=============== PARSE STATISTICS ===============\n"
    "   MIDI Name:              %s\n"
    "   Filesize:               %s Bytes\n"
    "   Took:\n"
    "       Count:              %lfs (%s notes/s)\n"
    "       Merge:              %lfs\n"
    "       Parse:              %lfs (%s notes/s)\n"
    "       Total:              %lfs\n"
    "   Properties:\n"
    "       PPQ / Ticks:        %s / %s (%s has events)\n"
    "       Tracks:             %s\n"
    "       Channel Events:     %s\n"
    "       Note ON Events:     %s\n"
    "       Tempo Events:       %s\n"
    "       SysEx Events:       %s\n"
    "   Memory Usage:\n"
    "       Timing:             %s Bytes (24 bytes/entry)\n"
    "       Events:             %s Bytes (3 bytes/event)\n"
    "       Track Index (RLE):  %s Bytes (%s groups)\n"
    "       Total:              %s Bytes\n"
    "       Page mode:          %s\n"
    "===============================================\n";

    uint64_t trackRLEBytes = (trackcolors? trackGroups.count : 0) * sizeof(TrackGroup);
    printf(parsestatistics, filename, 
        AddCommas(fileLen), counttime, AddCommas((totalnotes / counttime)), mergetime, parsetime, 
        AddCommas((totalnotes / parsetime)), loadtime, AddCommas(ppq), AddCommas(maxTick), AddCommas(activetickcount), AddCommas(trackAmount),
        AddCommas(eventcount), AddCommas(totalnotes), AddCommas(tempoCount - 1), AddCommas(sysexCount - 1),
        AddCommas(activetickcount * sizeof(TickGroup)), AddCommas(eventcount * sizeof(uint24_t)),
        AddCommas(trackRLEBytes), AddCommas(trackGroups.count),
        AddCommas((activetickcount * sizeof(TickGroup)) + (eventcount * sizeof(uint24_t)) + trackRLEBytes),
        big_mode_report());
}

TempoEvent_arr tempos = {0};
SysExEvent_arr sysexs = {0};

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
        if (child >= n) break;
        if (child + 1 < n && merge_less(heap[child + 1], heap[child])) child++;
        if (!merge_less(heap[child], node)) break;
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
        if (trackcolors)
            trackGroups.data[m->entry] = (TrackGroup){.event_offset = m->offset, .count = g->count, .track = (uint8_t)(track << 4)};
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
#define RANK_CHUNKS 64

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
    int chunks = distinct >= 4096 ? RANK_CHUNKS : 1;
    uint32_t* wprefix = malloc((words + 1) * sizeof(uint32_t));
    uint32_t* evcur = calloc(distinct, sizeof(uint32_t));                  // events already placed on each tick
    uint32_t* ecur = trackcolors ? calloc(distinct, sizeof(uint32_t)) : NULL; // next group index on each tick
    int64_t* tickbound = malloc((chunks + 1) * sizeof(int64_t));
    int64_t* bnd = malloc((size_t)tracks * (chunks + 1) * sizeof(int64_t));
    if (!wprefix || !evcur || (trackcolors && !ecur) || !tickbound || !bnd)
    {
        free(wprefix); free(evcur); free(ecur); free(tickbound); free(bnd);
        return 0;
    }
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
                    trackGroups.data[ecur[r]++] = (TrackGroup){.event_offset = offset, .count = d[j].count, .track = (uint8_t)(t << 4)};
                d[j].offset = offset; // the track parser reads its write position from here
            }
        }

    free(wprefix); free(evcur); free(ecur); free(tickbound); free(bnd);
    return 1;
}

// parsing needs each group's start offset in eventArr, so squeeze the 16 byte groups down to 8 byte offsets in
// place. a track whose groups end up back to back (nothing from another track lands between them, always true
// for a single track file) doesn't need them at all: only its first offset is kept and the array is released
static void compact_offsets(TrackTick_arr* a, int64_t* start)
{
    *start = 0;
    if (a->count == 0)
        return;
    TrackTick* src = a->data;
    *start = src[0].offset;
    int contiguous = 1;
    for (int64_t j = 0; j + 1 < a->count; j++)
        if (src[j + 1].offset != src[j].offset + src[j].count)
        {
            contiguous = 0;
            break;
        }
    if (contiguous)
    {
        free(a->data);
        a->data = NULL;
        return;
    }
    int64_t* dst = (int64_t*)a->data;
    for (int64_t j = 0; j < a->count; j++)
        dst[j] = src[j].offset; // forward order, dst[j] only overwrites groups that were already read
    void* p = realloc(a->data, (a->count + 2) * sizeof(int64_t)); // +2: spare entries the parse reads ahead
    if (p) a->data = p;
    ((int64_t*)a->data)[a->count] = ((int64_t*)a->data)[a->count + 1] = ((int64_t*)a->data)[a->count - 1];
}

int LoadMIDI(const char* filepath)
{
    double loadstart = get_time();
    UnloadMIDI();
    trackAmount = 0, totalnotes = 0, eventcount = 0, playednotes = 0, maxTick = 0;
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
    double start = get_time();
    int32_t loadedtracks = 0;
    
    #ifdef _OPENMP
        #pragma omp parallel for
    #endif
    for(int32_t i = 0; i < trackAmount; i++)
    {
        trackProperties* currtrack = &tracks.data[i];
        uint8_t* trackstart = filePtr + currtrack->start;
        ATOMIC_ADD(eventcount, CountTrackEvents(trackstart, trackstart + currtrack->length, &histogram[i]));
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
    double mergestart = get_time();
    uint64_t* tickbits = NULL;
    activetickcount = count_distinct_ticks(histogram, trackAmount, total_entries, &tickbits);
    if (activetickcount < 0)
        activetickcount = merge_timing(histogram, trackAmount, 0);
    int32_t usedtracks = 0;
    for (int32_t i = 0; i < trackAmount; i++)
        usedtracks += histogram[i].count > 0;

    updatestatsandprint("merging per track timing into main timing array and track RLE\n");
    timingArr = big_alloc((activetickcount + 1) * sizeof(TickGroup));
    if (trackcolors)
    {
        trackGroups.data = big_alloc(total_entries * sizeof(TrackGroup));
        trackGroups.count = total_entries;
    }
    int merged = 0;
    #ifndef MERGE_RANK_FORCE
        #define MERGE_RANK_FORCE 0
    #endif
    // the rank merge needs 4 (8 with track colors) bytes per distinct tick on top, only worth it when ticks are shared
    if (tickbits && usedtracks >= MERGE_RANK_MIN_TRACKS && (MERGE_RANK_FORCE || activetickcount * 8 <= total_entries * 4))
        merged = merge_by_rank(histogram, trackAmount, tickbits, activetickcount);
    if (!merged)
        merge_timing(histogram, trackAmount, 1);
    free(tickbits);

    int64_t* startOffsets = malloc((trackAmount + 1) * sizeof(int64_t));
    #ifdef _OPENMP
        #pragma omp parallel for
    #endif
    for (int32_t i = 0; i < trackAmount; i++)
        compact_offsets(&histogram[i], &startOffsets[i]);

    mergetime = get_time() - mergestart;
    updatestatsandprint("creating main event array\n");
    eventArr = big_alloc((eventcount + 1) * sizeof(uint24_t));

    loadedtracks = 0;
    updatestatsandprint("actually parsing events this time\n");
    start = get_time();
    
    #ifdef _OPENMP
        #pragma omp parallel for
    #endif
    for(int32_t i = 0; i < trackAmount; i++)
    {
        trackProperties* currtrack = &tracks.data[i];
        uint8_t* trackstart = filePtr + currtrack->start;
        ATOMIC_ADD(playednotes, ParseTrackEvents(trackstart, trackstart + currtrack->length, eventArr, (const int64_t*)histogram[i].data, histogram[i].count, startOffsets[i], &tempos, &sysexs, (i << 4)));
        TrackTick_arr_free(&histogram[i]); // memory comes back progressively, and in parallel
        ATOMIC_ADD(loadedtracks, 1);
        printf("(%s/%s) tracks parsed, %s notes parsed\r", AddCommas(loadedtracks), AddCommas(trackAmount), AddCommas(playednotes));
        fflush(stdout);
    }
    end = get_time();    

    free(histogram);
    free(startOffsets);

    updatestatsandprint("\nfinalizing events and sorting metadata\n");
    timingArr[activetickcount] = (TickGroup){INT64_MAX, (int32_t)totalnotes, (int64_t)eventcount};
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
    munmap(filePtr, fileLen);
    filePtr = NULL;
    midiloaded = 1;
    filename = getfilename(filepath);
    loadtime = get_time() - loadstart;
    printparsestatistics();
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
    activetickcount = 0;
    filename = "no midi loaded";
    filename_len = 0;
    
    for (uint32_t i = 0; i < sysexCount; i++)
        free(sysexArr[i].message);
    
    TempoEvent_arr_free(&tempos);
    SysExEvent_arr_free(&sysexs);
    big_free(eventArr);
    big_free(timingArr);
    // trackcolors can be toggled between load and unload, so key off the pointer
    big_free(trackGroups.data);
    trackGroups.data = NULL;
    trackGroups.count = 0;
    sysexArr = NULL;
    tempoArr = NULL;
    eventArr = NULL;
    timingArr = NULL;
    puts("successfully freed midi");
}