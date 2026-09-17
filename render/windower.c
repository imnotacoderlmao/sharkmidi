#include "windower.h"
#include "renderer.h"
#include "../playback/playback_thread.h"
#include "../playback/timer.h"
#include "../parse/parser.h"
#include "../third_party/glad/include/glad/glad.h"
#include "../synth/sound.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int singlethread = 1;
typedef struct
{
    int count;
    const char** filedirs;
} playlist_t;

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>

static HANDLE playbackThread = NULL;
static HANDLE playlistThread = NULL;

static DWORD WINAPI playback_thread_entry(LPVOID arg)
{
    StartPlayback((int)(intptr_t)arg);
    return 0;
}

static void spawn_playback_thread(int singlethread)
{
    if (playbackThread)
        CloseHandle(playbackThread);
    playbackThread = CreateThread(NULL, 0, playback_thread_entry, (LPVOID)(intptr_t)singlethread, 0, NULL);
}

static DWORD WINAPI playlist_thread_entry(LPVOID arg)
{
    playlist_t* playlist = (playlist_t*)arg;
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
    return 0;
}

static void spawn_playlist_thread(playlist_t* playlist)
{
    if (playlistThread)
        CloseHandle(playlistThread);
    playlistThread = CreateThread(NULL, 0, playlist_thread_entry, (LPVOID)playlist, 0, NULL);
}

static void join_playback_thread(void)
{
    if (playbackThread)
    {
        WaitForSingleObject(playbackThread, 1000);
        CloseHandle(playbackThread);
        playbackThread = NULL;
    }
}

#else
#include <pthread.h>

static pthread_t playbackThread;
static pthread_t playlistThread;

static void* playback_thread_entry(void* arg)
{
    StartPlayback((int)(intptr_t)arg);
    return NULL;
}

static void spawn_playback_thread(int singlethread)
{
    pthread_create(&playbackThread, NULL, playback_thread_entry, (void*)(intptr_t)singlethread);
}

static void* playlist_thread_entry(void* arg)
{
    playlist_t* playlist = (playlist_t*)arg;
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
    return NULL;
}

static void spawn_playlist_thread(playlist_t* playlist)
{
    pthread_create(&playlistThread, NULL, playlist_thread_entry, (void*)playlist);
}

static void join_playback_thread(void)
{
    pthread_join(playbackThread, NULL);
}
#endif

static GLFWwindow* win;

static void glfw_error_callback(int code, const char* desc)
{
    fprintf(stderr, "GLFW error %d: %s\n", code, desc);
}

double delta_time = 0.0, last_time = 0.0;

static void limitframerateto(int target_fps)
{
    if (target_fps <= 0 || target_fps > 1000) return;

    static double next_frame_time = 0.0;
    double frame_duration = 1.0 / (double)target_fps;
    double now = get_time();
    double remainder = next_frame_time - now;
    if (next_frame_time == 0.0 || now > next_frame_time + (frame_duration * 4.0))
        next_frame_time = now;

    while (remainder > 0)
    {
        now = get_time();
        remainder = next_frame_time - now;
        int ms_to_sleep = (int)((remainder - 0.002) * 1000.0);
        if (ms_to_sleep > 0)
            os_sleep_ms(ms_to_sleep);
    }
    next_frame_time += frame_duration;
}

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
                if (!issynthinitiated) 
                    Sound_Init(singlethread);
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
    }
    if (key == GLFW_KEY_RIGHT)
        clock_skip(tickscale, 0);

    if (key == GLFW_KEY_LEFT)
        clock_skip(tickscale * -1, 0);

    if (key == GLFW_KEY_UP)
        WindowTicks /= 1.1;

    if (key == GLFW_KEY_DOWN)
        WindowTicks *= 1.1;   
    
    if (key == GLFW_KEY_R)
        stopping = 1;
}

void drop_callback(GLFWwindow* w, int count, const char** paths)
{
    if (count == 1)
    {
        LoadMIDI(paths[0]);
        Renderer_InitForMIDI();
    }
    else
    {
        if (!issynthinitiated) 
            Sound_Init(singlethread);
        playlist_t* heap_playlistptr = malloc(sizeof(playlist_t));
        heap_playlistptr->count = count;
        heap_playlistptr->filedirs = malloc(sizeof(char*) * count);
        for (int i = 0; i < count; i++) 
            heap_playlistptr->filedirs[i] = strdup(paths[i]); 
        spawn_playlist_thread(heap_playlistptr);
    }
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

    return 1;
}

void Window_Run(char* filepath)
{
    Renderer_Init();
    
    if (filepath != NULL && LoadMIDI(filepath))
    {
        Renderer_InitForMIDI();
        Sound_Init(singlethread);
        spawn_playback_thread(singlethread);
    }

    const int PAD = 20; // placeholder until an actual gui exists
    glfwSetDropCallback(win, drop_callback);
    last_time = get_time();
    while (!glfwWindowShouldClose(win))
    {
        delta_time = get_time() - last_time;
        last_time += delta_time;
        
        int fbWidth, fbHeight;
        glfwGetFramebufferSize(win, &fbWidth, &fbHeight);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (midiloaded)
            Renderer_Render(fbWidth, fbHeight, current_clock, PAD);

        glfwSwapBuffers(win);
        glfwPollEvents();
        limitframerateto(60);
    }

    stopping = 1;
    join_playback_thread();
}

void Window_Shutdown(void)
{
    Renderer_Dispose();
    UnloadMIDI();
    glfwDestroyWindow(win);
    glfwTerminate();
}