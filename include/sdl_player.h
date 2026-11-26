#ifndef SDP_SDL_H
#define SDP_SDL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SdpPlayer SdpPlayer;

typedef struct SdpPlayerOptions {
    int process_video;
    int process_audio;
    const char *profile_path;
} SdpPlayerOptions;

typedef struct SdpPlayerFrame {
    int stream_index;
    unsigned char *data;
    size_t size;
} SdpPlayerFrame;

typedef struct SdpPlayerVideoFrame {
    const unsigned char *planes[4];
    int linesizes[4];
    int width;
    int height;
} SdpPlayerVideoFrame;

typedef void (*SdpPlayerVideoCallback)(
    void *userdata,
    const SdpPlayerVideoFrame *frame
);

typedef void (*SdpPlayerAudioCallback)(
    void *userdata,
    const unsigned char *pcm,
    size_t size
);

void sdp_sdl_set_video_callback(
    SdpPlayer *player,
    SdpPlayerVideoCallback callback,
    void *userdata
);

void sdp_sdl_set_audio_callback(
    SdpPlayer *player,
    SdpPlayerAudioCallback callback,
    void *userdata
);

SdpPlayer *sdp_sdl_open(const char *sdp_path, const SdpPlayerOptions *options);
int sdp_sdl_poll(SdpPlayer *player, SdpPlayerFrame *frame);
void sdp_sdl_close(SdpPlayer *player);

#ifdef __cplusplus
}
#endif

#endif
