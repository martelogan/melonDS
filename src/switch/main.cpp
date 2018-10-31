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

using namespace std;

vector<string> optionDisplay = { "Boot game directly", "Threaded 3D renderer", "Screen rotation", "Screen layout" };
vector<string> optionEntries = { "DirectBoot", "Threaded3D", "ScreenRotation", "ScreenLayout" };
vector<vector<string>> optionValuesDisplay = { { "Off", "On " }, { "Off", "On " }, { "0  ", "90 ", "180", "270" }, { "Natural   ", "Vertical  ", "Horizontal" } };
vector<unsigned int> optionValues = { 1, 1, 0, 0 };
u8* bufferData;
AudioOutBuffer* releasedBuffer;
AudioOutBuffer buffer;
u32* framebuffer;
unsigned int touchBoundLeft, touchBoundRight, touchBoundTop, touchBoundBottom;
static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;
static GLuint s_program, s_vao, s_vbo;
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
    float width, height, offsetTopX, offsetBotX, offsetTopY, offsetBotY;
    if (optionValues[3] == 0)
        optionValues[3] = (optionValues[2] % 2 == 0) ? 1 : 2;
    if (optionValues[3] == 1)
    {
        height = 1.0f;
        if (optionValues[2] % 2 == 0)
            width = height * 0.75;
        else
            width = height * 0.421875;
        offsetTopX = offsetBotX = -width / 2;
        offsetTopY = height;
        offsetBotY = 0.0f;
    }
    else
    {
        if (optionValues[2] % 2 == 0)
        {
            width = 1.0f;
            height = width / 0.75;
        }
        else
        {
            height = 2.0f;
            width = height * 0.421875;
        }
        offsetTopX = -width;
        offsetBotX = 0.0f;
        offsetTopY = offsetBotY = height / 2;
    }

    typedef struct
    {
        float position[3];
        float texcoord[2];
    } Vertex;

    static Vertex screenLayout[] =
    {
        { { offsetTopX + width, offsetTopY - height, 0.0f }, { 1.0f, 1.0f } },
        { { offsetTopX,         offsetTopY - height, 0.0f }, { 0.0f, 1.0f } },
        { { offsetTopX,         offsetTopY,          0.0f }, { 0.0f, 0.0f } },
        { { offsetTopX,         offsetTopY,          0.0f }, { 0.0f, 0.0f } },
        { { offsetTopX + width, offsetTopY,          0.0f }, { 1.0f, 0.0f } },
        { { offsetTopX + width, offsetTopY - height, 0.0f }, { 1.0f, 1.0f } },

        { { offsetBotX + width, offsetBotY - height, 0.0f }, { 1.0f, 1.0f } },
        { { offsetBotX,         offsetBotY - height, 0.0f }, { 0.0f, 1.0f } },
        { { offsetBotX,         offsetBotY,          0.0f }, { 0.0f, 0.0f } },
        { { offsetBotX,         offsetBotY,          0.0f }, { 0.0f, 0.0f } },
        { { offsetBotX + width, offsetBotY,          0.0f }, { 1.0f, 0.0f } },
        { { offsetBotX + width, offsetBotY - height, 0.0f }, { 1.0f, 1.0f } },
    };

    if (optionValues[2] == 1 || optionValues[2] == 2)
    {
        Vertex* copy = (Vertex*)memalign(0x1000, sizeof(screenLayout));
        memcpy(copy, screenLayout, sizeof(screenLayout));
        memcpy(screenLayout, &copy[6], sizeof(screenLayout) / 2);
        memcpy(&screenLayout[6], copy, sizeof(screenLayout) / 2);
    }

    touchBoundLeft = (screenLayout[8].position[0] + 1) * 640;
    touchBoundRight = (screenLayout[6].position[0] + 1) * 640;
    touchBoundTop = (-screenLayout[8].position[1] + 1) * 360;
    touchBoundBottom = (-screenLayout[6].position[1] + 1) * 360;

    for (unsigned int i = 0; i < optionValues[2]; i++)
    {
        for (int j = 0; j < 2; j++)
        {
            for (int k = 0; k < 12; k += 6)
            {
                screenLayout[k].position[j] = screenLayout[k + 1].position[j];
                screenLayout[k + 1].position[j] = screenLayout[k + 2].position[j];
                screenLayout[k + 2].position[j] = screenLayout[k + 4].position[j];
                screenLayout[k + 3].position[j] = screenLayout[k + 4].position[j];
                screenLayout[k + 4].position[j] = screenLayout[k + 5].position[j];
                screenLayout[k + 5].position[j] = screenLayout[k].position[j];
            }
        }
    }

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
    glBufferData(GL_ARRAY_BUFFER, sizeof(screenLayout), screenLayout, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texcoord));
    glEnableVertexAttribArray(1);

    glActiveTexture(GL_TEXTURE0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glUseProgram(s_program);
}

