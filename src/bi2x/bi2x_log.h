#ifndef BI2X_LOG_H
#define BI2X_LOG_H

enum bi2x_log_level {
    BI2X_LOG_MISC = 0,
    BI2X_LOG_INFO = 1,
    BI2X_LOG_WARNING = 2,
    BI2X_LOG_FATAL = 3,
};

typedef void (*bi2x_log_sink_t)(int level, const char *msg);

/* NULL sink -> stderr */
void bi2x_log_set_sink(bi2x_log_sink_t sink);
void bi2x_log_set_level(int min_level);

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void bi2x_log(int level, const char *fmt, ...);

#define bi2x_misc(...) bi2x_log(BI2X_LOG_MISC, __VA_ARGS__)
#define bi2x_info(...) bi2x_log(BI2X_LOG_INFO, __VA_ARGS__)
#define bi2x_warn(...) bi2x_log(BI2X_LOG_WARNING, __VA_ARGS__)
#define bi2x_fatal(...) bi2x_log(BI2X_LOG_FATAL, __VA_ARGS__)

#endif
