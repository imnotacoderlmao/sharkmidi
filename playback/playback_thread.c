#include "../parse/midistorage.h"
#include "../parse/parser.h"
#include "../misc/util.h"
#include "timer.h"
#include "../synth/sound.h"
#include "../third_party/conmidi_digit_formatter.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
int64_t playednotes = 0, playednotes2 = 0, notespersec = 0;
double now = 0.0, last = 0.0, delta = 0.0, tick = 0.0;
double laststatsupdate = 0.0, bpm = 120.0, tickscale = 0.0;
volatile int paused = 0, stopping = 1;
int64_t current_clock = 0;
static const double onesixtyh = 0.016666666;
static atomic_int playback_running = 0;
static _Atomic int64_t pending_skip = 0; // seeks requested by the UI, consumed by the playback thread

static TempoEvent* g_tempo_cursor = NULL;

void clock_start(void)
{
    last = get_time();
    bpm = 120.0;
    tick = 0.0;
    current_clock = 0;
    tickscale = (bpm * ppq) / 60.0;
    paused = 0;
    atomic_store(&pending_skip, 0);
}

void SetBPM(int32_t microsec)
{
    if (microsec <= 0) return; // a zero tempo would make tickscale infinite
    bpm = 60000000.0 / microsec;
    tickscale = (bpm * ppq) / 60.0;
}

// tempo events are sorted, so walk the cursor to the first event past `tick` (works in both directions)
static void resync_tempo_cursor(void)
{
    while (g_tempo_cursor > tempoArr && g_tempo_cursor->tick > tick)
        g_tempo_cursor--;
    if (g_tempo_cursor->tick > tick)
        SetBPM(500000); // seeked before the first tempo event, back to the MIDI default instead of keeping a stale bpm
    while (g_tempo_cursor->tick <= tick)
    {
        SetBPM(g_tempo_cursor->microsec);
        g_tempo_cursor++;
    }
}

double clock_getTick(void)
{
    int64_t skip = atomic_exchange(&pending_skip, 0);
    if (skip)
    {
        tick = MAX(0.0, tick + (double)skip);
        resync_tempo_cursor();
    }
    if (paused) { os_sleep_ms(1); return tick; }
    now = get_time();
    double remaining = MIN(now - last, onesixtyh);
    delta = now - last;
    last = now;
    // for tick to not jump too much when you have microsecond tempo changes or something
    while (remaining > 0.0)
    {
        double ticks_to_next_tempo = g_tempo_cursor->tick - tick;
        double time_to_next_tempo = ticks_to_next_tempo / tickscale;
        if (time_to_next_tempo > remaining)
        {
            tick += remaining * tickscale;
            break;
        }
        if (ticks_to_next_tempo > 0.0)
            tick = g_tempo_cursor->tick;
        remaining -= time_to_next_tempo;
        SetBPM(g_tempo_cursor->microsec);
        g_tempo_cursor++;
    }
    return tick;
}

