#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* AddCommas(int64_t n) {
    char temp[50];
    sprintf(temp, "%ld", n);

    int len = strlen(temp);
    int commas = (len - 1) / 3;
    int new_len = len + commas;
    char* output = malloc((new_len + 1) * sizeof(char));
    output[new_len] = '\0';

    int i = len - 1;
    int j = new_len - 1;
    int count = 0;

    while (i >= 0) {
        if (count == 3 && temp[i] != '-') {
            output[j--] = ',';
            count = 0;
        }
        output[j--] = temp[i--];
        count++;
    }
    return output;
}