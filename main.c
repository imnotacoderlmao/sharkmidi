#include "parse/parser.h"
#include "playback/playback_thread.h"
#include "render/windower.h"
#include "synth/sound.h"
#include <stdio.h>
#include <string.h>
int main(int32_t argc, char* argv[])
{
    puts("===== sharkmidi commit #17 =====");
    puts("   optionally, you could add a file path after the executable for the it to load a midi immediately, like:\n   ./sharkmidi (single threaded playback) (midi directory)");
    puts("   controls are:\n   up = zoom in, down = zoom out\n   right = seek fwd, left = seek back\n   space = start/pause playback\n   r = reset playback\n   u = unload midi\n   s = music sheet style\n   d = dynamic scroll speed\n   t = note transparency linear to velocity");
    puts("================================");
    
    setbuf(stdout, NULL); // just for printf to print everytime its called
    singlethread = (argv[1] != NULL && strcmp(argv[1], "false") == 0)? 0 : 1;
    Window_Init();
    // windows only builds crashes due to trying to access a pointer that dosent exist in my testing, otherwise id omit the argc check
    Window_Run((argc >= 3)? argv[2] : NULL);
    Window_Shutdown();
    return 0;
}