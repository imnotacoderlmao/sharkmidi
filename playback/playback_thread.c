#include "../parse/midistorage.h"
#include "timer.h"
#include "../synth/sound.h"
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
int64_t playednotes = 0, playednotes2 = 0;
int32_t current_clock = 0, frames = 0;
double now = 0.0, last = 0.0;
double bpm = 120.0;
double tickscale = 0.0;
double tick = 0.0;
int32_t paused = 0, stopping = 0, skipping = 0;
char sysex_str[64];
const double STALL_THRESH = 0.0166667;
// oh god do i really have to abuse macros too
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

void clock_start(void)
{
    now = get_time();
    last = now;
    bpm = 120.0;
    tick = 0.0;
    tickscale = (bpm * ppq) / 60.0;
    paused = 0;
}

double clock_getTick(void)
{
    now = get_time();
    double delta = MIN(now - last, STALL_THRESH);
    last = now;
    tick += delta * tickscale;
    return tick;
}

void clock_pause(void)
{
    if(!paused)
    {
        clock_getTick();
        paused = 1;
        AllNotesOFF();
    }
}

void clock_resume(void)
{
    if(paused)
    {
        last = clock_getTick();
        paused = 0;
    }
}

void SetBPM(uint24_t microsec)
{
    bpm = 60000000.0 / uint24_get(&microsec);
    tickscale = (bpm * ppq) / 60.0;
}


void* PlaybackStats(void* arg)
{
    printf("starting playback....");
    double lastupdate = 0;
    double notespersec = 0.0;
    double midifps = 0;
    while(!stopping)
    {
        double now = get_time();
        double delta = now - lastupdate;
        if (current_clock >= maxTick) 
            stopping = 1;
        if(delta > 0.1)
        {
            notespersec = (playednotes - playednotes2) / delta;
            midifps = frames / delta;
            playednotes2 = playednotes;
            lastupdate = now;
            frames = 0;
        }
        if(voicefetching)
            printf("tick: %d / %d | played notes: %ld / %ld (%lf/s) | bpm: %lf | midi thread: %lf | %d voices        \r", current_clock, maxTick, playednotes, totalnotes, notespersec, bpm, midifps, GetVoiceCount());
        else
            printf("tick: %d / %d | played notes: %ld / %ld (%lf/s) | bpm: %lf | midi thread: %lf        \r", current_clock, maxTick, playednotes, totalnotes, notespersec, bpm, midifps);
        
        usleep(16 * 1000);
    }
    pthread_exit(NULL);
    return NULL;
}

void SubmitSysEx(SysExEvent sysex)
{
    size_t offset = 0;
    for(int32_t i = 0; i < sysex.size; i++)
        offset += snprintf(sysex_str + offset, 4, "%02X " , sysex.message[i]);
    printf("\nSending Sysex Message: %s", sysex_str);
    #if defined(_WIN32) || defined(_WIN64)
        MIDIHDR header = 
        {
            sysex.message,
            (uint32_t)sysex.size * sizeof(uint8_t),
            (uint32_t)sysex.size * sizeof(uint8_t),
            0
        };
        uint32_t size = (uint32_t)sizeof(MIDIHDR);
        uint32_t send = 255, unprepare = 255;
        uint32_t prepare = PrepareLongData(&header, size);
        if (prepare == 0)
        {
            send = SendDirectLongData(&header, size);
            if (send == 0)
            {
                while (UnprepareLongData(&header, size) == 65) // MIDIERR_STILLPLAYING
                    sleep(1);
                unprepare = 0;
            }
        }
        if (prepare != 0 || send != 0 || unprepare != 0)
            printf("sysex prepare,send,unprepare returned (%d,%d,%d)", prepare, send, unprepare);
    #else
        uint32_t send = SendDirectLongData(sysex.message, sysex.size * sizeof(uint8_t));
        if (send != 0)
            printf("sysex send returned (%d)", send);
    #endif
}

void StartPlayback(int singlethread)
{
    stopping = 0;
    playednotes = 0, playednotes2 = 0;
    uint24_t* eventptr = eventArr;
    uint24_t* currev = eventptr;
    TickGroup* timing = timingArr;
    TempoEvent* tempo = tempoArr;
    int32_t tempoidx = 0;
    uint16_t rb_write = 0;
    SysExEvent* sysex = sysexArr;
    int32_t sysexidx = 0;
    current_clock = 0;
    pthread_t stats_thread;
    pthread_create(&stats_thread, NULL, PlaybackStats, NULL);
    clock_start();
    while(!stopping)
    {
        int32_t clock = (int32_t)clock_getTick();
        frames++;
        if (current_clock > clock)
        {
            while (timing->tick > clock && clock > 0)
            {
                timing--;
                currev = eventptr + timing->event_offset;
                playednotes -= timing->notecount;
            }
            while (tempo[tempoidx].tick > clock && tempoidx > 0) 
                tempoidx--;
            while (sysex[sysexidx].tick > clock && sysexidx > 0) 
                sysexidx--;
        }
        while (timing->tick < clock)
        {
            current_clock = timing->tick;
            if (!skipping)
            {
                uint24_t* targetMsg = eventptr + timing->event_offset;
                if (__builtin_expect(!singlethread, 1))
                {
                    size_t count = targetMsg - currev;\
                    size_t copied = 0;
                    while (copied < count)
                    {
                        size_t remaining = count - copied;
                        size_t free = RINGBUFFER_SIZE - rb_write;
                        size_t chunk = MIN(remaining, free);
                        memcpy(ringbuffer + rb_write, currev + copied, chunk * sizeof(uint24_t));
                        rb_write = (rb_write + chunk) & RINGBUFFER_MASK;
                        copied += chunk;
                    }
                }
                else
                { 
                    while (currev < targetMsg)
                        SendDirectData((uint32_t)uint24_get(currev++));
                }
            }
            else
                currev = eventptr + timing->event_offset;
            playednotes += timing->notecount;
            timing++;
        }
        while (tempo[tempoidx].tick <= clock)
        {
            SetBPM(tempo[tempoidx].microsec);
            tempoidx++;
        }
        while (sysex[sysexidx].tick <= clock)
        {
            SubmitSysEx(sysex[sysexidx]);
            sysexidx++;
        }
    }
    clock_start();
    current_clock = 0;
    pthread_join(stats_thread, NULL);
    puts("\nPlayback finished...");
}