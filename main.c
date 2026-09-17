#include "parse/parser.h"
#include "playback/playback_thread.h"
#include "render/windower.h"
#include "synth/sound.h"
#include <stdio.h>
#include <string.h>
int main(int32_t argc, char* argv[])
{
    puts("===== sharkmidi commit #10 =====");
    puts("   optionally, you could add a file path after the executable for the it to load a midi immediately, like:\n   ./sharkmidi (midi directory) (single threaded playback)");
    puts("   controls are:\n   up = zoom in, down = zoom out\n   right = seek fwd, left = seek back\n   u = unload midi");
    puts("================================");
    
    setbuf(stdout, NULL); // just for printf to print everytime its called
    singlethread = (argv[2] != NULL && strcmp(argv[2], "false") == 0)? 0 : 1;
    Window_Init();
    Window_Run(argv[1]);
    Window_Shutdown();
    return 0;
}