static void deinitRenderer()
{
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
    string romPath = "sdmc:/";

    fstream config;
    config.open("melonds.ini", ios::in);
    string line;
    while (getline(config, line))
    {
        vector<string>::iterator iter = find(optionEntries.begin(), optionEntries.end(), line.substr(0, line.find("=")));
        if (iter != optionEntries.end())
            optionValues[iter - optionEntries.begin()] = stoi(line.substr(line.find("=") + 1));
    }
    config.close();

    while (romPath.find(".nds", (romPath.length() - 4)) == string::npos)
    {
        consoleClear();
        printf("melonDS " MELONDS_VERSION "\n");
        printf(MELONDS_URL);

        unsigned int selection = 0;
        vector<string> files;

        DIR* directory = opendir(romPath.c_str());
        dirent* entry;
        while ((entry = readdir(directory)))
        {
            string name = entry->d_name;
            if (entry->d_type == DT_DIR || name.find(".nds", (name.length() - 4)) != string::npos)
                files.push_back(name);
        }
        closedir(directory);
        sort(files.begin(), files.end());

        while (true)
        {
            hidScanInput();
            u32 kDown = hidKeysDown(CONTROLLER_P1_AUTO);

            if (options)
            {
                if (kDown & KEY_A)
                {
                    optionValues[selection]++;
                    if (optionValues[selection] > optionValuesDisplay[selection].size() - 1)
                        optionValues[selection] = 0;
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
                    config.open("melonds.ini", ios::out);
                    for (unsigned int i = 0; i < optionDisplay.size(); i++)
                        config << optionEntries[i] + "=" + to_string(optionValues[i]) + "\n";
                    config.close();

                    options = false;
                    break;
                }

                for (unsigned int i = 0; i < optionDisplay.size(); i++)
                {
                    if (i == selection)
                    {
                        printf(CONSOLE_WHITE"\x1b[%d;1H%s", i + 4, optionDisplay[i].c_str());
                        printf(CONSOLE_WHITE"\x1b[%d;30H%s", i + 4, optionValuesDisplay[i][optionValues[i]].c_str());
                    }
                    else
                    {
                        printf(CONSOLE_RESET"\x1b[%d;1H%s", i + 4, optionDisplay[i].c_str());
                        printf(CONSOLE_RESET"\x1b[%d;30H%s", i + 4, optionValuesDisplay[i][optionValues[i]].c_str());
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

    string sramPath = romPath.substr(0, romPath.rfind(".")) + ".sav";
    string statePath = romPath.substr(0, romPath.rfind(".")) + ".mln";

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

            if (touch.px > touchBoundLeft && touch.px < touchBoundRight && touch.py > touchBoundTop && touch.py < touchBoundBottom)
            {
                int x, y;
                NDS::PressKey(16 + 6);
                if (optionValues[2] == 0)
                {
                    x = (touch.px - touchBoundLeft) * 256.0f / (touchBoundRight - touchBoundLeft);
                    y = (touch.py - touchBoundTop) * 256.0f / (touchBoundRight - touchBoundLeft);
                }
                else if (optionValues[2] == 1)
                {
                    x = (touch.py - touchBoundTop) * 192.0f / (touchBoundRight - touchBoundLeft);
                    y = 192 - (touch.px - touchBoundLeft) * 192.0f / (touchBoundRight - touchBoundLeft);
                }
                else if (optionValues[2] == 2)
                {
                    x = (touch.px - touchBoundLeft) * -256.0f / (touchBoundRight - touchBoundLeft);
                    y = 192 - (touch.py - touchBoundTop) * 256.0f / (touchBoundRight - touchBoundLeft);
                }
                else
                {
                    x = (touch.py - touchBoundTop) * -192.0f / (touchBoundRight - touchBoundLeft);
                    y = (touch.px - touchBoundLeft) * 192.0f / (touchBoundRight - touchBoundLeft);
                }
                NDS::TouchScreen(x, y);
            }
        }
        else
        {
            NDS::ReleaseKey(16 + 6);
            NDS::ReleaseScreen();
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, framebuffer);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, &framebuffer[256 * 192]);
        glDrawArrays(GL_TRIANGLES, 6, 12);
        eglSwapBuffers(s_display, s_surface);
    }

    NDS::DeInit();
    audoutExit();
    deinitRenderer();
    deinitEgl();
    gfxExit();
    return 0;
}
