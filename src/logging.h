#ifndef LOGGING_H
#define LOGGING_H

#include <stdarg.h>

#ifdef DEBUG
void debug_log_begin(void);
void debug_log(const char *format, ...);
void debug_ffmpeg_log(void *avcl, int level,
                      const char *format, va_list args);
void debug_log_shutdown(void);
#else
static inline void debug_log_begin(void) {}
static inline void debug_log(const char *format, ...)
{
    (void)format;
}
static inline void debug_ffmpeg_log(void *avcl, int level,
                                    const char *format, va_list args)
{
    (void)avcl;
    (void)level;
    (void)format;
    (void)args;
}
static inline void debug_log_shutdown(void) {}
#endif

#endif
