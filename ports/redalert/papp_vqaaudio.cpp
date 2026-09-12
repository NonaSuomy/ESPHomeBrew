// Movie (VQA) audio for the Red Alert PAPP (replaces common/vqaaudio_null.cpp).
//
// Follows common/vqaaudio_openal.cpp, with one voice of the game's sound mixer
// (papp_sound.cpp) in place of the OpenAL source: the movie player decodes
// audio into a ring of HMIBufSize blocks, and every frame (VQA_CopyAudio) the
// played blocks are replaced by the next loaded ones. Movie timing comes from
// the clock, not from the audio, as in the OpenAL build.
#include "papp_port.h"

#include "common/audio.h"
#include "common/soundio_imp.h"
#include "common/vqaaudio.h"
#include "common/vqaconfig.h"
#include "common/vqafile.h"
#include "common/vqaloader.h"
#include "common/vqatask.h"

#include <string.h>

int AudioFlags;
int TimerIntCount;
int TimerMethod;
int VQATickCount;
int TickOffset;
unsigned VQAAudioPaused;
VQAHandle* AudioVQAHandle;

enum
{
    VQA_BUFFER_COUNT = 2, // blocks queued when a movie starts, like OpenAL's two buffers
};

// Kept for the whole game: the mixer has a fixed number of voices.
static SampleTrackerTypeImp* s_voice = nullptr;

static SampleTrackerTypeImp* Movie_Voice(const VQAAudio* audio)
{
    if (s_voice == nullptr) {
        s_voice = SoundImp_Init_Sample(audio->BitsPerSample, audio->Channels > 1, audio->SampleRate);
    }
    return s_voice;
}

// Queue the block at PlayPosition and advance to the next one when it is loaded.
static bool Queue_Audio()
{
    VQAConfig* config = &AudioVQAHandle->Config;
    VQAAudio* audio = &AudioVQAHandle->VQABuf->Audio;

    if (s_voice == nullptr) {
        return false;
    }

    SoundImp_Set_Sample_Attributes(s_voice, audio->BitsPerSample, audio->Channels > 1, audio->SampleRate);
    SoundImp_Buffer_Sample_Data(s_voice, &audio->Buffer[audio->PlayPosition], config->HMIBufSize);
    audio->field_B8 = audio->field_B0;
    audio->field_B0 += config->HMIBufSize;

    if (audio->field_B0 >= audio->BuffBytes) {
        audio->field_B0 = 0;
    }

    audio->field_14 = audio->field_10 + 1;

    if (audio->field_14 >= audio->NumAudBlocks) {
        audio->field_14 = 0;
    }

    if (audio->IsLoaded[audio->field_14] != 1) {
        if (VQAMovieDone) {
            ++audio->field_B4;
        }

        ++audio->NumSkipped;
        config->DrawFlags &= 0xFB;

        return false;
    }

    audio->IsLoaded[audio->field_10] = 0;
    audio->PlayPosition += config->HMIBufSize;
    ++audio->field_10;

    if (audio->PlayPosition >= config->AudioBufSize) {
        audio->PlayPosition = 0;
        audio->field_10 = 0;
    }

    ++audio->field_B4;
    return true;
}

static void VQA_AudioCallback()
{
    if (VQAAudioPaused || AudioVQAHandle == nullptr || s_voice == nullptr
        || !(AudioFlags & VQA_AUDIO_FLAG_AUDIO_DMA_TIMER)) {
        return;
    }
    if (SoundImp_Get_Sample_Free_Buffer_Count(s_voice) > 0) {
        Queue_Audio();
    }
    // The mixer stops a voice that runs dry (a slow frame); pick it up again.
    if (!SoundImp_Sample_Status(s_voice)) {
        SoundImp_Start_Sample(s_voice);
    }
}

int VQA_StartTimerInt(VQAHandle* handle, int a2)
{
    (void)a2;
    VQAAudio* audio = &handle->VQABuf->Audio;

    if (!(AudioFlags & VQA_AUDIO_FLAG_INTERRUPT_TIMER)) {
        AudioFlags |= VQA_AUDIO_FLAG_UNKNOWN016;
    }

    audio->Flags |= VQA_AUDIO_FLAG_UNKNOWN016;
    ++TimerIntCount;
    return 0;
}

