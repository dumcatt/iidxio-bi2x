#include <stdarg.h>
#include <stdio.h>

#include "bi2x_log.h"

static bi2x_log_sink_t log_sink;
static int log_min_level = BI2X_LOG_INFO;

void bi2x_log_set_sink(bi2x_log_sink_t sink)
{
    log_sink = sink;
}

void bi2x_log_set_level(int min_level)
{
    log_min_level = min_level;
}

void bi2x_log(int level, const char *fmt, ...)
{
    static const char *const names[] = {"M", "I", "W", "F"};
    char buf[512];
    va_list ap;

    if (level < log_min_level) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (log_sink != NULL) {
        log_sink(level, buf);
    } else {
        fprintf(
            stderr, "[bi2x:%s] %s\n",
            names[(level >= 0 && level <= 3) ? level : 0], buf);
    }
}
