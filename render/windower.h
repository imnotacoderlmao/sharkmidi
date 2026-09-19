#ifndef window_hdr
#define window_hdr
extern int singlethread;
int Window_Init();
void Window_Run(const char* filepath);
void Window_Shutdown(void);
#endif