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
    return UnmapViewOfFile(addr)? 0 : -1;
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
    int filename_len = strlen(start_ptr);
    char* filename = malloc((filename_len + 1) * sizeof(char));
    strcpy(filename, start_ptr);
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

static int cmp_tempo(const void* a, const void* b)
{
    int32_t posa = ((const TempoEvent*)a)->tick;
    int32_t posb = ((const TempoEvent*)b)->tick;
    return (posa > posb) - (posa < posb);
}

static int cmp_sysex(const void* a, const void* b)
{
    int32_t posa = ((const SysExEvent*)a)->tick;
    int32_t posb = ((const SysExEvent*)b)->tick;
    return (posa > posb) - (posa < posb);
}

/*int __attribute__((noinline)) varlendecode_noinline(uint8_t* trackptr)
{
    int32_t delta = *trackptr++;
    if (delta >= 0x80)
    {
        delta &= 0x7F;
        uint8_t b = 0;
        do 
        { 
            b = *trackptr++;
            delta = (delta << 7) | (b & 0x7F); 
        } 
        while (b >= 0x80);
    }
    return delta;
}*/

int64_t CountTrackEvents(uint8_t* trackPtr, uint8_t* trackEnd, TickGroup_arr* tickgroup)
{
    int64_t totalnotes_local = 0;
    int64_t eventcount_local = 0;
    int32_t absolutetime = 0;
    uint8_t prevEvent = 0;
    int32_t trackMaxTick = 0;
    uint64_t count = 0;
    uint32_t notecount = 0;
    
    while (trackPtr < trackEnd)
    {
        // also inline varlen decode
        int32_t delta = *trackPtr++;
        if (delta >= 0x80)
        {
            delta &= 0x7F;
            uint8_t b = 0;
            do 
            { 
                b = *trackPtr++;
                delta = (delta << 7) | (b & 0x7F); 
            } 
            while (b >= 0x80);
        }

        if(delta > 0)
        {
            if (count > 0)
            {
                TickGroup group = {absolutetime, notecount, count};
                TickGroup_arr_push(tickgroup, group);
                eventcount_local += count;
                totalnotes_local += notecount;
                //eventcount += count;
                //totalnotes += notecount;
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
                            int32_t len = 0;
                            while (true)
                            {
                                uint8_t curByte = *trackPtr++;
                                len = (len << 7) | (curByte & 0x7F);
                                if ((curByte & 0x80) == 0) 
                                    break;
                            }
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
                            int32_t len2 = 0;
                            while (true)
                            {
                                uint8_t curByte = *trackPtr++;
                                len2 = (len2 << 7) | (curByte & 0x7F);
                                if ((curByte & 0x80) == 0) 
                                    break;
                            }
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
            printf("\ndear lord what is wrong with your midi file's varlen. current tick = %d", absolutetime);
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
    int32_t absolutetime = 0;
    int64_t notecount = 0;
    uint8_t prevEvent = 0;
    #ifdef _OPENMP
        #define next_pos __atomic_fetch_add(&writeCursors[absolutetime], 1, __ATOMIC_RELAXED)
    #else
        #define next_pos writeCursors[absolutetime]++
    #endif

    while (trackPtr < trackEnd)
    {
        // inline varlen decode
        int32_t delta = *trackPtr++;
        if (delta >= 0x80)
        {
            delta &= 0x7F;
            uint8_t b = 0;
            do 
            { 
                b = *trackPtr++;
                delta = (delta << 7) | (b & 0x7F); 
            } 
            while (b >= 0x80);
        }
        absolutetime += delta;
        uint8_t readEvent = *trackPtr++;
        if (readEvent >= 0x80)
        {
            if (readEvent < 0xF0)
            {
                prevEvent = readEvent;
                long pos = next_pos;
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
                        int size = 0;
                        while (true)
                        {
                            uint8_t curByte = *trackPtr++;
                            size = (size << 7) | (curByte & 0x7F);
                            if ((curByte & 0x80) == 0) 
                                break;
                        }
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
                            int len = 0;
                            while (true)
                            {
                                uint8_t curByte = *trackPtr++;
                                len = (len << 7) | (curByte & 0x7F);
                                if ((curByte & 0x80) == 0) 
                                    break;
                            }
                            int32_t tempoVal = 0;
                            for (int i = 0; i < len; i++) 
                                tempoVal = (tempoVal << 8) | *trackPtr++;
                            TempoEvent tev = {absolutetime, uint24_from(tempoVal)};
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
                            int len = 0;
                            while (true)
                            {
                                uint8_t curByte = *trackPtr++;
                                len = (len << 7) | (curByte & 0x7F);
                                if ((curByte & 0x80) == 0) 
                                    break;
                            }
                            trackPtr += len;
                        }
                        continue;
                    }
                }
            }
        }
        else
        {
            long pos = next_pos;
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
    #undef next_pos
    return notecount;
}

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
    double parsetime = end - start;
    printf("\ncounted %ld notes in %lf seconds (%lf notes/sec), building histogram\n", totalnotes, parsetime, (double)(totalnotes/parsetime));
    timingArr = calloc(maxTick + 2, sizeof(TickGroup));
    for(int32_t i = 0; i < trackAmount; i++)
    {
        for(int64_t t = 0; t < histogram[i].count; t++)
        {
            TickGroup* group = &histogram[i].data[t];
            timingArr[group->tick].notecount += group->notecount;
            timingArr[group->tick].event_offset += group->event_offset;
        }
        TickGroup_arr_free(&histogram[i]);
    }
    free(histogram);
    histogram = NULL;
    TempoEvent_arr tempos = {0};
    SysExEvent_arr sysexs = {0};
    eventArr = calloc(eventcount + 1, sizeof(uint24_t));
    trackArr = calloc(eventcount + 1, sizeof(uint8_t));
    int64_t* writeCursors = calloc(maxTick + 2, sizeof(int64_t));
    int64_t offset = 0;
    for (int32_t t = 0; t <= maxTick; t++)
    {
        int64_t cnt = timingArr[t].event_offset;
        writeCursors[t] = offset;
        timingArr[t].tick = t;
        timingArr[t].event_offset = offset;
        offset += cnt;
    }
    loadedtracks = 0;
    totalnotes = 0;
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
    // sentinels
    timingArr[maxTick + 1] = (TickGroup){INT32_MAX, totalnotes, eventcount};
    TempoEvent tev = {INT32_MAX, uint24_from(500000)};
    TempoEvent_arr_push(&tempos, tev);
    SysExEvent sex = {INT32_MAX};
    SysExEvent_arr_push(&sysexs, sex);

    parsetime = end - start;
    qsort(tempos.data, tempos.count, sizeof(TempoEvent), cmp_tempo);
    qsort(sysexs.data, sysexs.count, sizeof(SysExEvent), cmp_sysex);
    tempoArr = tempos.data;
    sysexArr = sysexs.data;
    tempoCount = tempos.count;
    sysexCount = sysexs.count;
    printf(
        "\nparsed in %lf seconds. which is %s notes/sec.\nMIDI Stats below\nNotecount: %s notes from %s tracks\nLength: %s ticks\nPPQ: %s\nplayback is ready\n", parsetime, 
        AddCommas((double)(totalnotes/parsetime)), AddCommas(totalnotes), AddCommas(trackAmount), AddCommas(maxTick), AddCommas(ppq));
    free(writeCursors);
    writeCursors = NULL;
    trackProperties_arr_free(&tracks);
    munmap(filePtr, filestat.st_size);
    filePtr = NULL;
    midiloaded = 1;
    filename = getfilename(filepath);
    return midiloaded;
}

void UnloadMIDI(void)
{
    if (!midiloaded) return;
    stopping = 1;
    midiloaded = 0;
    trackAmount = 0;
    ppq = 0;
    totalnotes = 0;
    eventcount = 0;
    filename = "no midi loaded";
    for (uint32_t i = 0; i < sysexCount; i++)
        free(sysexArr[i].message);
    free(sysexArr);
    free(tempoArr);
    free(eventArr);
    free(timingArr);
    sysexArr = NULL;
    tempoArr = NULL;
    eventArr = NULL;
    timingArr = NULL;
    puts("successfully freed midi");
}
