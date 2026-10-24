#include "windower.h"
#include "renderer.h"
#include "../parse/midistorage.h"
#include "../playback/playback_thread.h"
#include "../playback/timer.h"
#include "../misc/util.h"
#include "../parse/parser.h"
#include "../synth/sound.h"
#include "../third_party/conmidi_digit_formatter.h"
#include "text_renderer.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int dynascroll = 0, uivisible = 1, PAD = 20;;
double scrollfactor = 1.0;
typedef struct
{
    int count;
    const char** filedirs;
} playlist_t;

static void run_playlist(playlist_t* playlist)
{
    int playlist_cnt = playlist->count;
    const char** files = playlist->filedirs;
    for (int i = 0; i < playlist_cnt; i++)
    {
        if (LoadMIDI(files[i]))
        {
            Renderer_InitForMIDI();
            printf("\n%d/%d files played, %s is up next\n", i + 1, playlist_cnt, files[i]);
            StartPlayback(singlethread);
            Renderer_ResetForUnload();
            UnloadMIDI();
        }
    }
    for (int i = 0; i < playlist_cnt; i++)
        free((void*)files[i]);
    free(files);
    free(playlist);
}

static void run_load(char* filepath)
{
    if (LoadMIDI(filepath))
        Renderer_InitForMIDI();
    free(filepath);
}

// only the thread entry signature and spawn call differ per platform, the bodies above are shared
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#define THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
#define THREAD_DONE return 0
static void spawn_thread(LPTHREAD_START_ROUTINE fn, void* arg)
{
    HANDLE handle = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (handle)
        CloseHandle(handle);
}
#else
#include <pthread.h>
#define THREAD_FN(name) static void* name(void* arg)
#define THREAD_DONE return NULL
static void spawn_thread(void* (*fn)(void*), void* arg)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, fn, arg) == 0)
        pthread_detach(thread);
}
#endif

THREAD_FN(playback_thread_entry) { StartPlayback((int)(intptr_t)arg); THREAD_DONE; }
THREAD_FN(playlist_thread_entry) { run_playlist((playlist_t*)arg); THREAD_DONE; }
THREAD_FN(loading_thread_entry)  { run_load((char*)arg); THREAD_DONE; }

static void spawn_playback_thread(int singlethread) { spawn_thread(playback_thread_entry, (void*)(intptr_t)singlethread); }
static void spawn_playlist_thread(playlist_t* playlist) { spawn_thread(playlist_thread_entry, playlist); }
static void spawn_loading_thread(const char* filepath) { spawn_thread(loading_thread_entry, strdup(filepath)); }

static GLFWwindow* win;

static void ensure_synth(void)
{
    if (!issynthinitiated)
        Sound_Init(singlethread);
}

static void glfw_error_callback(int code, const char* desc)
{
    fprintf(stderr, "GLFW error %d: %s\n", code, desc);
}

double delta_time = 0.0, last_time = 0.0;
int fps = 0, vsync = 1;

static void key_callback(GLFWwindow* w, int key, int scancode, int action, int mods)
{
    if (action == GLFW_PRESS)
    {
        if (key == GLFW_KEY_ESCAPE)
            glfwSetWindowShouldClose(w, GLFW_TRUE);
        if (key == GLFW_KEY_SPACE)
        {
            if (stopping)
            {
                ensure_synth();
                spawn_playback_thread(singlethread);
            }
            else
                clock_pause();
        }
        if (key == GLFW_KEY_U)
        {
            stopping = 1;    
            Renderer_ResetForUnload();
            UnloadMIDI();
        }
        if (key == GLFW_KEY_G)
            EnableGlow = !EnableGlow;
        if (key == GLFW_KEY_H)
        {
            uivisible = !uivisible;
            PAD = uivisible? 20 : 0;
        }
        if (key == GLFW_KEY_T)
        {
            EnableTransparency = !EnableTransparency;
            EnableTransparency? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        }
        if (key == GLFW_KEY_C)
        {
            trackcolors = !trackcolors;
            printf("track coloring is %s on next load\n", trackcolors ? "TRUE" : "FALSE");
        }
        if (key == GLFW_KEY_D)
        {
            dynascroll = !dynascroll;
            if (dynascroll) 
            {
                stationarynotes = 0;
                scrollfactor = MAX((int)(WindowTicks / tickscale), 1);
            }
        }
        if (key == GLFW_KEY_S)
        {
            stationarynotes = !stationarynotes;
            if (stationarynotes) 
            {
                dynascroll = 0;
                int64_t m = TimeSig_MeasureAt(current_clock); // the current window, in measures of the current time signature
                scrollfactor = MAX((int)(WindowTicks / MAX(TimeSig_MeasureStart(m + 1) - TimeSig_MeasureStart(m), 1)), 1);
            }
        }

        if (key == GLFW_KEY_V)
        {
            vsync = !vsync;
            glfwSwapInterval(vsync);
        }
        if (key == GLFW_KEY_Q)
            reinitsynths();   
        if (key == GLFW_KEY_R)
            stopping = 1;
    }
    if (action != GLFW_RELEASE)
    {

        
        if (key == GLFW_KEY_RIGHT)
            clock_skip(tickscale);

        if (key == GLFW_KEY_LEFT)
            clock_skip(-tickscale);

        if (key == GLFW_KEY_UP)
        {
            if (dynascroll)
            {
                if (scrollfactor <= 0.5)
                    scrollfactor = MAX(scrollfactor / 2, 0.1);
                else
                    scrollfactor -= 0.1; 
            }
            else if (stationarynotes)
                scrollfactor = MAX(scrollfactor - 1, 1);
            else
                WindowTicks /= 1.1;
        }

        if (key == GLFW_KEY_DOWN)
        {
            if (dynascroll)
            {
                if (scrollfactor <= 0.5)
                    scrollfactor *= 2;
                else
                    scrollfactor += 0.1; 
            }
            else if (stationarynotes)
                scrollfactor += 1;
            else
                WindowTicks = (WindowTicks * 1.1) + 1;   
        }
    }
}

