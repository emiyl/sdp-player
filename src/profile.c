#include "profile.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

int profile_init(Profile *profile, const char *requested_path)
{
    memset(profile, 0, sizeof(*profile));
    if (!requested_path || !requested_path[0])
        return 0;

    snprintf(profile->path, sizeof(profile->path), "%s", requested_path);
    profile->file = fopen(profile->path, "w");
    if (!profile->file)
        return -1;

    fprintf(profile->file,
            "elapsed_ms,event,received_packets,received_bytes,"
            "video_packets,audio_packets,video_frames,audio_frames,"
            "decode_us,render_us,queue_packets,dropped_packets,"
            "read_errors\n");
    profile->start_counter = SDL_GetPerformanceCounter();
    profile->last_sample_counter = profile->start_counter;
    return 0;
}

void profile_packet(Profile *profile, int is_video, size_t bytes)
{
    profile->received_packets++;
    profile->received_bytes += bytes;
    if (is_video)
        profile->video_packets++;
    else
        profile->audio_packets++;
}

void profile_audio_frame(Profile *profile, size_t bytes)
{
    profile->audio_frames++;
    profile->received_bytes += bytes;
}

void profile_video_frame(Profile *profile, Uint64 decode_us,
                         Uint64 render_us)
{
    profile->video_frames++;
    profile->decode_us += decode_us;
    profile->render_us += render_us;
}

void profile_decode_time(Profile *profile, Uint64 decode_us)
{
    profile->decode_us += decode_us;
}

void profile_sample(Profile *profile, size_t queue_packets,
                    Uint64 dropped_packets, Uint64 read_errors)
{
    Uint64 counter = SDL_GetPerformanceCounter();
    Uint64 frequency = SDL_GetPerformanceFrequency();
    Uint64 elapsed_ms =
        (counter - profile->start_counter) * 1000 / frequency;
    Uint64 since_last_ms =
        (counter - profile->last_sample_counter) * 1000 / frequency;
    if (!profile->file || since_last_ms < 1000)
        return;

    fprintf(profile->file,
            "%llu,sample,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%zu,%llu,%llu\n",
            (unsigned long long)elapsed_ms,
            (unsigned long long)profile->received_packets,
            (unsigned long long)profile->received_bytes,
            (unsigned long long)profile->video_packets,
            (unsigned long long)profile->audio_packets,
            (unsigned long long)profile->video_frames,
            (unsigned long long)profile->audio_frames,
            (unsigned long long)profile->decode_us,
            (unsigned long long)profile->render_us,
            queue_packets,
            (unsigned long long)dropped_packets,
            (unsigned long long)read_errors);
    fflush(profile->file);
    profile->last_sample_counter = counter;
}

void profile_finish(Profile *profile)
{
    if (!profile->file)
        return;

    Uint64 counter = SDL_GetPerformanceCounter();
    Uint64 frequency = SDL_GetPerformanceFrequency();
    Uint64 elapsed_ms =
        (counter - profile->start_counter) * 1000 / frequency;
    fprintf(profile->file,
            "%llu,final,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "0,%llu,%llu\n",
            (unsigned long long)elapsed_ms,
            (unsigned long long)profile->received_packets,
            (unsigned long long)profile->received_bytes,
            (unsigned long long)profile->video_packets,
            (unsigned long long)profile->audio_packets,
            (unsigned long long)profile->video_frames,
            (unsigned long long)profile->audio_frames,
            (unsigned long long)profile->decode_us,
            (unsigned long long)profile->render_us,
            (unsigned long long)profile->dropped_packets,
            (unsigned long long)profile->read_errors);
    fclose(profile->file);
    profile->file = NULL;
}
