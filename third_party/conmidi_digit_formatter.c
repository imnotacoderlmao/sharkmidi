#include <stdlib.h>
#include <stdint.h>
static int count_digits(int64_t n) {
    int digits = 1;
    while (n >= 10) {
        n /= 10;
        digits++;
    }
    return digits;
}

char* AddCommas(int64_t num) {
    int digits = count_digits(num);
    int commas = (digits - 1) / 3;
    int len_with_commas = digits + commas;

    char* result = (char*)malloc(len_with_commas + 1);

    result[len_with_commas] = '\0';

    int i = digits - 1;
    int j = len_with_commas - 1;
    int k = 0;

    while (i >= 0) {
        unsigned long long digit = num % 10;
        num /= 10;
        result[j--] = '0' + (char)digit;
        k++;
        if (k == 3 && i != 0) {
            result[j--] = ',';
            k = 0;
        }
        i--;
    }

    return result;
}