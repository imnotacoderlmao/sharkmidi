#ifndef PARSER_H
#define PARSER_H
#include <stdint.h>
int32_t LoadMIDI(const char* filepath);
void UnloadMIDI(void);
extern int32_t midiloaded, trackcolors;
#endif
