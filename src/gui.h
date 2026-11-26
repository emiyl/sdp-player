#ifndef GUI_H
#define GUI_H

#include <SDL.h>

typedef struct Gui {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
} Gui;

int gui_init(Gui *gui);
void gui_shutdown(Gui *gui);
int gui_poll_quit(Gui *gui, int status_visible);
void gui_draw_status(Gui *gui, const char *text);
void gui_draw_waiting(Gui *gui);
void gui_render_yuv(Gui *gui, const uint8_t *planes[4],
                     const int linesizes[4], int width, int height);

#endif
