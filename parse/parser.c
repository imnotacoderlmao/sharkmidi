#include "parser.h"
#include "midistorage.h"
#include "../misc/typed-array.h"
#include "../playback/timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdatomic.h>
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#define _stat64 stat;
void* mmap_wrapper(void* addr, size_t length, int fd, off_t offset) 
{
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    HANDLE hMap = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMap);
    return pView;
}
int munmap(void* addr, size_t length) {
    return UnmapViewOfFile(addr) ? 0 : -1;
}
#else
#include <sys/mman.h>
void* mmap_wrapper(void* addr, size_t length, int fd, off_t offset)
{
    return mmap(addr, length, PROT_READ, MAP_PRIVATE, fd, 0);
}
#endif


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
int32_t midiloaded = 0;
uint64_t eventcount = 0;
DEFINE_ARRAY(trackProperties);
DEFINE_ARRAY(TickGroup);
DEFINE_ARRAY(TempoEvent);
DEFINE_ARRAY(SysExEvent);

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

static int SearchText(uint8_t* text)
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

int32_t InitMMF(uint8_t* filepath)
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
    filePtr = mmap_wrapper(NULL, filestat.st_size, filedesc, 0);
    close(filedesc);
    printf("successfully initiated memory mapped file with %lu bytes in size\n", fileLen);
    return 1;
}

void countTrackEvents(uint8_t* trackPtr, uint8_t* trackEnd, TickGroup_arr* tickgroup)
{
    int32_t absolutetime = 0;
    int32_t lastTick = 0;
    uint8_t prevEvent = 0;
    int32_t trackMaxTick = 0;
    uint64_t count = 0;
    uint32_t notecount = 0;
    
    while (trackPtr < trackEnd)
    {
        // also inline varlen decode
        uint8_t b = *trackPtr++;
        if ((b & 0x80) == 0)
            absolutetime += b;
        else
        {
            int delta = b & 0x7F;
            do 
            { 
                b = *(trackPtr++); 
                delta = (delta << 7) | (b & 0x7F); 
            } 
            while ((b & 0x80) != 0);
            absolutetime += delta;
        }
        uint8_t readEvent = *trackPtr++;
        if (readEvent < 0x80)
        {
            trackPtr--;
            readEvent = prevEvent; 
        }
        uint8_t status = (uint8_t)(readEvent & 0xF0);
        
        if (readEvent >= 0x80 && readEvent < 0xF0)
            prevEvent = readEvent;
        
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
                int32_t len2 = 0;
                
                while (true)
                {
                    uint8_t curByte = *trackPtr++;
                    len2 = (len2 << 7) | (curByte & 0x7F);
                    if ((curByte & 0x80) == 0) 
                        break;
                }
                if (readEvent == 0x2F)
                {
                    trackMaxTick = absolutetime;
                    goto finalize;
                }
                else 
                {
                    trackPtr += len2;
                }
                continue;
        }
        if (lastTick != absolutetime && count > 0)
        {
            TickGroup group = {lastTick, notecount, count};
            TickGroup_arr_push(tickgroup, group);
            add(eventcount, count);
            add(totalnotes, notecount);
            //eventcount += count;
            //totalnotes += notecount;
            notecount = 0;
            count = 0;
        }
        lastTick = absolutetime;
        switch (status)
        {
            case 0x90:
                trackPtr += 1;
                if (*trackPtr++ != 0) 
                    notecount++;
                count++;
                break;
            case 0x80:
            case 0xA0:
            case 0xB0:
            case 0xE0:
                trackPtr += 2;
                count++;
                break;
            case 0xC0:
            case 0xD0:
                trackPtr += 1;
                count++;
                break;
        }
    }
    puts("does this midi not have an end of track uint8_t? this message isnt supposed to appear otherwise");
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
            TickGroup group = {lastTick, notecount, count};
            TickGroup_arr_push(tickgroup, group);
            add(eventcount, count);
            add(totalnotes, notecount);
        }
        return;
}