// called from the UI thread, the actual seek happens in clock_getTick so the tempo cursor has a single owner
void clock_skip(double ticks)
{
    atomic_fetch_add(&pending_skip, (int64_t)ticks);
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

TickGroup* timing = NULL;
double midifps = 0;
double smoothed_nps = 0.0, smoothed_midifps = 0.0;

static void update_playback_stats(void)
{   
    double noww = get_time();
    double dt = noww - laststatsupdate;
    
    if (dt < onesixtyh || stopping)
        return;
        
    if (tick >= maxTick) 
        stopping = 1;

    if (tick < ppq && (timing + 1)->tick > (ppq * 10))
        tick = (timing + 1)->tick - ppq;
        
    midifps = delta > 0.0 ? 1.0 / delta : 0.0;
    playednotes = timing->notecount;

    int64_t delta_notes = playednotes - playednotes2;
    double instant_nps = (double)delta_notes / dt;
    playednotes2 = playednotes;

    double alpha = 1.0 - exp(-dt * 2.0); // equivalent to dividing by 0.5 for a 0.5s window
    smoothed_nps += alpha * (instant_nps - smoothed_nps);
    smoothed_midifps += alpha * (midifps - smoothed_midifps);
    notespersec = (int64_t)smoothed_nps;

    if (voicefetching)
    {
        printf("tick: %s / %s | notes: %s / %s (%s/s) | bpm: %.2lf | midi thread: %10s fps | %s voices    \r", 
            AddCommas(current_clock), AddCommas(maxTick), AddCommas(playednotes), AddCommas(totalnotes), 
            AddCommas(notespersec), bpm, AddCommas(smoothed_midifps), AddCommas(KDMAPI_GetVoiceCount()));  
    }
    else
    {
        printf("tick: %s / %s | notes: %s / %s (%s/s) | bpm: %.2lf | midi thread: %10s fps    \r", 
            AddCommas(current_clock), AddCommas(maxTick), AddCommas(playednotes), AddCommas(totalnotes), 
            AddCommas(notespersec), bpm, AddCommas(smoothed_midifps));
    }
    laststatsupdate = noww;
}

// called from both the playback thread and the render loop, whoever loses the race just skips this update - Copy
static atomic_flag stats_busy = ATOMIC_FLAG_INIT;
void UpdatePlaybackStats(void)
{
    if (atomic_flag_test_and_set_explicit(&stats_busy, memory_order_acquire))
        return;
    update_playback_stats();
    atomic_flag_clear_explicit(&stats_busy, memory_order_release);
}

void SubmitSysEx(SysExEvent sysex)
{
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
        uint32_t prepare = KDMAPI_PrepareLongData(&header, size);
        if (prepare == 0)
        {
            send = KDMAPI_SendDirectLongData(&header, size);
            // always unprepare once prepared, even if the send failed
            while ((unprepare = KDMAPI_UnprepareLongData(&header, size)) == 65 && send == 0) // MIDIERR_STILLPLAYING
                os_sleep_ms(1);
        }
        if (prepare != 0 || send != 0 || unprepare != 0)
            printf("sysex prepare,send,unprepare returned (%d,%d,%d)", prepare, send, unprepare);
    #else
        uint32_t send = KDMAPI_SendDirectLongData(sysex.message, sysex.size * sizeof(uint8_t));
        if (send != 0)
            printf("sysex send returned (%d)", send);
    #endif
}

void Playback_StopAndWait(void)
{
    stopping = 1;
    while (atomic_load(&playback_running))
        os_sleep_ms(1);
}

void StartPlayback(int singlethread)
{
    if (!midiloaded)
    {
        puts("no midi loaded!!!!!!!");
        return;
    }
    if (!issynthinitiated) { puts("no synth initiated!!!!!!!"); return; }
    if (atomic_exchange(&playback_running, 1))
        return; // already playing
    playednotes = 0, playednotes2 = 0;
    uint24_t* eventptr = eventArr;
    timing = timingArr;
    g_tempo_cursor = tempoArr;
    SysExEvent* sysex = sysexArr;
    int64_t played = 0;
    clock_start();
    // stopping drops to 0 only after everything the stats/render side dereferences is set up
    stopping = 0;
    if (!singlethread)
        audiothread_entry();
    while(!stopping)
    {
        int64_t clock = (int64_t)clock_getTick();
        if (clock < current_clock)
        {
            while (timing > timingArr && timing->tick > clock)
                timing--;
            played = timing->event_offset;
            while (sysex > sysexArr && sysex->tick > clock)
                sysex--;
            if (!singlethread)
                memset(ringbuffer, 0, RINGBUFFER_SIZE * sizeof(uint24_t));
        }
        while (timing->tick <= clock)
        {
            int64_t count = (timing + 1)->event_offset;
            if (!singlethread)
            {
                while (played < count)
                {
                    int64_t rb_write = played & RINGBUFFER_MASK;
                    int64_t chunk = MIN(count - played, (int64_t)RINGBUFFER_SIZE - rb_write);
                    memcpy(ringbuffer + rb_write, eventptr + played, chunk * sizeof(uint24_t));
                    played += chunk;
                }
                atomic_store_explicit(&writeptr, (uint32_t)(played & RINGBUFFER_MASK), memory_order_release);
            }
            else
            { 
                while (played < count)
                    KDMAPI_SendDirectData((uint32_t)uint24_get(eventptr + played++));
            }        
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
    }
    if (!singlethread)
        audiothread_join();
    clock_start();
    AllNotesOFF();
    uint8_t rolandreset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
    SubmitSysEx((SysExEvent){0, 11, rolandreset});    
    notespersec = 0, smoothed_nps = 0;
    puts("\nPlayback finished...");
    atomic_store(&playback_running, 0);
}
