#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <SDL.h>

#include "gui.h"
#include "media.h"
#include "network.h"
#include "profile.h"

#include <stdio.h>
#include <string.h>

static int exit_with_error(Gui *gui, int gui_initialized,
                           int process_audio, int process_video,
                           int exit_code, const char *message)
{
    fprintf(stderr, "%s\n", message);

    if (process_video && !gui_initialized)
        gui_initialized = gui_init(gui, process_audio) == 0;

    if (gui_initialized)
        gui_draw_status(gui, message);

    Uint32 start = SDL_GetTicks();
    while (SDL_GetTicks() - start < 5000) {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            (void)event;
        SDL_Delay(10);
    }

    if (gui_initialized)
        gui_shutdown(gui);
    else
        SDL_Quit();
    return exit_code;
}

static void render_video_frame(void *userdata,
                               const SdpPlayerVideoFrame *frame)
{
    Gui *gui = userdata;
    gui_render_yuv(
        gui,
        frame->planes,
        frame->linesizes,
        frame->width,
        frame->height
    );
}

int main(int argc, char **argv)
{
#ifdef __SWITCH__
    const char *sdp_path = "sdmc:/switch/sdp-player/stream.sdp";
#else
    const char *sdp_path = "stream.sdp";
#endif
    int process_audio = 1;
    int process_video = 1;
    const char *profile_path = NULL;
    Gui gui;
    memset(&gui, 0, sizeof(gui));
    int gui_initialized = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-audio") == 0)
            process_audio = 0;
        else if (strcmp(argv[i], "--no-video") == 0)
            process_video = 0;
        else if (strcmp(argv[i], "--profile") == 0) {
            if (i + 1 >= argc) {
                char message[256];
                snprintf(message, sizeof(message),
                         "Usage: %s [--no-video] [--no-audio] [--profile <path>] <file.sdp>",
                         argv[0]);
                return exit_with_error(&gui, 0, process_audio, process_video, 2, message);
            }
            profile_path = argv[++i];
        }
        else
            sdp_path = argv[i];
    }
    if (!sdp_path) {
        char message[256];
        snprintf(message, sizeof(message),
                 "Usage: %s [--no-video] [--no-audio] [--profile <path>] <file.sdp>",
                 argv[0]);
        return exit_with_error(&gui, 0, process_audio, process_video, 2, message);
    }

    if (process_video) {
        gui_initialized = gui_init(&gui, process_audio) == 0;
        if (!gui_initialized)
            return exit_with_error(&gui, 0, process_audio, process_video,
                                   1, "Could not initialize the player display");
    }
    if (!process_video &&
        SDL_Init(process_audio ? SDL_INIT_AUDIO | SDL_INIT_EVENTS
                               : SDL_INIT_EVENTS) != 0) {
        char message[256];
        snprintf(message, sizeof(message), "SDL_Init failed: %s", SDL_GetError());
        return exit_with_error(&gui, 0, process_audio, process_video, 1, message);
    }

    Profile profile;
    if (profile_init(&profile, profile_path) != 0) {
        if (gui_initialized)
            gui_shutdown(&gui);
        return exit_with_error(&gui, 0, process_audio, process_video,
                               1, "Could not create performance profile log");
    }

    Network network;
    if (network_open(&network, sdp_path) != 0) {
        if (gui_initialized)
            gui_shutdown(&gui);
        profile_finish(&profile);
        char error_message[256];
        snprintf(error_message, sizeof(error_message),
                 "Network error: Could not open the SDP stream\n"
                 "IP address: %s",
                 network_address(&network));
        return exit_with_error(&gui, 0, process_audio, process_video,
                               1, error_message);
    }
    if (!process_audio)
        network_disable_audio(&network);
    if (!process_video)
        network_disable_video(&network);

    Media media;
    media.profile = &profile;
    if (media_init(&media, network.format,
                   network.video_index,
                   process_audio ? network.audio_index : -1,
                   process_audio, process_video) != 0) {
        network_close(&network);
        profile_finish(&profile);
        if (gui_initialized)
            gui_shutdown(&gui);
        return exit_with_error(&gui, 0, process_audio, process_video,
                               1, "SDP contains no supported video or audio stream");
    }
    if (process_video) {
        media.video_callback = render_video_frame;
        media.video_userdata = &gui;
    }
    if (network_start(&network) != 0) {
        media_shutdown(&media);
        network_close(&network);
        profile_finish(&profile);
        if (gui_initialized)
            gui_shutdown(&gui);
        return exit_with_error(&gui, 0, process_audio, process_video,
                               1, "Could not start the RTP network stream");
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
            profile_packet(
                &profile,
                packet->stream_index == network.video_index,
                (size_t)packet->size
            );
            if (stream_was_received && !video_received)
                gui_draw_status(&gui, "Reconnecting...");

            enum AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
            if (packet->stream_index == network.video_index)
                type = AVMEDIA_TYPE_VIDEO;
            else if (packet->stream_index == network.audio_index)
                type = AVMEDIA_TYPE_AUDIO;

            Uint64 decode_start = SDL_GetPerformanceCounter();
            if (media_decode_packet(&media, packet, type, &profile)) {
                video_received = 1;
                stream_was_received = 1;
                displayed_status = 0;
                last_packet_ms = SDL_GetTicks();
            }
            Uint64 decode_end = SDL_GetPerformanceCounter();
            Uint64 decode_us =
                (decode_end - decode_start) * 1000000 /
                SDL_GetPerformanceFrequency();
            profile_decode_time(&profile, decode_us);
        } else if (network_pop_packet(&network, packet)) {
            profile_packet(
                &profile,
                packet->stream_index == network.video_index,
                (size_t)packet->size
            );
            enum AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
            if (packet->stream_index == network.audio_index)
                type = AVMEDIA_TYPE_AUDIO;
            media_decode_packet(&media, packet, type, &profile);
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

        profile_sample(
            &profile,
            network_queue_depth(&network),
            network_dropped_packets(&network),
            profile.read_errors
        );
        SDL_Delay(1);
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    network_stop(&network);
    media_shutdown(&media);
    network_close(&network);
    profile_finish(&profile);
    if (process_video)
        gui_shutdown(&gui);
    else
        SDL_Quit();
    return 0;
}
