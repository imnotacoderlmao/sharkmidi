#include "midistorage.h"
#include <stddef.h>
TickGroup* timingArr = NULL;
uint24_t* eventArr = NULL;
uint8_t* trackArr = NULL;
SysExEvent* sysexArr = NULL;
TempoEvent* tempoArr = NULL;
uint64_t totalnotes = 0;
uint32_t ppq = 0;
uint32_t tempoCount = 0;
uint32_t sysexCount = 0;
int32_t maxTick = 0;
char* filename = "to load a midi, drag and drop one to the window";