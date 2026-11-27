#include "logging.h"

#ifdef DEBUG

#include <libavutil/log.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

static FILE *debug_log_file;

void debug_log_begin(void)
{
    if (debug_log_file)
        return;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    debug_log_file = fopen("sdmc:/switch/sdp.log", "w");
#else
    debug_log_file = stderr;
#endif
    if (!debug_log_file)
        fprintf(stderr, "Could not open debug log\n");
}

void debug_log(const char *format, ...)
{
    if (!debug_log_file)
        debug_log_begin();
    if (!debug_log_file)
        return;

    va_list args;
    va_start(args, format);
    vfprintf(debug_log_file, format, args);
    va_end(args);
    fflush(debug_log_file);
}

void debug_ffmpeg_log(void *avcl, int level,
                      const char *format, va_list args)
{
    if (!debug_log_file)
        debug_log_begin();
    if (!debug_log_file)
        return;

    char line[2048];
    int print_prefix = 1;
    av_log_format_line(avcl, level, format, args,
                       line, sizeof(line), &print_prefix);
    fprintf(debug_log_file, "%s", line);
    fflush(debug_log_file);
}

void debug_log_shutdown(void)
{
    av_log_set_callback(av_log_default_callback);
    if (debug_log_file && debug_log_file != stderr) {
        debug_log("network closed\n");
        fclose(debug_log_file);
        debug_log_file = NULL;
    }
}

#endif
