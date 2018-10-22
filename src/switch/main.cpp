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
#include <stdio.h>
#include <switch.h>
#include <vector>

// Deal with conflicting typedefs
#define u64 u64_
#define s64 s64_

#include "../Config.h"
#include "../Savestate.h"
#include "../GPU.h"
#include "../NDS.h"
#include "../SPU.h"
#include "../version.h"

std::string romPath = "sdmc:/";
unsigned int selection;
u8* bufferData;
AudioOutBuffer* releasedBuffer;
AudioOutBuffer buffer;
static Mutex mutex;

u32 swapRedBlue(u32 pixel)
{
    u32 swap;
    swap = (u8)pixel;
    swap <<= 8;
    swap |= (u8)(pixel >> 8);
    swap <<= 8;
    swap |= (u8)(pixel >> 16);
    return swap;
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

    printf("melonDS" MELONDS_VERSION "\n");
    printf(MELONDS_URL "\n\n");

    DIR* directory = opendir(romPath.c_str());
    dirent* entry;
    std::vector<std::string> files;
    while ((entry = readdir(directory)))
    {
        std::string name = entry->d_name;
        if (entry->d_type == DT_DIR || name.find(".nds", (name.length() - 4)) != std::string::npos)
            files.push_back(name);
    }
    closedir(directory);
    std::sort(files.begin(), files.end());
    for (unsigned int i = 0; i < files.size(); i++)
    {
        if (i == selection)
            printf(CONSOLE_WHITE"%s\n", files[i].c_str());
        else
            printf(CONSOLE_RESET"%s\n", files[i].c_str());
    }
    gfxFlushBuffers();
    gfxSwapBuffers();

    while (romPath.find(".nds", (romPath.length() - 4)) == std::string::npos)
    {
        hidScanInput();
        u32 kDown = hidKeysDown(CONTROLLER_P1_AUTO);
        if (kDown & KEY_A)
        {
            romPath += "/" + files[selection];
            selection = 0;
            return main(argc, argv);
        }
        else if (kDown & KEY_UP && selection > 0)
        {
            selection--;
            return main(argc, argv);
        }
        else if (kDown & KEY_DOWN && selection < files.size() - 1)
        {
            selection++;
            return main(argc, argv);
        }
    }

    Config::Load();

    if (!Config::HasConfigFile("bios7.bin") || !Config::HasConfigFile("bios9.bin") || !Config::HasConfigFile("firmware.bin"))
    {
        printf("One or more of the following required files don't exist or couldn't be accessed:");
        printf("bios7.bin -- ARM7 BIOS\n");
        printf("bios9.bin -- ARM9 BIOS\n");
        printf("firmware.bin -- firmware image\n\n");
        printf("Dump the files from your DS and place them in sdmc:/switch/melonds");
        gfxFlushBuffers();
        gfxSwapBuffers();
        while (true);
    }

    NDS::Init();

    if (!NDS::LoadROM(romPath.c_str(), Config::DirectBoot))
    {
        gfxFlushBuffers();
        gfxSwapBuffers();
        while (true);
    }

    consoleClear();
    fclose(stdout);
    gfxConfigureResolution(682, 384);

    audoutInitialize();
    audoutStartAudioOut();

    bufferData = (u8*)memalign(0x1000, 710 * 8);
    buffer.next = NULL;
    buffer.buffer = bufferData;
    buffer.buffer_size = 710 * 8;
    buffer.data_size = 710 * 8;
    buffer.data_offset = 0;

    Thread frameThread;
    threadCreate(&frameThread, advFrame, NULL, 0x80000, 0x30, 1);
    threadStart(&frameThread);

    Thread audioThread;
    threadCreate(&audioThread, playAudio, NULL, 0x80000, 0x30, 3);
    threadStart(&audioThread);

    u32 width, height;
    HidControllerKeys keys[] = { KEY_A, KEY_B, KEY_MINUS, KEY_PLUS, KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_ZR, KEY_ZL, KEY_X, KEY_Y };
    while (true)
    {
        hidScanInput();
        u32 kDown = hidKeysDown(CONTROLLER_P1_AUTO);
        u32 kUp = hidKeysUp(CONTROLLER_P1_AUTO);

        if (kDown & KEY_L || kDown & KEY_R)
        {
            std::string statePath = romPath.substr(0, romPath.rfind(".")) + ".bin";
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

        u32* src = GPU::Framebuffer;
        u32* dst = (u32*)gfxGetFramebuffer(&width, &height);
        mutexLock(&mutex);
        for (int x = 0; x < 256; x++)
            for (int y = 0; y < 384; y++)
                dst[gfxGetFramebufferDisplayOffset(x + 213, y)] = swapRedBlue(src[(y * 256) + x]);
        mutexUnlock(&mutex);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    NDS::DeInit();
    gfxExit();
    audoutExit();
    return 0;
}
