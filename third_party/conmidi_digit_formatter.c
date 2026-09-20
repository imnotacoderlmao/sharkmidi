#include <stdint.h>
#include <stdio.h>
#include <string.h>

// how can memory leak if its already allocated??? checkmate liberulz,,,,,
char totalnotes_str[24] = "0", playednotes_str[24] = "0";
char curr_tick_str[24] = "0", midifps_str[24] = "0";
char rendererfps_str[24] = "0", quad_on_screen_str[24] = "0";
char maxtick_str[24] = "0", trackamount_str[24] = "0";
char loadedtracks_str[24] = "0", play_nps_str[24] = "0";

void __attribute__((noinline)) AddCommas(int64_t n, char output[24]) 
{
    char temp[24];
    sprintf(temp, "%ld", n);
    
    int len = strlen(temp);
    int commas = (len - 1) / 3;
    int new_len = len + commas;
    output[new_len] = '\0';

    int i = len - 1;
    int j = new_len - 1;
    int count = 0;

    while (i >= 0) 
    {
        if (count == 3 && (temp[i] >= 0x30 && temp[i] < 0x40)) 
        {
            output[j--] = ',';
            count = 0;
        }
        output[j--] = temp[i--];
        count++;
    }
}