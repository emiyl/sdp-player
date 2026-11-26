#include "gui.h"
#include "font.h"

#include <stdio.h>
#include <string.h>

static const struct Font *find_glyph(char ch)
{
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++) {
        if (font[i].letter == ch)
            return &font[i];
    }
    return NULL;
}

static void glyph_bounds(const struct Font *glyph, int *left, int *width)
{
    if (!glyph) {
        *left = 0;
        *width = 0;
        return;
    }

    int left_column = 5;
    int right_column = -1;
    for (int row = 0; row < 7; row++) {
        for (int column = 0; column < 5; column++) {
            if (glyph->code[row][column] == '#') {
                if (column < left_column)
                    left_column = column;
                if (column > right_column)
                    right_column = column;
            }
        }
    }

    if (right_column >= left_column) {
        *left = left_column;
        *width = right_column - left_column + 1;
    } else {
        *left = 0;
        *width = 0;
    }
}

int gui_init(Gui *gui, int init_audio)
{
    memset(gui, 0, sizeof(*gui));

    Uint32 init_flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    if (init_audio)
        init_flags |= SDL_INIT_AUDIO;
    if (SDL_Init(init_flags) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    gui->window = SDL_CreateWindow(
        "SDP RTP Player",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_RESIZABLE
    );
    if (!gui->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    gui->renderer = SDL_CreateRenderer(gui->window, -1, SDL_RENDERER_ACCELERATED);
    if (!gui->renderer)
        gui->renderer = SDL_CreateRenderer(gui->window, -1, SDL_RENDERER_SOFTWARE);
    if (!gui->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(gui->window);
        gui->window = NULL;
        SDL_Quit();
        return -1;
    }

    gui_draw_waiting(gui);
    SDL_PumpEvents();
    return 0;
}

void gui_shutdown(Gui *gui)
{
    if (!gui)
        return;
    if (gui->texture)
        SDL_DestroyTexture(gui->texture);
    if (gui->renderer)
        SDL_DestroyRenderer(gui->renderer);
    if (gui->window)
        SDL_DestroyWindow(gui->window);
    SDL_Quit();
}

int gui_poll_quit(Gui *gui, int status_visible)
{
    (void)gui;
    SDL_Event event;
    int resized = 0;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT ||
            (event.type == SDL_KEYDOWN &&
             event.key.keysym.sym == SDLK_ESCAPE))
            return 1;
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE)
            return 1;
        if (status_visible &&
            event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_RESIZED)
            resized = 1;
    }
    return -resized;
}

void gui_draw_status(Gui *gui, const char *text)
{
    if (!gui || !gui->renderer)
        return;

    SDL_SetRenderDrawColor(gui->renderer, 18, 24, 32, 255);
    SDL_RenderClear(gui->renderer);
    SDL_SetRenderDrawColor(gui->renderer, 235, 240, 245, 255);

    int scale = 3;
    int text_width = 0;
    for (size_t i = 0; text[i]; i++) {
        if (text[i] == ' ') {
            text_width += 3 * scale;
            continue;
        }
        const struct Font *glyph = find_glyph(text[i]);
        int left, width;
        glyph_bounds(glyph, &left, &width);
        text_width += width > 0 ? (width + 1) * scale : 0;
    }
    if (text_width >= scale)
        text_width -= scale;

    int viewport_width, viewport_height;
    SDL_GetRendererOutputSize(gui->renderer, &viewport_width, &viewport_height);
    int origin_x = (viewport_width - text_width) / 2;
    int origin_y = (viewport_height - 7 * scale) / 2;

    for (size_t i = 0; text[i]; i++) {
        char ch = text[i];
        if (ch == ' ') {
            origin_x += 3 * scale;
            continue;
        }
        const struct Font *glyph = find_glyph(ch);
        if (!glyph) {
            origin_x += 3 * scale;
            continue;
        }

        int left, glyph_width;
        glyph_bounds(glyph, &left, &glyph_width);
        for (int row = 0; row < 7; row++) {
            for (int column = 0; column < 5; column++) {
                if (glyph->code[row][column] != '#')
                    continue;
                SDL_Rect pixel = {
                    origin_x + (column - left) * scale,
                    origin_y + row * scale,
                    scale,
                    scale
                };
                SDL_RenderFillRect(gui->renderer, &pixel);
            }
        }
        origin_x += (glyph_width + 1) * scale;
    }

    SDL_RenderPresent(gui->renderer);
}

void gui_draw_waiting(Gui *gui)
{
    gui_draw_status(gui, "Waiting for video stream...");
}

void gui_render_yuv(Gui *gui, const uint8_t *planes[4],
                    const int linesizes[4], int width, int height)
{
    if (!gui || !gui->renderer)
        return;

    if (gui->texture) {
        int texture_width, texture_height;
        SDL_QueryTexture(
            gui->texture,
            NULL,
            NULL,
            &texture_width,
            &texture_height
        );
        if (texture_width != width || texture_height != height) {
            SDL_DestroyTexture(gui->texture);
            gui->texture = NULL;
        }
    }

    if (!gui->texture) {
        gui->texture = SDL_CreateTexture(
            gui->renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            width,
            height
        );
    }
    if (!gui->texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return;
    }

    SDL_UpdateYUVTexture(
        gui->texture,
        NULL,
        planes[0],
        linesizes[0],
        planes[1],
        linesizes[1],
        planes[2],
        linesizes[2]
    );
    SDL_RenderClear(gui->renderer);
    SDL_RenderCopy(gui->renderer, gui->texture, NULL, NULL);
    SDL_RenderPresent(gui->renderer);
}
