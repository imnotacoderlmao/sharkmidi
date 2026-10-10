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
    #define add(a, b) __atomic_fetch_add(&a, b, __ATOMIC_RELAXED);
#else
    #define add(a, b) a += b;     
#endif

typedef struct
{
    int64_t start;
    uint32_t length;
} trackProperties;

typedef struct
{
    int64_t tick;
    uint32_t count;
    uint32_t notecount;
    uint16_t track;
    uint32_t group_idx;
} ExtendedTickGroup;

uint8_t* filePtr = NULL;
uint64_t fileLen = 0;
uint64_t filePos = 0;
struct stat filestat;
uint32_t headersize = 0; 
uint16_t trackAmount = 0;
uint32_t fmt = 0;
int32_t midiloaded = 0, trackcolors = 1;
uint64_t eventcount = 0;

DEFINE_ARRAY(trackProperties);
DEFINE_ARRAY(TickGroup);
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
    GetFileSizeEx(filehandle, &fileSize);
    if (filehandle == INVALID_HANDLE_VALUE)
    {
        updatestatsandprint("error getting file properties, does the file even exist?");
        CloseHandle(filehandle);
        return 0;
    }
    HANDLE mapfilehandle = CreateFileMapping(filehandle, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(filehandle);
    fileLen = fileSize.QuadPart;
    filePos = 0;
    filePtr = MapViewOfFile(mapfilehandle, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapfilehandle);
    printf("successfully initiated memory mapped file with %lu bytes in size\n", fileLen);
    return 1;
}
#else
#include <sys/mman.h>
int InitMMF(const char* filepath)
{
    int filedesc = open(filepath, O_RDONLY);
    if (fstat(filedesc, &filestat) == -1)
    {
        updatestatsandprint("error getting filesize, does the file even exist?\n");
        close(filedesc);
        return 0;
    }
    fileLen = filestat.st_size;
    filePos = 0;
    filePtr = mmap(NULL, filestat.st_size, PROT_READ, MAP_PRIVATE, filedesc, 0);
    close(filedesc);
    printf("successfully initiated memory mapped file with %lu bytes in size\n", fileLen);
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
        uint32_t size = ReadUInt32();
        trackProperties track = {filePos, size};
        trackProperties_arr_push(tracklistptr, track);
        filePos += size;
        trackAmount++;
        return 1;
    }
    return 0;
}

