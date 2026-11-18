#include "parse/parser.h"
#include "playback/playback_thread.h"
#define TIMER_IMPL
#ifdef NOT_HEADLESS
    #include "render/windower.h"
#else
    #include "synth/sound.h"
#endif
#include "playback/timer.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char* argv[])
{
    char welcome[] = 
    "========== sharkmidi commit #40 ==========\n"
#ifdef USE_TSC_CLOCK
    "you are using a build of sharkmidi that uses __rdtsc() for time fetches. playback might drift a little.\n"
#endif
#ifndef NOT_HEADLESS
    "you are using a build of sharkmidi thats headless, hopefully for people that run in pentium 3's\n"
#endif
    "optionally, you could add a file path after the executable for the it to load a midi immediately, like:\n"
    "   ./sharkmidi (single threaded playback) (midi file directory)\n"
    "controls are:\n"   
#ifdef NOT_HEADLESS
    "   up = zoom in, down = zoom out\n"
    "   right = seek fwd, left = seek back\n"
    "   space = start/pause playback\n"
    "   r = reset playback\n"
    "   b = toggle buffered sending\n"
    "   u = unload midi\n"
    "   s = music sheet style\n"
    "   d = dynamic scroll speed\n"
    "   t = note transparency linear to velocity\n"
    "   g = toggle note glow\n"
    "   c = track colors (applies on next load)\n"
    "   v = toggle fps cap\n"
    "   q = reinitialize synth\n"
#else
    "   ctrl+C = quit\n"
    "   ctrl+Z = pause (resume depends on your terminal's way of continuing a job)\n"
#endif
    "   esc = quit\n"
    "===========================================";
    
    puts(welcome);
    timer_calibrate();
    setbuf(stdout, NULL); // just for printf to print everytime its called
#ifdef NOT_HEADLESS
    if (!Window_Init())
        return 1;
    Window_Run((argc >= 3)? argv[2] : NULL); // argv[2] only exists when argc >= 3
    Window_Shutdown();
#else
    if (argc < 2) { puts("headless mode, please input a file after the executable!"); return 0; }
    LoadMIDI(argv[1]);
    int singlethread = (argc >= 3) && strcmp(argv[2], "true") == 0? 1 : 0;
    Sound_Init(singlethread);
    StartPlayback(singlethread);
    UnloadMIDI();
    Sound_Close();
#endif
    return 0;
}
