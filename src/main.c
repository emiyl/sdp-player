#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <SDL.h>

#include "gui.h"
#include "media.h"
#include "network.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *sdp_path = NULL;
    int process_audio = 1;
    int process_video = 1;

    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s [--no-video] [--no-audio] <file.sdp>\n",
                argv[0]);
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-audio") == 0)
            process_audio = 0;
        else if (strcmp(argv[i], "--no-video") == 0)
            process_video = 0;
        else
            sdp_path = argv[i];
    }
    if (!sdp_path) {
        fprintf(stderr,
                "Usage: %s [--no-video] [--no-audio] <file.sdp>\n",
                argv[0]);
        return 2;
    }

    Gui gui;
    memset(&gui, 0, sizeof(gui));
    if (process_video && gui_init(&gui, process_audio) != 0)
        return 1;
    if (!process_video &&
        SDL_Init(process_audio ? SDL_INIT_AUDIO | SDL_INIT_EVENTS
                               : SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    Network network;
    if (network_open(&network, sdp_path) != 0) {
        gui_shutdown(&gui);
        return 1;
    }
    if (!process_audio)
        network_disable_audio(&network);
    if (!process_video)
        network_disable_video(&network);

    Media media;
    if (media_init(&media, network.format,
                   network.video_index,
                   process_audio ? network.audio_index : -1,
                   process_audio, process_video) != 0)
        fprintf(stderr, "SDP contains no supported video or audio stream\n");
    if (network_start(&network) != 0) {
        media_shutdown(&media);
        network_close(&network);
        gui_shutdown(&gui);
        return 1;
    }

    AVPacket *packet = av_packet_alloc();
    int running = 1;
    int video_received = 0;
    int stream_was_received = 0;
    int displayed_status = 0;
    Uint32 last_packet_ms = SDL_GetTicks();

    while (running) {
        if (process_video) {
            int gui_event = gui_poll_quit(&gui, !video_received);
            if (gui_event == 1) {
                running = 0;
                break;
            }
            if (gui_event == -1) {
                if (stream_was_received) {
                    gui_draw_status(
                        &gui,
                        displayed_status == 2 ? "Connection lost" :
                        displayed_status == 1 ? "Reconnecting..." :
                        "Waiting for video stream..."
                    );
                } else {
                    gui_draw_waiting(&gui);
                }
            }
        }

        if (process_video &&
            network_pop_packet(&network, packet)) {
            last_packet_ms = SDL_GetTicks();
            if (stream_was_received && !video_received)
                gui_draw_status(&gui, "Reconnecting...");

            enum AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
            if (packet->stream_index == network.video_index)
                type = AVMEDIA_TYPE_VIDEO;
            else if (packet->stream_index == network.audio_index)
                type = AVMEDIA_TYPE_AUDIO;

            if (media_decode_packet(&media, packet, type, &gui)) {
                video_received = 1;
                stream_was_received = 1;
                displayed_status = 0;
                last_packet_ms = SDL_GetTicks();
            }
        } else if (network_pop_packet(&network, packet)) {
            enum AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
            if (packet->stream_index == network.audio_index)
                type = AVMEDIA_TYPE_AUDIO;
            media_decode_packet(&media, packet, type, &gui);
        }

        if (process_video) {
            Uint32 silent_ms = SDL_GetTicks() - last_packet_ms;
            if (!stream_was_received) {
                if (displayed_status != 0) {
                    displayed_status = 0;
                    video_received = 0;
                }
                gui_draw_waiting(&gui);
            } else if (silent_ms >= 5000) {
                if (displayed_status != 2) {
                    video_received = 0;
                    displayed_status = 2;
                    gui_draw_status(&gui, "Connection lost");
                }
            } else if (silent_ms >= 2000) {
                if (displayed_status != 1) {
                    video_received = 0;
                    displayed_status = 1;
                    gui_draw_status(&gui, "Reconnecting...");
                }
            } else if (video_received && displayed_status != 0) {
                displayed_status = 0;
            }
        }

        SDL_Delay(10);
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    network_stop(&network);
    media_shutdown(&media);
    network_close(&network);
    if (process_video)
        gui_shutdown(&gui);
    else
        SDL_Quit();
    return 0;
}
