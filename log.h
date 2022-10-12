#ifndef FAKE_CHEN_LOG_H
#define FAKE_CHEN_LOG_H
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#if defined(__GNUC__) && __GNUC__ >= 4
#define L_PRINTF(x, y) __attribute__((__format__(__printf__, x, y)))
#else
#define L_PRINTF(x, y)
#endif

enum log_importance {
	SILENT = 0,
	ERROR = 1,
	INFO = 2,
	DEBUG = 3,
	LOG_IMPORTANCE_LAST,
};
typedef void (*s_log_func_t)(const char *, va_list) L_PRINTF(1, 0);

typedef void (*log_func_t)(enum log_importance importance,
	const char *fmt, va_list args);


#ifdef __GNUC__
#define _ATTRIB_PRINTF(start, end) __attribute__((format(printf, start, end)))
#else
#define _ATTRIB_PRINTF(start, end)
#endif

void _debug_log(enum log_importance verbosity, const char *format, ...) _ATTRIB_PRINTF(2, 3);
void _debug_vlog(enum log_importance verbosity, const char *format, va_list args) _ATTRIB_PRINTF(2, 0);

#define fake_log(verb, fmt, ...) \
	_debug_log(verb, "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define fake_vlog(verb, fmt, args) \
	_debug_vlog(verb, "[%s:%d] " fmt, __FILE__E, __LINE__, args)
#define fake_log_errno(verb, fmt, ...) \
	fake_log(verb, fmt ": %s", ##__VA_ARGS__, strerror(errno))

void log_init(enum log_importance verbosity, log_func_t callback);
#endif 