void VQA_StopTimerInt(VQAHandle* handle)
{
    (void)handle;
    if (TimerIntCount > 0) {
        --TimerIntCount;
    }

    AudioFlags &= ~VQA_AUDIO_FLAG_INTERRUPT_TIMER;
}

int VQA_OpenAudio(VQAHandle* handle, void* hwnd)
{
    (void)hwnd;
    VQAConfig* config = &handle->Config;
    VQAHeader* header = &handle->Header;
    VQAAudio* audio = &handle->VQABuf->Audio;

    Start_Primary_Sound_Buffer(true);

    audio->field_10 = 0;
    audio->field_BC = 0;

    if (config->AudioRate == -1) {
        if (header->FPS == config->FrameRate) {
            config->AudioRate = audio->SampleRate;
        } else {
            config->AudioRate = config->FrameRate * audio->SampleRate / header->FPS;
        }
    }

    audio->field_C0 = 1;
    audio->Flags |= VQA_AUDIO_FLAG_UNKNOWN001;
    AudioFlags |= VQA_AUDIO_FLAG_UNKNOWN001;
    return 0;
}

void VQA_CloseAudio(VQAHandle* handle)
{
    VQAAudio* audio = &handle->VQABuf->Audio;

    VQA_StopAudio(handle);
    AudioFlags &= ~(VQA_AUDIO_FLAG_UNKNOWN004 | VQA_AUDIO_FLAG_UNKNOWN008);
    audio->Flags &= ~(VQA_AUDIO_FLAG_UNKNOWN004 | VQA_AUDIO_FLAG_UNKNOWN008);
    audio->field_C0 = 0;
    audio->field_BC = 0;
    audio->Flags &= ~(VQA_AUDIO_FLAG_UNKNOWN001 | VQA_AUDIO_FLAG_UNKNOWN002);
    AudioFlags &= ~(VQA_AUDIO_FLAG_UNKNOWN001 | VQA_AUDIO_FLAG_UNKNOWN002 | VQA_AUDIO_FLAG_AUDIO_DMA_TIMER);
}

int VQA_StartAudio(VQAHandle* handle)
{
    VQAConfig* config = &handle->Config;
    VQAAudio* audio = &handle->VQABuf->Audio;

    AudioVQAHandle = handle;

    if (AudioFlags & VQA_AUDIO_FLAG_AUDIO_DMA_TIMER) {
        return -1; // already started
    }

    SampleTrackerTypeImp* voice = Movie_Voice(audio);
    if (voice == nullptr) {
        return -1; // no sound: the movie plays silently
    }
    SoundImp_Stop_Sample(voice);

    audio->BuffBytes = config->HMIBufSize * 4;
    audio->field_B0 = 0;
    audio->field_B4 = 0;

    for (unsigned i = 0; i < VQA_BUFFER_COUNT; ++i) {
        Queue_Audio();
    }

    SoundImp_Set_Sample_Volume(voice, (unsigned)config->Volume * 256);
    SoundImp_Start_Sample(voice);

    audio->Flags |= VQA_AUDIO_FLAG_AUDIO_DMA_TIMER;
    AudioFlags |= VQA_AUDIO_FLAG_AUDIO_DMA_TIMER;
    return 0;
}

void VQA_StopAudio(VQAHandle* handle)
{
    VQAAudio* audio = &handle->VQABuf->Audio;

    if (AudioFlags & VQA_AUDIO_FLAG_AUDIO_DMA_TIMER) {
        if (s_voice != nullptr) {
            SoundImp_Stop_Sample(s_voice);
        }
        audio->Flags &= ~VQA_AUDIO_FLAG_AUDIO_DMA_TIMER;
        AudioFlags &= ~VQA_AUDIO_FLAG_AUDIO_DMA_TIMER;
    }

    AudioVQAHandle = nullptr;
}

