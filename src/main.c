#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <SDL.h>

#include "gui.h"
#include "media.h"
#include "network.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file.sdp>\n", argv[0]);
        return 2;
    }

    Gui gui;
    if (gui_init(&gui) != 0)
        return 1;

    Network network;
    if (network_open(&network, argv[1]) != 0) {
        gui_shutdown(&gui);
        return 1;
    }

    if (network.video_index >= 0 &&
        network.format->streams[network.video_index]->codecpar->width <= 0)
        fprintf(stderr,
                "Warning: the SDP has no sprop-parameter-sets and H264 SPS/PPS "
                "was not received during startup.\n");

    Media media;
    if (media_init(&media, network.format,
                   network.video_index, network.audio_index) != 0)
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

        if (network_pop_packet(&network, packet)) {
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
        }

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

        SDL_Delay(10);
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    network_stop(&network);
    media_shutdown(&media);
    network_close(&network);
    gui_shutdown(&gui);
    return 0;
}
