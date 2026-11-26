#include "sdp_player.h"

#include "network.h"
#include "media.h"
#include "profile.h"

#include <libavcodec/avcodec.h>
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

struct SdpPlayer {
    Network network;
    Media media;
    Profile profile;
    AVPacket *packet;
    SdpPlayerVideoCallback video_callback;
    SdpPlayerAudioCallback audio_callback;
    void *video_userdata;
    void *audio_userdata;
};

void sdp_player_set_video_callback(
    SdpPlayer *player,
    SdpPlayerVideoCallback callback,
    void *userdata
)
{
    if (!player)
        return;
    player->video_callback = callback;
    player->video_userdata = userdata;
    player->media.video_callback = callback;
    player->media.video_userdata = userdata;
}

void sdp_player_set_audio_callback(
    SdpPlayer *player,
    SdpPlayerAudioCallback callback,
    void *userdata
)
{
    if (!player)
        return;
    player->audio_callback = callback;
    player->audio_userdata = userdata;
    player->media.audio_callback = callback;
    player->media.audio_userdata = userdata;
}

SdpPlayer *sdp_player_open(const char *sdp_path, const SdpPlayerOptions *options)
{
    SdpPlayer *player = calloc(1, sizeof(*player));
    if (!player)
        return NULL;

    int process_video = options ? options->process_video : 1;
    int process_audio = options ? options->process_audio : 1;
    const char *profile_path =
        options ? options->profile_path : NULL;

    if (network_open(&player->network, sdp_path) != 0) {
        free(player);
        return NULL;
    }
    if (!process_audio)
        network_disable_audio(&player->network);
    if (!process_video)
        network_disable_video(&player->network);

    profile_init(&player->profile, profile_path);
    player->media.profile = &player->profile;
    if (media_init(&player->media,
                   player->network.format,
                   process_video ? player->network.video_index : -1,
                   process_audio ? player->network.audio_index : -1,
                   process_audio,
                   process_video) != 0) {
        network_close(&player->network);
        profile_finish(&player->profile);
        free(player);
        return NULL;
    }
    if (network_start(&player->network) != 0) {
        media_shutdown(&player->media);
        network_close(&player->network);
        profile_finish(&player->profile);
        free(player);
        return NULL;
    }
    player->packet = av_packet_alloc();
    return player;
}

int sdp_player_poll(SdpPlayer *player, SdpPlayerFrame *frame)
{
    if (!player || !frame)
        return 0;
    if (!network_pop_packet(&player->network, player->packet))
        return 0;

    profile_packet(
        &player->profile,
        player->packet->stream_index == player->network.video_index,
        (size_t)player->packet->size
    );

    frame->stream_index = player->packet->stream_index;
    frame->data = player->packet->data;
    frame->size = player->packet->size;
    av_packet_unref(player->packet);
    return 1;
}

void sdp_player_close(SdpPlayer *player)
{
    if (!player)
        return;
    network_stop(&player->network);
    media_shutdown(&player->media);
    network_close(&player->network);
    profile_finish(&player->profile);
    av_packet_free(&player->packet);
    free(player);
}
