#include "parse/parser.h"
#include "playback/playback_thread.h"
#include "synth/sound.h"
#include <stdio.h>
#include <string.h>
int main(int32_t argc, char* argv[])
{
    if(argv[1] == NULL)
    {
        puts("executable {filedir} bool{singlethread (default=false)}");
        return 1;
    }
    #if defined(_WIN32) || defined(_WIN64)
        puts("you are using a windows build. for some reason windows builds on memory mapping d NOt like filesizes over 2gb")
    #endif
    
    setbuf(stdout, NULL); // just for printf to print everytime its called
    int32_t singlethread = (argv[2] != NULL && strcmp(argv[2], "true") == 0)? 1 : 0;
    if(LoadMIDI(argv[1]))
    {
        if(!Sound_Init(singlethread))
            return 1;
        StartPlayback(singlethread);
        UnloadMIDI();
    }
    return 0;
}