void VQA_PauseAudio()
{
    if (AudioVQAHandle != nullptr && s_voice != nullptr && (AudioFlags & VQA_AUDIO_FLAG_AUDIO_DMA_TIMER)
        && !VQAAudioPaused) {
        SoundImp_Stop_Sample(s_voice);
        VQAAudioPaused = VQA_GetTime(AudioVQAHandle);
    }
}

void VQA_ResumeAudio()
{
    if (AudioVQAHandle != nullptr && s_voice != nullptr && (AudioFlags & VQA_AUDIO_FLAG_AUDIO_DMA_TIMER)
        && VQAAudioPaused) {
        TickOffset -= VQA_GetTime(AudioVQAHandle) - VQAAudioPaused;
        VQAAudioPaused = 0;
    }
}

int VQA_CopyAudio(VQAHandle* handle)
{
    VQAConfig* config = &handle->Config;
    VQAAudio* audio = &handle->VQABuf->Audio;

    VQA_AudioCallback();

    if (config->OptionFlags & VQAOPTF_AUDIO && audio->Buffer != nullptr && audio->TempBufSize > 0) {
        int current_block = audio->AudBufPos / config->HMIBufSize;
        int next_block = (audio->TempBufSize + audio->AudBufPos) / config->HMIBufSize;

        if ((unsigned)next_block >= audio->NumAudBlocks) {
            next_block -= audio->NumAudBlocks;
        }

        if (audio->IsLoaded[next_block] == 1) {
            return -10;
        }

        if (next_block < current_block) {
            // Wraps around the ring.
            int end_space = config->AudioBufSize - audio->AudBufPos;
            int remaining = audio->TempBufSize - end_space;
            memcpy(&audio->Buffer[audio->AudBufPos], audio->TempBuf, end_space);
            memcpy(audio->Buffer, &audio->TempBuf[end_space], remaining);
            audio->AudBufPos = remaining;
            audio->TempBufSize = 0;

            for (unsigned i = current_block; i < audio->NumAudBlocks; ++i) {
                audio->IsLoaded[i] = 1;
            }

            for (int i = 0; i < next_block; ++i) {
                audio->IsLoaded[i] = 1;
            }
        } else {
            memcpy(&audio->Buffer[audio->AudBufPos], audio->TempBuf, audio->TempBufSize);
            audio->AudBufPos += audio->TempBufSize;
            audio->TempBufSize = 0;

            for (int i = current_block; i < next_block; ++i) {
                audio->IsLoaded[i] = 1;
            }
        }
    }
    return 0;
}

void VQA_SetTimer(VQAHandle* handle, int time, int method)
{
    if (method == -1) {
        if (AudioFlags & VQA_AUDIO_FLAG_AUDIO_DMA_TIMER) {
            method = VQA_AUDIO_TIMER_METHOD_DMA;
        } else if (AudioFlags & (VQA_AUDIO_FLAG_UNKNOWN016 | VQA_AUDIO_FLAG_UNKNOWN032)) {
            method = VQA_AUDIO_TIMER_METHOD_INTERRUPT;
        } else {
            method = VQA_AUDIO_TIMER_METHOD_DOS;
        }
    } else {
        if (!(AudioFlags & VQA_AUDIO_FLAG_AUDIO_DMA_TIMER) && method == 3) {
            method = VQA_AUDIO_TIMER_METHOD_INTERRUPT;
        }

        if (!(AudioFlags & (VQA_AUDIO_FLAG_UNKNOWN016 | VQA_AUDIO_FLAG_UNKNOWN032))
            && method == VQA_AUDIO_TIMER_METHOD_INTERRUPT) {
            method = VQA_AUDIO_TIMER_METHOD_DOS;
        }
    }

    TimerMethod = method;
    TickOffset = 0;
    TickOffset = time - VQA_GetTime(handle);
}

// 60 Hz ticks.
unsigned VQA_GetTime(VQAHandle* handle)
{
    (void)handle;
    return unsigned(TickOffset + 60 * (papp_time_us() / 1000) / 1000);
}

int VQA_TimerMethod()
{
    return TimerMethod;
}
