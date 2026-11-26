#ifndef PROFILE_H
#define PROFILE_H

#include <stdint.h>
#include <stdio.h>
#include <SDL.h>

typedef struct Profile {
    FILE *file;
    char path[1024];
    Uint64 start_counter;
    Uint64 last_sample_counter;
    Uint64 received_packets;
    Uint64 received_bytes;
    Uint64 video_packets;
    Uint64 audio_packets;
    Uint64 video_frames;
    Uint64 audio_frames;
    Uint64 dropped_packets;
    Uint64 read_errors;
    Uint64 decode_us;
    Uint64 render_us;
} Profile;

int profile_init(Profile *profile, const char *requested_path);
void profile_packet(Profile *profile, int is_video, size_t bytes);
void profile_audio_frame(Profile *profile, size_t bytes);
void profile_video_frame(Profile *profile, Uint64 decode_us, Uint64 render_us);
void profile_decode_time(Profile *profile, Uint64 decode_us);
void profile_sample(Profile *profile, size_t queue_packets,
                    Uint64 dropped_packets, Uint64 read_errors);
void profile_finish(Profile *profile);

#endif
