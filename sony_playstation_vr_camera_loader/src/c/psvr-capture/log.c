#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static log_level_t min_level = LOG_INFO;
static int use_colors = 1;
static const char *log_scope = "Engine";

static log_level_t parse_level(const char *text)
{
    if (!text) return LOG_INFO;
    if (!strcasecmp(text, "debug")) return LOG_DEBUG;
    if (!strcasecmp(text, "warning")) return LOG_WARNING;
    if (!strcasecmp(text, "error")) return LOG_ERROR;
    return LOG_INFO;
}

void log_init(const char *scope)
{
    const char *colors = getenv("COLOR_LOGS");
    min_level = parse_level(getenv("LOG_LEVEL"));
    use_colors = !colors || !strcasecmp(colors, "true");
    if (scope) log_scope = scope;
}

void log_msg(log_level_t level, const char *format, ...)
{
    static const struct { log_level_t level; const char *name; const char *color; } labels[] = {
        {LOG_DEBUG, "DEBUG", NULL}, {LOG_INFO, "INFO", "36"},
        {LOG_WARNING, "WARNING", "33"}, {LOG_ERROR, "ERROR", "31"},
    };
    const char *name = "INFO", *color = NULL;
    char message[1024];
    va_list args;

    if (level < min_level) return;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (labels[i].level == level) {
            name = labels[i].name;
            color = labels[i].color;
        }
    }
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    /* stderr is line buffered by the supervisor; one fprintf keeps lines whole. */
    if (use_colors && color)
        fprintf(stderr, "\033[%sm[%s]\033[0m [%s] %s\n", color, name, log_scope, message);
    else
        fprintf(stderr, "[%s] [%s] %s\n", name, log_scope, message);
}
