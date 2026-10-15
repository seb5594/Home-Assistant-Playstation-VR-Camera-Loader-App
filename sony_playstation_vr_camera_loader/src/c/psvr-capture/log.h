/* Leveled log lines in the same format as the Python services. */
#ifndef PSVR_LOG_H
#define PSVR_LOG_H

typedef enum { LOG_DEBUG = 10, LOG_INFO = 20, LOG_WARNING = 30, LOG_ERROR = 40 } log_level_t;

/* Reads LOG_LEVEL and COLOR_LOGS from the environment. */
void log_init(const char *scope);
void log_msg(log_level_t level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_info(...) log_msg(LOG_INFO, __VA_ARGS__)
#define log_warning(...) log_msg(LOG_WARNING, __VA_ARGS__)
#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)

#endif
