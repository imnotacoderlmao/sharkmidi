#ifndef text_renderer_hdr
#define text_renderer_hdr
void Text_Init(void);
void Text_Dispose(void);
void Text_Queue(float x, float y, float scale, float r, float g, float b, const char* fmt, ...);
void Text_Flush(void);
#endif