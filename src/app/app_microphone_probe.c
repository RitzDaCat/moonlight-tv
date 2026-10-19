#include "app_microphone_probe.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if TARGET_WEBOS
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static FILE *probe_output_open(void) {
#if TARGET_WEBOS
    const char *home = getenv("HOME");
    char path[4096];
    if (home == NULL) {
        return NULL;
    }
    const int length = snprintf(path, sizeof(path), "%s/conf/microphone-probe.log", home);
    if (length < 0 || (size_t) length >= sizeof(path)) {
        return NULL;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        SDL_Log("MIC_PROBE diagnostic_file_open_failed");
        return NULL;
    }
    /* This contains probe metadata only. The SDK reader has a different UID
     * from the application, so a private 0600 log cannot be collected. */
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        fchmod(fd, 0644) != 0 || ftruncate(fd, 0) != 0) {
        close(fd);
        return NULL;
    }
    FILE *output = fdopen(fd, "w");
    if (output == NULL) {
        close(fd);
    }
    return output;
#else
    return NULL;
#endif
}

static void probe_log(FILE *output, const char *format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    SDL_Log("%s", message);
    if (output != NULL) {
        fprintf(output, "%s\n", message);
        fflush(output);
    }
}

static bool excluded_input(const char *name) {
    return SDL_strcasecmp(name, "default") == 0 ||
           SDL_strcasecmp(name, "precord") == 0 ||
           SDL_strcasecmp(name, "pvoipsource") == 0 ||
           SDL_strstr(name, ".monitor") != NULL ||
           SDL_strncasecmp(name, "Monitor of ", 11) == 0;
}

int app_microphone_probe(const char *device_name) {
    FILE *output = probe_output_open();
    const bool own_audio = (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0;
    if (own_audio && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        probe_log(output, "MIC_PROBE audio_init_failed: %s", SDL_GetError());
        if (output != NULL) {
            fclose(output);
        }
        return 1;
    }

    int result = 0;
    SDL_AudioDeviceID device = 0;
    const int count = SDL_GetNumAudioDevices(1);
    const char *driver = SDL_GetCurrentAudioDriver();
    probe_log(output, "MIC_PROBE driver=%s capture_devices=%d", driver ? driver : "unknown", count);
    if (count < 0) {
        probe_log(output, "MIC_PROBE enumeration_failed: %s", SDL_GetError());
        result = 1;
        goto finish;
    }

    int matches = 0;
    for (int i = 0; i < count; ++i) {
        const char *name = SDL_GetAudioDeviceName(i, 1);
        if (name == NULL) {
            continue;
        }
        probe_log(output, "MIC_PROBE input[%d]=%s excluded=%d", i, name, excluded_input(name));
        if (device_name != NULL && SDL_strcmp(name, device_name) == 0) {
            ++matches;
        }
    }
    if (device_name == NULL) {
        probe_log(output, "MIC_PROBE enumeration_only; no input opened; physical_input_verified=false");
        goto finish;
    }
    if (excluded_input(device_name) || matches != 1) {
        probe_log(output, "MIC_PROBE input_rejected; require one exact named input; no default fallback");
        result = 2;
        goto finish;
    }

    SDL_AudioSpec wanted = {0};
    SDL_AudioSpec obtained = {0};
    wanted.freq = 48000;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 1;
    wanted.samples = 240;
    device = SDL_OpenAudioDevice(device_name, 1, &wanted, &obtained,
                                 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (device == 0) {
        probe_log(output, "MIC_PROBE input_open_failed: %s", SDL_GetError());
        result = 3;
        goto finish;
    }
    if (obtained.format != AUDIO_S16SYS || obtained.channels != 1 || obtained.freq <= 0) {
        probe_log(output, "MIC_PROBE unsupported_capture_format");
        result = 3;
        goto finish;
    }

    probe_log(output, "MIC_PROBE local_capture_start rate=%d channels=%u duration_ms=1000", obtained.freq,
            obtained.channels);
    SDL_PauseAudioDevice(device, 0);
    const Uint32 end = SDL_GetTicks() + 1000;
    uint64_t sample_count = 0;
    uint64_t nonzero_count = 0;
    int peak = 0;
    while (!SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
        int16_t samples[4096];
        const Uint32 bytes = SDL_DequeueAudio(device, samples, sizeof(samples));
        for (Uint32 i = 0; i < bytes / sizeof(samples[0]); ++i) {
            const int sample = samples[i];
            const int magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak) {
                peak = magnitude;
            }
            nonzero_count += sample != 0;
            ++sample_count;
        }
        SDL_Delay(5);
    }
    SDL_PauseAudioDevice(device, 1);
    probe_log(output, "MIC_PROBE samples=%llu nonzero_samples=%llu peak=%d physical_input_verified=false",
            (unsigned long long) sample_count, (unsigned long long) nonzero_count, peak);
    probe_log(output, "MIC_PROBE confirm selected headset identity and voice correlation separately");
    if (nonzero_count == 0) {
        result = 4;
    }

finish:
    if (device != 0) {
        SDL_CloseAudioDevice(device);
    }
    if (own_audio) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (output != NULL) {
        fclose(output);
    }
    return result;
}
