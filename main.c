#include "parse/parser.h"
#include "playback/playback_thread.h"
#include "render/windower.h"
#include "synth/sound.h"
#include <stdio.h>
#include <string.h>
int main(int32_t argc, char* argv[])
{
    puts("===== sharkmidi commit #8? =====\noptionally, you could add a file path after the executable for the it to load a midi immediately, like:\n ./sharkmidi (midi directory) (single threaded playback)");
    #if defined(_WIN32) || defined(_WIN64)
        puts("you are using a windows build. for some reason windows builds on memory mapping d NOt like filesizes over 2gb");
    #endif
    puts("===============================");
    
    setbuf(stdout, NULL); // just for printf to print everytime its called
    int32_t singlethread = (argv[2] != NULL && strcmp(argv[2], "true") == 0)? 1 : 0;
    Window_Init();
    Window_Run(argv[1]);
    Window_Shutdown();
    return 0;
}