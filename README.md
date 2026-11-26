# sdp-sdl

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
./build/sdp-sdl /path/to/source.sdp
```

Press Escape or close the window to quit.

The player disables FFmpeg's decoder threading and probing delays where
possible, sets the RTP reorder queue to zero, and presents each decoded video
frame immediately without SDL or application-side frame buffering. Network
packet loss, multicast route, and encoder keyframe settings can still limit
the achievable latency.
