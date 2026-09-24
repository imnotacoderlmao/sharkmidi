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

#ifdef _OPENMP
    #include <omp.h>
    #define add(a, b) __atomic_fetch_add(&a, b, __ATOMIC_RELAXED);
#else
    #define add(a, b) a += b;     
#endif

static inline long get_next_pos(int64_t absolutetime, uint32_t* active_idx, int64_t* writeCursors)
{
    while (timingArr[*active_idx].tick < absolutetime)
    {
        (*active_idx)++;
    }
#ifdef _OPENMP
    return __atomic_fetch_add(&writeCursors[*active_idx], 1, __ATOMIC_RELAXED);
#else
    return writeCursors[*active_idx]++;
#endif
}

typedef struct
{
    int64_t start;
    uint32_t length;
} trackProperties;

#define true 1
#define false 0

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
        puts("error getting file properties, does the file even exist?");
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
        puts("error getting filesize, does the file even exist?");
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
    *len &= 0x7F;
    uint8_t b = 0;
    do 
    { 
        b = *((*trackPtr)++);
        *len = (*len << 7) | (b & 0x7F); 
    } 
    while (b >= 0x80);
    return *len;
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

static int SearchText(char* text)
{
    for (int32_t i = 0; i < strlen(text); i++)
    {
        if (filePos >= fileLen) 
            return false;
        if (filePtr[filePos++] != text[i])
        {
            printf("issue searching for %s\n", text);
            return false;
        }
    }
    return true;
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
        return true;
    }
    return false;
}

static int cmp_tickgroup(const void* a, const void* b)
{
    int64_t posa = ((const TickGroup*)a)->tick;
    int64_t posb = ((const TickGroup*)b)->tick;
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
                        }
                        continue;
                    case 0xF1: 
                        trackPtr += 1; 
                        continue;
                    case 0xF2: 
                        trackPtr += 2; 
                        continue;
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
    puts("does this midi not have an end of track? this message isnt supposed to appear otherwise");
    finalize:
        if (absolutetime > 1 << 28)
            printf("\ndear lord what is with your midi file's length. current tick = %ld\n", absolutetime);
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

