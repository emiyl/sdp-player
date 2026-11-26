# sdp-player

## Library API

The project builds `libsdp-player-core.a` as the reusable streaming library and
`sdp-player` as the SDL frontend. Include [sdp_player.h](include/sdp_player.h):

```c
#include <sdp_player.h>

SdpPlayerOptions options = {
    .process_video = 1,
    .process_audio = 1,
    .profile_path = NULL,
};

SdpPlayer *player = sdp_player_open("/path/to/source.sdp", &options);
SdpPlayerFrame frame;
while (sdp_player_poll(player, &frame)) {
    /* The frontend processes frame.data and frame.size. */
}
sdp_player_close(player);
```

A low-latency C application that opens an SDP file and renders RTP audio/video
with SDL2 and FFmpeg.

## Dependencies

- CMake 3.16+
- SDL2
- FFmpeg development libraries (libavformat, libavcodec, libavutil,
  libswscale, libswresample)

On macOS with Homebrew:

```sh
brew install cmake sdl2 ffmpeg pkg-config
```

On Debian/Ubuntu:

```sh
sudo apt install cmake libsdl2-dev libavformat-dev libavcodec-dev \
  libavutil-dev libswscale-dev libswresample-dev pkg-config
```

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Run

```sh
./build/sdp-player /path/to/source.sdp
```

To receive video only:

```sh
./build/sdp-player --no-audio /path/to/source.sdp
```

To receive audio only:

```sh
./build/sdp-player --no-video /path/to/source.sdp
```

To process neither stream:

```sh
./build/sdp-player --no-video --no-audio /path/to/source.sdp
```

To write performance measurements to a specific CSV file:

```sh
./build/sdp-player --profile /tmp/sdp-player-profile.csv /path/to/source.sdp
```

Without `--profile`, the app creates a timestamped
`sdp-player-profile-YYYYMMDD-HHMMSS.csv` file in the current directory.

Press Escape or close the window to quit.

Audio playback uses SDL queued audio with a bounded 64 KB queue and a 16 KB
prebuffer. This prevents callback underruns and limits audio/video backlog.

The player disables FFmpeg's decoder threading and probing delays where
possible, sets the RTP reorder queue to zero, and presents each decoded video
frame immediately without SDL or application-side frame buffering. Network
packet loss, multicast route, and encoder keyframe settings can still limit
the achievable latency.