static int cmp_extended_tickgroup(const void* a, const void* b)
{
    int64_t posa = ((const ExtendedTickGroup*)a)->tick;
    int64_t posb = ((const ExtendedTickGroup*)b)->tick;
    return (posa > posb) - (posa < posb);
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

int64_t CountTrackEvents(uint8_t* trackPtr, uint8_t* trackEnd, TickGroup_arr* tickgroup)
{
    int64_t totalnotes_local = 0;
    int64_t eventcount_local = 0;
    int64_t absolutetime = 0;
    uint8_t prevEvent = 0;
    int32_t trackMaxTick = 0;
    uint64_t count = 0;
    uint32_t notecount = 0;
    
    while (trackPtr < trackEnd)
    {
        int64_t delta = *trackPtr++;
        if (delta >= 0x80)
            delta = varlen_decode_slow(&delta, &trackPtr);
        if(delta > 0)
        {
            if (count > 0)
            {
                TickGroup group = {absolutetime, notecount, count};
                TickGroup_arr_push(tickgroup, group);
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
                        {
                            trackMaxTick = absolutetime;
                            goto finalize;
                        }
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
        if (trackMaxTick > maxTick)
            maxTick = trackMaxTick;
        if (count > 0)
        {
            TickGroup group = {absolutetime, notecount, count};
            TickGroup_arr_push(tickgroup, group);
            eventcount_local += count;
            totalnotes_local += notecount;
        }
        add(totalnotes, totalnotes_local);
        return eventcount_local;
}

int64_t ParseTrackEvents(uint8_t* trackPtr, uint8_t* trackEnd, uint24_t* msgPtr, TickGroup_arr* trackHistogram, TempoEvent_arr* tempo, SysExEvent_arr* sysex, uint8_t track)
{
    int64_t absolutetime = 0;
    int64_t notecount = 0;
    uint8_t prevEvent = 0;

    uint32_t group_idx = 0;
    int64_t write_pos = 0;
    uint32_t events_in_group = 0;

    if (trackHistogram->count > 0)
        write_pos = trackHistogram->data[0].event_offset;

    while (trackPtr < trackEnd)
    {
        int64_t delta = *trackPtr++;
        if (delta >= 0x80)
            delta = varlen_decode_slow(&delta, &trackPtr);

        if (delta > 0 && events_in_group > 0)
        {
            group_idx++;
            if (group_idx < trackHistogram->count)
                write_pos = trackHistogram->data[group_idx].event_offset;
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
                if (data2 != 0)
                    notecount++;
                else 
                {
                    msgPtr[pos] = uint24_from(0x80 | (prevEvent & 0x0F) | (readEvent << 8) | (64 << 16));
                    continue;
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
double parsetime = 0.0;
void printparsestatistics(void)
{
    char parsestatistics[] =
    "=============== PARSE STATICTICS ===============\n"
    "   MIDI Name:              %s\n"
    "   Filesize:               %s Bytes\n"
    "   Took:\n"
    "       Count:              %lfs (%s notes/s)\n"
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
    "       Events:             %s Bytes (3 bytes/event)\n"
    "       Track Index (RLE):  %s Bytes (%s groups)\n"
    "       Total:              %s Bytes\n"
    "===============================================\n";

    uint64_t trackRLEBytes = (trackcolors? trackGroups.count : 0) * sizeof(TrackGroup);
    printf(parsestatistics, filename, 
        AddCommas(fileLen), counttime, AddCommas((totalnotes / counttime)), parsetime, 
        AddCommas((totalnotes / parsetime)), AddCommas(ppq), AddCommas(maxTick), AddCommas(activetickcount), AddCommas(trackAmount),
        AddCommas(eventcount), AddCommas(totalnotes), AddCommas(tempoCount - 1), AddCommas(sysexCount - 1),
        AddCommas(activetickcount * sizeof(TickGroup)), AddCommas(eventcount * sizeof(uint24_t)),
        AddCommas(trackRLEBytes), AddCommas(trackGroups.count),
        AddCommas((activetickcount * sizeof(TickGroup)) + (eventcount * sizeof(uint24_t)) + trackRLEBytes));
}

TempoEvent_arr tempos = {0};
SysExEvent_arr sysexs = {0};

int LoadMIDI(const char* filepath)
{
    UnloadMIDI();
    updatestatsandprint("initiating memory mapped file\n");
    
    if (!InitMMF(filepath)) 
        return midiloaded;
        
    VerifyHeader();
    maxTick = 0;
    trackProperties_arr tracks = {0};
    
    while(filePos < fileLen)
    {
        if (!IndexTrack(&tracks))
            break;
        updatestatsandprint("\rfound %d tracks", trackAmount);
    }
    updatestatsandprint("\ncounting events\n");

    TickGroup_arr* histogram = calloc(trackAmount, sizeof(TickGroup_arr));
    double start = get_time();
    int32_t loadedtracks = 0;
    
    #ifdef _OPENMP
        #pragma omp parallel for
    #endif
    for(int32_t i = 0; i < trackAmount; i++)
    {
        trackProperties* currtrack = &tracks.data[i];
        uint8_t* trackstart = filePtr + currtrack->start;
        add(eventcount, CountTrackEvents(trackstart, trackstart + currtrack->length, &histogram[i]));
        add(loadedtracks, 1);
        printf("(%s/%s) tracks scanned, %s notes counted\r", AddCommas(loadedtracks), AddCommas(trackAmount), AddCommas(totalnotes));
        fflush(stdout);
    }
    double end = get_time();
    counttime = end - start;

    updatestatsandprint("\ncounting active ticks\n");
    int64_t total_entries = 0;
    for (int32_t i = 0; i < trackAmount; i++)
        total_entries += histogram[i].count;

    updatestatsandprint("concatenating per track timing array\n");
    ExtendedTickGroup* flat = malloc(total_entries * sizeof(ExtendedTickGroup));
    int64_t flat_idx = 0;
    for (int32_t i = 0; i < trackAmount; i++)
    {
        for (size_t j = 0; j < histogram[i].count; j++)
        {
            flat[flat_idx++] = (ExtendedTickGroup){
                .tick = histogram[i].data[j].tick,
                .count = (uint32_t)histogram[i].data[j].event_offset,
                .notecount = (uint32_t)histogram[i].data[j].notecount,
                .track = (uint16_t)i,
                .group_idx = (uint32_t)j
            };
        }
    }

    updatestatsandprint("sorting timing array by tick\n");
    qsort(flat, total_entries, sizeof(ExtendedTickGroup), cmp_extended_tickgroup);

    activetickcount = 0;
    if (total_entries > 0)
    {
        activetickcount = 1;
        for (size_t i = 1; i < total_entries; i++)
        {
            if (flat[i].tick != flat[i - 1].tick)
                activetickcount++;
        }
    }

    updatestatsandprint("creating main timing array and track RLE\n");
    timingArr = calloc(activetickcount + 1, sizeof(TickGroup));
    if (trackcolors)
    {
        trackGroups.data = malloc(total_entries * sizeof(TrackGroup));
        trackGroups.count = total_entries;
    }
    int64_t current_global_offset = 0;
    int64_t accumulated_notes = 0;

    if (total_entries > 0)
    {
        int64_t out_idx = 0;
        timingArr[0].tick = flat[0].tick;
        timingArr[0].event_offset = 0;
        timingArr[0].notecount = 0;

        for (size_t i = 0; i < total_entries; i++)
        {
            ExtendedTickGroup* eg = &flat[i];

            if (i > 0 && eg->tick != timingArr[out_idx].tick)
            {
                out_idx++;
                timingArr[out_idx].tick = eg->tick;
                timingArr[out_idx].event_offset = current_global_offset;
                timingArr[out_idx].notecount = accumulated_notes;
            }

            timingArr[out_idx].notecount += eg->notecount;
            accumulated_notes += eg->notecount;

            histogram[eg->track].data[eg->group_idx].event_offset = current_global_offset;
            if (trackcolors)
            {
                trackGroups.data[i] = (TrackGroup){
                    .event_offset = current_global_offset,
                    .count = eg->count,
                    .track = (uint8_t)(eg->track << 4)
                };
            }

            current_global_offset += eg->count;
        }
    }
    free(flat);
    flat = NULL;

    updatestatsandprint("creating main event array\n");
    eventArr = calloc(eventcount + 1, sizeof(uint24_t));

    loadedtracks = 0;
    updatestatsandprint("actually parsing events this time\n");
    playednotes = 0;
    start = get_time();
    
    #ifdef _OPENMP
        #pragma omp parallel for
    #endif
    for(int32_t i = 0; i < trackAmount; i++)
    {
        trackProperties* currtrack = &tracks.data[i];
        uint8_t* trackstart = filePtr + currtrack->start;
        add(playednotes, ParseTrackEvents(trackstart, trackstart + currtrack->length, eventArr, &histogram[i], &tempos, &sysexs, (i << 4)));
        add(loadedtracks, 1);
        printf("(%s/%s) tracks parsed, %s notes parsed\r", AddCommas(loadedtracks), AddCommas(trackAmount), AddCommas(playednotes));
        fflush(stdout);
    }
    end = get_time();    

    for (int32_t i = 0; i < trackAmount; i++)
        TickGroup_arr_free(&histogram[i]);
    free(histogram);

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
    munmap(filePtr, filestat.st_size);
    filePtr = NULL;
    midiloaded = 1;
    filename = getfilename(filepath);
    printparsestatistics();
    return midiloaded;
}

void UnloadMIDI(void)
{
    if (!midiloaded) return;
    stopping = 1;
    midiloaded = 0;
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
    free(eventArr);
    free(timingArr);
    if (trackcolors)
    {
        free(trackGroups.data);
        trackGroups.data = NULL;
        trackGroups.count = 0;
    }
    sysexArr = NULL;
    tempoArr = NULL;
    eventArr = NULL;
    timingArr = NULL;
    puts("successfully freed midi");
}