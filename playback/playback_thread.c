#include "../parse/midistorage.h"
#include "../parse/parser.h"
#include "../misc/util.h"
#include "timer.h"
#include "../synth/sound.h"
#include "../third_party/conmidi_digit_formatter.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

int64_t playednotes = 0, playednotes2 = 0, notespersec = 0;
double now = 0.0, last = 0.0, delta = 0.0, tick = 0.0;
double laststatsupdate = 0.0, bpm = 120.0, tickscale = 0.0;
volatile int paused = 0, stopping = 1;
int64_t current_clock = 0;
#define onesixtyh 0.016666666
static atomic_int playback_running = 0;
_Atomic double playback_next_due = 0.0;
static _Atomic int64_t pending_skip = 0;

static TempoEvent* g_tempo_cursor = NULL;
#define MAX_SLEEP_US 2000
static double spin_margin = DEFAULT_SPIN_US * 1e-6;

static void wait_for_next_tick(int64_t clock)
{
    if (tickscale > MAX_TPS_FOR_SPIN) 
        return;
    double dt = ((double)(clock + 1) - tick) / tickscale;
    double to_tempo = (g_tempo_cursor->tick - tick) / tickscale;
    if (to_tempo < dt) 
        dt = to_tempo;
    if (dt > MAX_SLEEP_US * 1e-6) 
        dt = MAX_SLEEP_US * 1e-6;
    atomic_store_explicit(&playback_next_due, last + dt, memory_order_relaxed);
    double wake = last + dt - spin_margin;
    if (wake - get_time() > 0.0)
        os_sleep_until(wake);
    else
        os_cpu_relax();
}

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
    bpm = 60000000.0 / microsec;
    tickscale = (bpm * ppq) / 60.0;
}

static void resync_tempo_cursor(void)
{
    while (g_tempo_cursor > tempoArr && g_tempo_cursor->tick > tick)
        g_tempo_cursor--;
    if (g_tempo_cursor->tick > tick)
        SetBPM(500000);
    while (g_tempo_cursor->tick <= tick)
    {
        SetBPM(g_tempo_cursor->microsec);
        g_tempo_cursor++;
    }
}

double clock_getTick(void)
{
    if (pending_skip)
    {
        tick = MAX(0.0, tick + (double)pending_skip);
        resync_tempo_cursor();
        pending_skip = 0;
    }
    now = get_time();
    delta = now - last;
    last = now;
    if (paused) { os_sleep_ms(1); return tick; }
    double remaining = MIN(delta, onesixtyh);
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

    if (tick < tickscale && (timing + 1)->tick > (tickscale * 10))
        tick = (timing + 1)->tick - ppq;
        
    midifps = delta > 0.0 ? 1.0 / delta : 0.0;
    playednotes = timing->notecount;

    int64_t delta_notes = playednotes - playednotes2;
    double instant_nps = (double)delta_notes / dt;
    playednotes2 = playednotes;

    double alpha = 1.0 - exp(-dt * 2.0);
    smoothed_nps += alpha * (instant_nps - smoothed_nps);
    smoothed_midifps += alpha * (midifps - smoothed_midifps);
    notespersec = (int64_t)smoothed_nps;

    printf("tick: %s / %s | notes: %s / %s (%s/s) | bpm: %.2lf | midi thread: %10s fps",
        AddCommas(current_clock), AddCommas(maxTick), AddCommas(playednotes), AddCommas(totalnotes),
        AddCommas(notespersec), bpm, AddCommas(smoothed_midifps));
    if (voicefetching)
        printf(" | %s voices", AddCommas(KDMAPI_GetVoiceCount()));
    fputs("    \r", stdout);
    laststatsupdate = noww;
}

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
            (uint32_t)sysex.size,
            (uint32_t)sysex.size,
            0
        };
        uint32_t size = (uint32_t)sizeof(MIDIHDR);
        uint32_t send = 255, unprepare = 255;
        uint32_t prepare = KDMAPI_PrepareLongData(&header, size);
        if (prepare == 0)
        {
            send = KDMAPI_SendDirectLongData(&header, size);
            while ((unprepare = KDMAPI_UnprepareLongData(&header, size)) == 65 && send == 0)
                os_sleep_ms(1);
        }
        if (prepare != 0 || send != 0 || unprepare != 0)
            printf("sysex prepare,send,unprepare returned (%d,%d,%d)\n", prepare, send, unprepare);
    #else
        uint32_t send = KDMAPI_SendDirectLongData(sysex.message, sysex.size);
        if (send != 0)
            printf("sysex send returned (%d)\n", send);
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
        return;
    playednotes = 0, playednotes2 = 0;
    const uint8_t* eventptr = eventArr;
    timing = timingArr;
    g_tempo_cursor = tempoArr;
    SysExEvent* sysex = sysexArr;
    int64_t played = 0;
    clock_start();
    os_timer_init();
    if (getenv("SHARKMIDI_SPIN_US"))
        spin_margin = atof(getenv("SHARKMIDI_SPIN_US")) * 1e-6;
    stopping = 0;
    if (!singlethread)
        audiothread_entry();
    while(!stopping)
    {
        int64_t clock = (int64_t)clock_getTick();
        if (clock == current_clock) { wait_for_next_tick(clock); continue; }
        if (clock < current_clock)
        {
            while (timing > timingArr && timing->tick > clock)
                timing--;
            played = timing->event_offset;
            while (sysex > sysexArr && sysex->tick > clock)
                sysex--;
            if (!singlethread)
                Sound_Flush();
        }
        while (timing->tick <= clock)
        {
            int64_t count = (timing + 1)->event_offset;
            if (!singlethread)
                Sound_Send(eventptr + played, eventptr + count);
            else
                EV_RUNS(eventptr + played, eventptr + count, st, third, key, const uint32_t head = st | (third << 16);
                        ,KDMAPI_SendDirectData(head | (key << 8)););

            played = count;
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
    notespersec = 0, smoothed_nps = 0, playednotes = 0, playednotes2 = 0;
    puts("\nPlayback finished...");
    atomic_store(&playback_running, 0);
}