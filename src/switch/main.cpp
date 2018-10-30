/*
    Copyright 2018 Hydr8gon

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <algorithm>
#include <dirent.h>
#include <malloc.h>
#include <fstream>
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

// Deal with conflicting typedefs
#define u64 u64_
#define s64 s64_

#include "../Config.h"
#include "../Savestate.h"
#include "../GPU.h"
#include "../NDS.h"
#include "../SPU.h"
#include "../version.h"

std::vector<std::string> optionDisplay = { "Boot game directly", "Threaded 3D renderer" };
std::vector<std::string> optionEntries = { "DirectBoot", "Threaded3D" };
std::vector<std::string> optionValues = { "1", "1" };
u8* bufferData;
AudioOutBuffer* releasedBuffer;
AudioOutBuffer buffer;
u32* framebuffer;
static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;
static GLuint s_program, s_vao, s_vbo, s_tex;
static GLint loc_tex_diffuse;
static Mutex mutex;

static const char* const vertexShader =
    "#version 330 core\n"
    "precision mediump float;\n"

    "layout (location = 0) in vec3 inPos;\n"
    "layout (location = 1) in vec2 inTexCoord;\n"
    "out vec2 vtxTexCoord;\n"

    "void main()\n"
    "{\n"
        "gl_Position = vec4(inPos, 1.0);\n"
        "vtxTexCoord = inTexCoord;\n"
    "}";

static const char* const fragmentShader =
    "#version 330 core\n"
    "precision mediump float;\n"

    "in vec2 vtxTexCoord;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D tex_diffuse;\n"

    "void main()\n"
    "{\n"
        "fragColor = texture(tex_diffuse, vtxTexCoord);\n"
    "}";

static void initEgl()
{
    EGLConfig config;
    EGLint numConfigs;
    static const EGLint framebufferAttributeList[] = { EGL_RED_SIZE, 1, EGL_GREEN_SIZE, 1, EGL_BLUE_SIZE, 1, EGL_NONE };
    static const EGLint contextAttributeList[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };

    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(s_display, NULL, NULL);
    eglBindAPI(EGL_OPENGL_API);
    eglChooseConfig(s_display, framebufferAttributeList, &config, 1, &numConfigs);
    s_surface = eglCreateWindowSurface(s_display, config, (char*)"", NULL);
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttributeList);
    eglMakeCurrent(s_display, s_surface, s_surface, s_context);
}

static void deinitEgl()
{
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_display, s_context);
    s_context = NULL;
    eglDestroySurface(s_display, s_surface);
    s_surface = NULL;
    eglTerminate(s_display);
    s_display = NULL;
}

static void initRenderer()
{
    typedef struct
    {
        float position[3];
        float texcoord[2];
    } Vertex;

    static const Vertex vertex_list[] =
    {
        { {  0.375f, -1.0f, 0.0f }, { 1.0f, 1.0f } },
        { { -0.375f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
        { { -0.375f,  1.0f, 0.0f }, { 0.0f, 0.0f } },
        { { -0.375f,  1.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  0.375f,  1.0f, 0.0f }, { 1.0f, 0.0f } },
        { {  0.375f, -1.0f, 0.0f }, { 1.0f, 1.0f } }
    };

    GLint vsh = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vsh, 1, &vertexShader, NULL);
    glCompileShader(vsh);

    GLint fsh = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fsh, 1, &fragmentShader, NULL);
    glCompileShader(fsh);

    s_program = glCreateProgram();
    glAttachShader(s_program, vsh);
    glAttachShader(s_program, fsh);
    glLinkProgram(s_program);

    glDeleteShader(vsh);
    glDeleteShader(fsh);

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);

    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_list), vertex_list, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texcoord));
    glEnableVertexAttribArray(1);

    glGenTextures(1, &s_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glUseProgram(s_program);
    loc_tex_diffuse = glGetUniformLocation(s_program, "tex_diffuse");
    glUniform1i(loc_tex_diffuse, 0);
}

static void deinitRenderer()
{
    glDeleteTextures(1, &s_tex);
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_program);
}

void fillAudioBuffer() {
    s16 inBuffer[710 * 2];
    s16* outBuffer = (s16*)bufferData;
    int inSamples = SPU::ReadOutput(inBuffer, 710);
    int outSamples = 1024;

    int margin = 6;
    if (inSamples < 710 - margin)
    {
        int last = inSamples - 1;
        if (last < 0)
            last = 0;

        for (int i = inSamples; i < 710 - margin; i++)
            ((u32*)inBuffer)[i] = ((u32*)inBuffer)[last];

        inSamples = 710 - margin;
    }

    float timer = 0;
    float incr = (float)inSamples / outSamples;
    int pos = 0;
    for (int i = 0; i < 1024; i++)
    {
        outBuffer[i * 2] = inBuffer[pos * 2];
        outBuffer[i * 2 + 1] = inBuffer[pos * 2 + 1];

        timer += incr;
        while (timer >= 1)
        {
            timer--;
            pos++;
        }
    }
}

void advFrame(void* args)
{
    while (true)
    {
        mutexLock(&mutex);
        NDS::RunFrame();
        mutexUnlock(&mutex);
        memcpy(framebuffer, GPU::Framebuffer, 256 * 384 * 4);
    }
}

void playAudio(void* args)
{
    while (true)
    {
        fillAudioBuffer();
        audoutPlayBuffer(&buffer, &releasedBuffer);
    }
}

int main(int argc, char **argv)
{
    gfxInitDefault();
    consoleInit(NULL);

    bool options = false;
    std::string romPath = "sdmc:/";

    std::fstream config;
    config.open("melonds.ini", std::ios::in);
    std::string line;
    while (getline(config, line))
    {
        std::vector<std::string>::iterator iter = std::find(optionEntries.begin(), optionEntries.end(), line.substr(0, line.find("=")));
        if (iter != optionEntries.end())
            optionValues[iter - optionEntries.begin()] = line.substr(line.find("=") + 1);
    }
    config.close();

    while (romPath.find(".nds", (romPath.length() - 4)) == std::string::npos)
    {
        consoleClear();
        printf("melonDS " MELONDS_VERSION "\n");
        printf(MELONDS_URL);

        unsigned int selection = 0;
        std::vector<std::string> files;

        DIR* directory = opendir(romPath.c_str());
        dirent* entry;
        while ((entry = readdir(directory)))
        {
            std::string name = entry->d_name;
            if (entry->d_type == DT_DIR || name.find(".nds", (name.length() - 4)) != std::string::npos)
                files.push_back(name);
        }
        closedir(directory);
        std::sort(files.begin(), files.end());

        while (true)
        {
            hidScanInput();
            u32 kDown = hidKeysDown(CONTROLLER_P1_AUTO);

            if (options)
            {
                if (kDown & KEY_A)
                {
                    optionValues[selection] = optionValues[selection] == "0" ? "1" : "0";
                }
                else if (kDown & KEY_UP && selection > 0)
                {
                    selection--;
                }
                else if (kDown & KEY_DOWN && selection < optionDisplay.size() - 1)
                {
                    selection++;
                }
                else if (kDown & KEY_X)
                {
                    config.open("melonds.ini", std::ios::out);
                    for (unsigned int i = 0; i < optionDisplay.size(); i++)
                        config << optionEntries[i] + "=" + optionValues[i] + "\n";
                    config.close();

                    options = false;
                    break;
                }

                for (unsigned int i = 0; i < optionDisplay.size(); i++)
                {
                    if (i == selection)
                    {
                        printf(CONSOLE_WHITE"\x1b[%d;1H%s", i + 4, optionDisplay[i].c_str());
                        printf(CONSOLE_WHITE"\x1b[%d;30H%s", i + 4, optionValues[i] == "0" ? "Off" : "On ");
                    }
                    else
                    {
                        printf(CONSOLE_RESET"\x1b[%d;1H%s", i + 4, optionDisplay[i].c_str());
                        printf(CONSOLE_RESET"\x1b[%d;30H%s", i + 4, optionValues[i] == "0" ? "Off" : "On ");
                    }
                }
                printf(CONSOLE_RESET"\x1b[45;1HPress X to return to the file browser.");
            }
            else
            {
                if (kDown & KEY_A && files.size() > 0)
                {
                    romPath += "/" + files[selection];
                    break;
                }
                else if (kDown & KEY_B && romPath != "sdmc:/")
                {
                    romPath = romPath.substr(0, romPath.rfind("/"));
                    break;
                }
                else if (kDown & KEY_UP && selection > 0)
                {
                    selection--;
                }
                else if (kDown & KEY_DOWN && selection < files.size() - 1)
                {
                    selection++;
                }
                else if (kDown & KEY_X)
                {
                    options = true;
                    break;
                }

                for (unsigned int i = 0; i < files.size(); i++)
                {
                    if (i == selection)
                        printf(CONSOLE_WHITE"\x1b[%d;1H%s", i + 4, files[i].c_str());
                    else
                        printf(CONSOLE_RESET"\x1b[%d;1H%s", i + 4, files[i].c_str());
                }
                printf(CONSOLE_RESET"\x1b[45;1HPress X to open the options menu.");
            }

            gfxFlushBuffers();
            gfxSwapBuffers();
        }
    }

    Config::Load();

    if (!Config::HasConfigFile("bios7.bin") || !Config::HasConfigFile("bios9.bin") || !Config::HasConfigFile("firmware.bin"))
    {
        consoleClear();
        printf("One or more of the following required files don't exist or couldn't be accessed:");
        printf("bios7.bin -- ARM7 BIOS\n");
        printf("bios9.bin -- ARM9 BIOS\n");
        printf("firmware.bin -- firmware image\n\n");
        printf("Dump the files from your DS and place them in sdmc:/switch/melonds");
        while (true)
        {
            gfxFlushBuffers();
            gfxSwapBuffers();
        }
    }

    NDS::Init();

    std::string sramPath = romPath.substr(0, romPath.rfind(".")) + ".sav";
    std::string statePath = romPath.substr(0, romPath.rfind(".")) + ".mln";

    if (!NDS::LoadROM(romPath.c_str(), sramPath.c_str(), Config::DirectBoot))
    {
        consoleClear();
        printf("Failed to load ROM. Make sure the file can be accessed.");
        while (true)
        {
            gfxFlushBuffers();
            gfxSwapBuffers();
        }
    }

    consoleClear();
    fclose(stdout);
    initEgl();
    gladLoadGL();
    initRenderer();

    Thread frameThread;
    threadCreate(&frameThread, advFrame, NULL, 0x80000, 0x30, 1);
    threadStart(&frameThread);

    audoutInitialize();
    audoutStartAudioOut();

    bufferData = (u8*)memalign(0x1000, 710 * 8);
    buffer.next = NULL;
    buffer.buffer = bufferData;
    buffer.buffer_size = 710 * 8;
    buffer.data_size = 710 * 8;
    buffer.data_offset = 0;

    Thread audioThread;
    threadCreate(&audioThread, playAudio, NULL, 0x80000, 0x30, 0);
    threadStart(&audioThread);

    framebuffer = (u32*)memalign(0x1000, 256 * 384 * 4);

    HidControllerKeys keys[] = { KEY_A, KEY_B, KEY_MINUS, KEY_PLUS, KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_ZR, KEY_ZL, KEY_X, KEY_Y };
    while (true)
    {
        hidScanInput();
        u32 kDown = hidKeysDown(CONTROLLER_P1_AUTO);
        u32 kUp = hidKeysUp(CONTROLLER_P1_AUTO);

        if (kDown & KEY_L || kDown & KEY_R)
        {
            Savestate* state = new Savestate(const_cast<char*>(statePath.c_str()), kDown & KEY_L);
            mutexLock(&mutex);
            NDS::DoSavestate(state);
            mutexUnlock(&mutex);
            delete state;
        }

        for (int i = 0; i < 12; i++)
        {
            if (kDown & keys[i])
                NDS::PressKey(i > 9 ? i + 6 : i);
            else if (kUp & keys[i])
                NDS::ReleaseKey(i > 9 ? i + 6 : i);
        }

        if (hidTouchCount() > 0)
        {
            touchPosition touch;
            hidTouchRead(&touch, 0);

            if (touch.px > 400 && touch.px < 880 && touch.py > 360)
            {
                NDS::PressKey(16 + 6);
                NDS::TouchScreen((touch.px - 400) / 1.875, (touch.py - 360) / 1.875);
            }
        }
        else
        {
            NDS::ReleaseKey(16 + 6);
            NDS::ReleaseScreen();
        }

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 384, 0, GL_BGRA, GL_UNSIGNED_BYTE, framebuffer);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        eglSwapBuffers(s_display, s_surface);
    }

    NDS::DeInit();
    audoutExit();
    deinitRenderer();
    deinitEgl();
    gfxExit();
    return 0;
}