int64_t ParseTrackEvents(uint8_t* trackPtr, uint8_t* trackEnd, uint24_t* msgPtr, uint8_t* trackidxptr, int64_t* writeCursors, TempoEvent_arr* tempo, SysExEvent_arr* sysex, uint8_t track)
{
    int64_t absolutetime = 0;
    int64_t notecount = 0;
    uint8_t prevEvent = 0;
    uint32_t active_idx = 0;

    while (trackPtr < trackEnd)
    {
        int64_t delta = *trackPtr++;
        if (delta >= 0x80)
            delta = varlen_decode_slow(&delta, &trackPtr);

        absolutetime += delta;
        uint8_t readEvent = *trackPtr++;
        if (readEvent >= 0x80)
        {
            if (readEvent < 0xF0)
            {
                prevEvent = readEvent;
                long pos = get_next_pos(absolutetime, &active_idx, writeCursors);
                if (trackcolors)
                    trackArr[pos] = track;
                if ((readEvent & 0xE0) != 0xC0)
                {
                    uint8_t data1 = *trackPtr++;
                    uint8_t data2 = *trackPtr++;
                    if ((readEvent & 0xF0) == 0x90)
                    {
                        if(data2 != 0)
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
                        if(size >= 0x80)
                            size = varlen_decode_slow(&size, &trackPtr);
                        
                        uint8_t* data = malloc(size + 1);
                        data[0] = readEvent;
                        for (uint32_t i = 1; i < (uint32_t)(size + 1); i++)
                            data[i] = *trackPtr++;
                        SysExEvent sex = {absolutetime, size + 1, data};
                        #ifdef _OPENMP
                            #pragma omp critical
                        #endif
                        SysExEvent_arr_push(sysex, sex);
                        continue;
                    }
                    case 0xF1:
                    { 
                        trackPtr += 1; 
                        continue;
                    }
                    case 0xF2:
                    { 
                        trackPtr += 2; 
                        continue;
                    }
                    case 0xF3: 
                    {    
                        trackPtr += 1; 
                        continue;
                    }
                    case 0xFF:
                    {
                        readEvent = *trackPtr++;
                        if (readEvent == 0x51)
                        {
                            int64_t len = *trackPtr++;
                            if(len >= 0x80)
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
            long pos = get_next_pos(absolutetime, &active_idx, writeCursors);
            if (trackcolors)
                trackArr[pos] = track;
            if ((prevEvent & 0xE0) != 0xC0)
            {
                uint8_t data2 = *trackPtr++;
                if(data2 != 0)
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
    "   MIDI Name: %s\n"
    "   Filesize:  %s Bytes\n"
    "   Took:\n"
    "       Count: %lfs (%s notes/s)\n"
    "       Parse: %lfs (%s notes/s)\n"
    "   Counted:\n"
    "       MIDI Tracks: %s\n"
    "       MIDI Ticks:  %s (%ld has events)\n"
    "       Channel Events: %s\n"
    "       Note Events:  %s\n"
    "       Tempo Events: %s\n"
    "       SysEx Events: %s\n"
    "   Memory Usage:\n"
    "       Timing:         %s Bytes (24 bytes/entry)\n"
    "       Events:         %s Bytes (3 bytes/event)\n"
    "       Track Index:    %s Bytes (1 byte/event)\n"
    "===============================================\n";
    // dear god
    char filesize_str[24];
    char scan_nps_str[24], parse_nps_str[24];
    char eventcount_str[24];
    char tempocount_str[24], sysexcount_str[24];
    char timeline_bytes[24], events_bytes[24], track_bytes[24];
    AddCommas(fileLen, filesize_str);
    AddCommas(trackAmount, trackamount_str);
    AddCommas(maxTick, maxtick_str);
    AddCommas(eventcount, eventcount_str);
    AddCommas(totalnotes, totalnotes_str);
    AddCommas((totalnotes / counttime), scan_nps_str);
    AddCommas((totalnotes / parsetime), parse_nps_str);
    AddCommas(tempoCount, tempocount_str);
    AddCommas(sysexCount, sysexcount_str);
    AddCommas(activetickcount * sizeof(TickGroup), timeline_bytes);
    AddCommas(eventcount * sizeof(uint24_t), events_bytes);
    AddCommas(eventcount * sizeof(uint8_t), track_bytes);
    printf(parsestatistics, filename, filesize_str, counttime, scan_nps_str, 
        parsetime, parse_nps_str, trackamount_str, maxtick_str, activetickcount, 
        eventcount_str, totalnotes_str, tempocount_str, sysexcount_str,
        timeline_bytes, events_bytes, track_bytes);
}

TempoEvent_arr tempos = {0};
SysExEvent_arr sysexs = {0};

int LoadMIDI(const char* filepath)
{
    UnloadMIDI();
    printf("loading %s or something\n", filepath);
    if (!InitMMF(filepath)) 
        return midiloaded;
    VerifyHeader();
    maxTick = 0;
    trackProperties_arr tracks = {0};
    while(filePos < fileLen)
    {
        if (!IndexTrack(&tracks))
            break;
        printf("\rfound %d tracks", trackAmount);
    }
    puts("\ncounting events");
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
        printf("(%d/%d) tracks scanned, %ld notes counted\r", loadedtracks, trackAmount, totalnotes);
    }
    double end = get_time();
    counttime = end - start;
    puts("\ncounting active ticks");
    int64_t total_entries = 0;
    for (int32_t i = 0; i < trackAmount; i++)
        total_entries += histogram[i].count;
    puts("concatenating per-track timing array");
    TickGroup* flat = malloc(total_entries * sizeof(TickGroup));
    int64_t flat_idx = 0;
    for (int32_t i = 0; i < trackAmount; i++)
    {
        if (histogram[i].count > 0)
        {
            memcpy(flat + flat_idx, histogram[i].data, histogram[i].count * sizeof(TickGroup));
            flat_idx += histogram[i].count;
        }
        TickGroup_arr_free(&histogram[i]);
    }
    free(histogram);
    histogram = NULL;
    puts("sorting timing array by tick");
    qsort(flat, total_entries, sizeof(TickGroup), cmp_tickgroup);

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
    puts("creating main timing array");
    timingArr = calloc(activetickcount + 1, sizeof(TickGroup));
    int64_t* writeCursors = calloc(activetickcount + 1, sizeof(int64_t));

    int64_t offset = 0;
    int64_t notecount = 0;
    if (total_entries > 0)
    {
        int64_t out_idx = 0;
        timingArr[0].tick = flat[0].tick;
        int64_t current_note_cnt = flat[0].notecount;
        int64_t current_event_cnt = flat[0].event_offset;

        for (size_t i = 1; i < total_entries; i++)
        {
            if (flat[i].tick == timingArr[out_idx].tick)
            {
                timingArr[out_idx].notecount += flat[i].notecount;
                current_event_cnt += flat[i].event_offset;
                current_note_cnt += flat[i].notecount;
            }
            else
            {
                writeCursors[out_idx] = offset;
                timingArr[out_idx].event_offset = offset;
                timingArr[out_idx].notecount = notecount;
                notecount += current_note_cnt;
                offset += current_event_cnt;

                out_idx++;
                timingArr[out_idx].tick = flat[i].tick;
                current_event_cnt = flat[i].event_offset;
                current_note_cnt = flat[i].notecount;
            }
        }
        writeCursors[out_idx] = offset;
        timingArr[out_idx].event_offset = offset;
        timingArr[out_idx].notecount = notecount;
        offset += current_event_cnt;
        notecount += current_note_cnt;
    }
    free(flat);
    flat = NULL;
    puts("creating main event array");
    eventArr = calloc(eventcount + 1, sizeof(uint24_t));
    trackArr = calloc(eventcount + 1, sizeof(uint8_t));

    loadedtracks = 0;
    totalnotes = 0;
    puts("actually parsing events this time");
    start = get_time();
    #ifdef _OPENMP
        #pragma omp parallel for
    #endif
    for(int32_t i = 0; i < trackAmount; i++)
    {
        trackProperties* currtrack = &tracks.data[i];
        uint8_t* trackstart = filePtr + currtrack->start;
        add(totalnotes, ParseTrackEvents(trackstart, trackstart + currtrack->length, eventArr, trackArr, writeCursors, &tempos, &sysexs, (i << 4)))
        add(loadedtracks, 1);
        printf("(%d/%d) tracks parsed, %ld notes parsed\r", loadedtracks, trackAmount, totalnotes);
    }
    end = get_time();    

    puts("adding dummy events and sorting sysex, tempos");
    // sentinels
    timingArr[activetickcount] = (TickGroup){INT32_MAX, (int32_t)totalnotes, (int64_t)eventcount};
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
    free(writeCursors);
    writeCursors = NULL;
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
    free((void*)filename); 
    filename = "no midi loaded";
    filename_len = 0;
    
    for (uint32_t i = 0; i < sysexCount; i++)
        free(sysexArr[i].message);
    
    TempoEvent_arr_free(&tempos);
    SysExEvent_arr_free(&sysexs);
    free(eventArr);
    free(trackArr);
    free(timingArr);
    sysexArr = NULL;
    tempoArr = NULL;
    eventArr = NULL;
    timingArr = NULL;
    puts("successfully freed midi");
}