static void drop_callback(GLFWwindow* w, int count, const char** paths)
{
    if (count == 1)
    {
        spawn_loading_thread(paths[0]);
    }
    else
    {
        ensure_synth();
        playlist_t* heap_playlistptr = malloc(sizeof(playlist_t));
        heap_playlistptr->count = count;
        heap_playlistptr->filedirs = malloc(sizeof(char*) * count);
        for (int i = 0; i < count; i++) 
            heap_playlistptr->filedirs[i] = strdup(paths[i]); 
        spawn_playlist_thread(heap_playlistptr);
    }
}

static void fb_resize_callback(GLFWwindow* w, int width, int height)
{
    fbWidth = width;
    fbHeight = height;
    glViewport(0, 0, fbWidth, fbHeight);
}

char filename_truncated[33];
double lastscrollupdate = 0.0;
int filenamepos = 0;
const char* scrollfilenameifover32characters(const char* filename)
{
    if (filename_len <= 32) return filename;

    double now = get_time();
    if ((now - lastscrollupdate) < 0.1) return filename_truncated;
    lastscrollupdate = now;

    for (int i = 0; i < 32; i++)
        filename_truncated[i] = filename[(filenamepos + i) % filename_len];

    filename_truncated[32] = '\0';
    filenamepos = (filenamepos + 1) % filename_len;
    
    return filename_truncated;
}

int Window_Init(void)
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
    {
        fprintf(stderr, "glfwInit failed\n");
        return 0;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    win = glfwCreateWindow(1280, 720, "sharkmidi", NULL, NULL);
    if (!win)
    {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 0;
    }

    glfwMakeContextCurrent(win);
    glfwSwapInterval(0);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        fprintf(stderr, "gladLoadGLLoader failed\n");
        glfwTerminate();
        return 0;
    }

    glfwSetKeyCallback(win, key_callback);

    printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return 1;
}

void Window_Run(const char* filepath)
{
    Renderer_Init();
    Text_Init();
    if (filepath != NULL && LoadMIDI(filepath))
    {
        Renderer_InitForMIDI();
        Sound_Init(singlethread);
        spawn_playback_thread(singlethread);
    }

    double smoothed_fps = 0.0;
    glfwSetDropCallback(win, drop_callback);
    glfwSetFramebufferSizeCallback(win, fb_resize_callback); 
    glfwSwapInterval(vsync);
    last_time = get_time();
    
    while (!glfwWindowShouldClose(win))
    {
        delta_time = get_time() - last_time;
        last_time += delta_time;
        fps = delta_time > 0.0 ? (int)(1.0 / delta_time) : 0;
        double alpha = 1.0 - exp(-delta_time * 2.0);
        smoothed_fps += alpha * (fps - smoothed_fps);

        glClear(GL_COLOR_BUFFER_BIT);

        StationaryBars = MAX((int)scrollfactor, 1);
        if (midiloaded)
            Renderer_Render(current_clock, PAD);

        if (dynascroll)
            WindowTicks = (int)(tickscale * scrollfactor);
        
        UpdatePlaybackStats();

        if (uivisible)
        {
            Text_Queue(5, 4, 1.75f, 0.0f, 1.0f, 0.0f,
                "tick: %s | zoom: %d %s | QoS: %s | bpm: %.2f | fps: %s",
                AddCommas(current_clock), stationarynotes? (int)scrollfactor : WindowTicks, stationarynotes? "bars" : "ticks", AddCommas(NotesDrawnLastFrame), bpm, AddCommas(smoothed_fps));
            Text_Queue(5, fbHeight - 16, 1.75f, 0.0f, 0.7f, 1.0f,
                "%s | notes: %s / %s (%s/s)", scrollfilenameifover32characters(filename), AddCommas(playednotes), AddCommas(totalnotes), AddCommas(notespersec));
            Text_Flush();
        }

        glfwSwapBuffers(win);
        glfwPollEvents();
    }
    stopping = 1;
}

void Window_Shutdown(void)
{
    Sound_Close();
    UnloadMIDI();
    Text_Dispose();
    Renderer_Dispose();    
    glfwDestroyWindow(win);
    glfwTerminate();
}