int64_t ParseTrackEvents(uint8_t* trackPtr, uint8_t* trackEnd, uint24_t* msgPtr, int64_t* writeCursors, TempoEvent_arr* tempo, SysExEvent_arr* sysex)
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
        uint8_t b = *trackPtr++;
        if ((b & 0x80) == 0)
            absolutetime += b;
        else
        {
            int delta = b & 0x7F;
            do 
            { 
                b = *trackPtr++; 
                delta = (delta << 7) | (b & 0x7F); 
            } 
            while ((b & 0x80) != 0);
            absolutetime += delta;
        }
        uint8_t readEvent = *trackPtr++;
        if (readEvent < 0x80) 
        { 
            trackPtr--;
            readEvent = prevEvent; 
        }
        
        uint8_t status = (uint8_t)(readEvent & 0xF0);
        if (readEvent >= 0x80 && readEvent < 0xF0)
            prevEvent = readEvent;
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
        switch (status)
        {
            case 0x80:
            {
                uint8_t note = *trackPtr++; 
                uint8_t vel = *trackPtr++;
                msgPtr[next_pos] = uint24_from(readEvent | (note << 8) | (vel << 16));
                break;
            }
            case 0x90:
            {
                uint8_t note = *trackPtr++;
                uint8_t vel = *trackPtr++;
                if (vel != 0)
                { 
                    notecount++; 
                    msgPtr[next_pos] = uint24_from(readEvent | (note << 8) | (vel << 16));
                }
                else 
                { 
                    uint8_t channel = (uint8_t)(readEvent & 0x0F);
                    uint8_t dummynoteoff = (uint8_t)(0x80 | channel);  
                    msgPtr[next_pos] = uint24_from(dummynoteoff | (note << 8) | (64 << 16));
                }
                break;
            }
            case 0xA0: 
            { 
                uint8_t note = *trackPtr++;
                uint8_t pressure = *trackPtr++; 
                msgPtr[next_pos] = uint24_from(readEvent | (note << 8) | (pressure << 16));
                break; 
            }
            case 0xB0: 
            { 
                uint8_t controller = *trackPtr++;
                uint8_t val = *trackPtr++;      
                msgPtr[next_pos] = uint24_from(readEvent | (controller << 8) | (val << 16));
                break; 
            }
            case 0xC0: 
            { 
                uint8_t prog = *trackPtr++;                            
                msgPtr[next_pos] = uint24_from(readEvent | (prog << 8));
                break; 
            }
            case 0xD0: 
            { 
                uint8_t pres = *trackPtr++;                            
                msgPtr[next_pos] = uint24_from(readEvent | (pres << 8));
                break; 
            }
            case 0xE0: 
            { 
                uint8_t lsb = *trackPtr++; 
                uint8_t msb = *trackPtr++;      
                msgPtr[next_pos] = uint24_from(readEvent | (lsb << 8) | (msb << 16));
                break; 
            }
        }
    }
    #undef next_pos
    return notecount;
}

int32_t LoadMIDI(uint8_t* filepath)
{
    midiloaded = 0;
    printf("loading %s or something\n", filepath);
    if (!InitMMF(filepath)) return midiloaded;
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
        countTrackEvents(trackstart, trackstart + currtrack->length, &histogram[i]);
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
    // sentinel
    timingArr[maxTick + 1] = (TickGroup){INT32_MAX, totalnotes, eventcount};
    start = get_time();
    loadedtracks = 0;
    totalnotes = 0;
    #ifdef _OPENMP
        #pragma omp parallel for
    #endif
    for(int32_t i = 0; i < trackAmount; i++)
    {
        trackProperties* currtrack = &tracks.data[i];
        uint8_t* trackstart = filePtr + currtrack->start;
        add(totalnotes, ParseTrackEvents(trackstart, trackstart + currtrack->length, eventArr, writeCursors, &tempos, &sysexs))
        add(loadedtracks, 1);
        printf("(%d/%d) tracks parsed, %ld notes parsed\r", loadedtracks, trackAmount, totalnotes);
    }
    
    // sentinel part 2
    TempoEvent tev = {INT32_MAX, uint24_from(500000)};
    TempoEvent_arr_push(&tempos, tev);
    SysExEvent sex = {INT32_MAX};
    SysExEvent_arr_push(&sysexs, sex);

    end = get_time();
    parsetime = end - start;
    qsort(tempos.data, tempos.count, sizeof(TempoEvent), cmp_tempo);
    qsort(sysexs.data, sysexs.count, sizeof(SysExEvent), cmp_sysex);
    tempoArr = tempos.data;
    sysexArr = sysexs.data;
    tempoCount = tempos.count;
    sysexCount = sysexs.count;
    printf("\nparsed in %lf seconds. which is %lf notes/sec.\nMIDI Stats below\nNotecount: %ld notes from %d tracks\nLength: %d ticks\nPPQ: %d\nplayback is ready\n", parsetime, (double)(totalnotes/parsetime), totalnotes, trackAmount, maxTick, ppq);
    free(writeCursors);
    writeCursors = NULL;
    trackProperties_arr_free(&tracks);
    munmap(filePtr, filestat.st_size);
    filePtr = NULL;
    midiloaded = 1;
    return midiloaded;
}

void UnloadMIDI(void)
{
    if (!midiloaded) return;
    midiloaded = 0;
    trackAmount = 0;
    ppq = 0;
    totalnotes = 0;
    eventcount = 0;
    for (uint32_t i = 0; i < sysexCount; i++)
        free(sysexArr[i].message);
    free(sysexArr);
    free(tempoArr);
    free(eventArr);
    sysexArr = NULL;
    tempoArr = NULL;
    eventArr = NULL;
}
