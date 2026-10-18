#include "render/windower.h"
#include "synth/sound.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char* argv[])
{
    char welcome[] = 
    "========== sharkmidi commit #25 ==========\n"
    "optionally, you could add a file path after the executable for the it to load a midi immediately, like:\n"
    "   ./sharkmidi (single threaded playback) (midi file directory)\n"
    "controls are:\n"   
    "   up = zoom in, down = zoom out\n"
    "   right = seek fwd, left = seek back\n"
    "   space = start/pause playback\n"
    "   r = reset playback\n"
    "   u = unload midi\n"
    "   s = music sheet style\n"
    "   d = dynamic scroll speed\n"
    "   t = note transparency linear to velocity\n"
    "   g = toggle note glow\n"
    "   c = track colors (applies on next load)\n"
    "   v = toggle fps cap\n"
    "   q = reinitialize synth\n"
    "   esc = quit\n"
    "===========================================";
    
    puts(welcome);
    
    setbuf(stdout, NULL); // just for printf to print everytime its called
    singlethread = (argv[1] != NULL && strcmp(argv[1], "false") == 0)? 0 : 1;
    if (!Window_Init())
        return 1;
    Window_Run((argc >= 3)? argv[2] : NULL); // argv[2] only exists when argc >= 3
    Window_Shutdown();
    return 0;
}
