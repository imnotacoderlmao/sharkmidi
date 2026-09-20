#include "../parse/midistorage.h"
#include "../parse/parser.h"
#include "timer.h"
#include "../synth/sound.h"
#include "../third_party/conmidi_digit_formatter.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
int64_t playednotes = 0, playednotes2 = 0, notespersec = 0;
double now = 0.0, last = 0.0, delta = 0.0, tick = 0.0;
double laststatsupdate = 0.0, bpm = 120.0, tickscale = 0.0;
volatile int paused = 0, stopping = 1;
int skipping = 0;
int64_t npshistory[60];
int npshistoryidx = 0;
int32_t current_clock = 0;
const double STALL_THRESH = 0.0166667;
// oh god do i really have to abuse macros too
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define INFINITY 1.7976931348623157e+308

void clock_start(void)
{
    now = get_time();
    last = now;
    bpm = 120.0;
    tick = 0.0;
    current_clock = 0;
    tickscale = (bpm * ppq) / 60.0;
    paused = 0;
}

static TempoEvent* g_tempo_cursor = NULL; 
void SetBPM(uint24_t microsec)
{
    bpm = 60000000.0 / uint24_get(&microsec);
    tickscale = (bpm * ppq) / 60.0;
}

double clock_getTick(void)
{
    if (paused) { os_sleep_ms(1); return tick; }
    now = get_time();
    double remaining = MIN(now - last, STALL_THRESH);
    delta = now - last;
    last = now;
    // for tick to not jump too much when you have microsecond tempo changes or something
    while (remaining > 0.0)
    {
        double ticks_to_next_tempo = g_tempo_cursor->tick - tick;
        double time_to_next_tempo = ticks_to_next_tempo / tickscale;
        double step_time = MIN(remaining, time_to_next_tempo);
        tick += step_time * tickscale;
        remaining -= step_time;

        if (step_time == time_to_next_tempo)
        {
            SetBPM(g_tempo_cursor->microsec);
            g_tempo_cursor++;
        }
        else 
            break;
    }
    return tick;
}

void clock_skip(double skiptick, int skipto)
{
    double tickafterskip = MAX(0, tick + skiptick);
    tick = skipto ? skiptick : tickafterskip;
    // resync tempo cursor, this will be done both ways
    while (g_tempo_cursor > tempoArr && g_tempo_cursor->tick > tick)
        g_tempo_cursor--;
    while (g_tempo_cursor->tick <= tick && g_tempo_cursor->tick != INT32_MAX)
    {
        SetBPM(g_tempo_cursor->microsec);
        g_tempo_cursor++;
    }
}

void clock_pause(void)
{
    if(!paused)
    {
        clock_getTick();
        paused = 1;
        AllNotesOFF();
    }
    else 
    {
        last = get_time();
        paused = 0;
    }
}

double midifps = 0;
void UpdatePlaybackStats()
{   
    if ((now - laststatsupdate) < 0.01666666)
        return;
    if (tick >= maxTick) 
        stopping = 1;
    
    midifps = 1 / delta;

    npshistoryidx = (npshistoryidx + 1) % 60;
    notespersec -= npshistory[npshistoryidx];
    npshistory[npshistoryidx] = playednotes - playednotes2;
    notespersec += npshistory[npshistoryidx];
    playednotes2 = playednotes;

    AddCommas(tick, curr_tick_str);
    AddCommas(playednotes, playednotes_str);
    AddCommas(notespersec, play_nps_str);
    AddCommas(midifps, midifps_str);
    if(voicefetching)
        printf("tick: %s / %s | played notes: %s / %s (%s/s) | bpm: %.2lf | midi thread: %s fps | %d voices        \r", curr_tick_str, maxtick_str, playednotes_str, totalnotes_str, play_nps_str, bpm, midifps_str, GetVoiceCount());
    else
        printf("tick: %s / %s | played notes: %s / %s (%s/s) | bpm: %.2lf | midi thread: %s fps        \r", curr_tick_str, maxtick_str, playednotes_str, totalnotes_str, play_nps_str, bpm, midifps_str);
    laststatsupdate = now;
}

void SubmitSysEx(SysExEvent sysex)
{
    char sysex_str[64];
    size_t offset = 0;
    offset += snprintf(sysex_str + offset, 3, "%02X" , sysex.message[0]);
    for(int32_t i = 1; i < sysex.size; i++)
        offset += snprintf(sysex_str + offset, 4, "-%02X" , sysex.message[i]);
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
                    os_sleep_ms(1);
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
    if (!midiloaded)
    {
        puts("no midi loaded!!!!!!!");
        return;
    }
    stopping = 0;
    if (!singlethread)
        audiothread_entry();
    uint24_t* eventptr = eventArr;
    TickGroup* timing = timingArr;
    g_tempo_cursor = tempoArr;
    SysExEvent* sysex = sysexArr;
    size_t played = 0;
    clock_start();
    while(!stopping)
    {
        int32_t clock = (int32_t)clock_getTick();
        if (clock < current_clock)
        {
            while (timing > timingArr && timing->tick > clock)
            {
                timing--;
                played = timing->event_offset;
                playednotes -= timing->notecount;
            }
            while (sysex > sysexArr && sysex->tick > clock)
                sysex--;
        }
        while (timing->tick <= clock)
        {
            if (!skipping)
            {
                size_t count = (timing + 1)->event_offset;
                if (!singlethread)
                {
                    while (played < count)
                    {
                        size_t rb_write = played & RINGBUFFER_MASK;
                        size_t chunk = MIN(count - played, RINGBUFFER_SIZE - rb_write);
                        memcpy(ringbuffer + rb_write, eventptr + played, chunk * sizeof(uint24_t));
                        played += chunk;
                    }
                    writeptr = played & RINGBUFFER_MASK;
                }
                else
                { 
                    while (played < count)
                        SendDirectData((uint32_t)uint24_get(eventptr + played++));
                }        
            }
            else
                played = (timing + 1)->event_offset;
            playednotes += timing->notecount;
            // this feels frankenstein, but its a way for smooth scrolling under throttle
            current_clock = timing->tick;
            UpdatePlaybackStats();
            timing++;
        }
        while (sysex->tick <= clock)
        {
            SubmitSysEx(*sysex);
            sysex++;
        }
        current_clock = clock;
        UpdatePlaybackStats();
    }
    clock_start();
    AllNotesOFF();
    uint8_t rolandreset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
    SubmitSysEx((SysExEvent){0, 11, rolandreset});
    playednotes = 0, playednotes2 = 0;
    puts("\nPlayback finished...");
}