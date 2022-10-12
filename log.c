#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "log.h" 

static const long NSEC_PER_SEC = 1000000000;
static struct timespec start_time = {-1};
static enum log_importance log_importance = ERROR;
static bool colored = true;
static const char *verbosity_colors[] = {
	[SILENT] = "",
	[ERROR] = "\x1B[1;31m",
	[INFO] = "\x1B[1;34m",
	[DEBUG] = "\x1B[1;90m",
};
static const char *verbosity_headers[] = {
	[SILENT] = "",
	[ERROR] = "[ERROR]",
	[INFO] = "[INFO]",
	[DEBUG] = "[DEBUG]",
};

static void init_start_time(void) {
	if (start_time.tv_sec >= 0) {
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &start_time);
}

void timespec_sub(struct timespec *r, const struct timespec *a,
		const struct timespec *b) {
	r->tv_sec = a->tv_sec - b->tv_sec;
	r->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (r->tv_nsec < 0) {
		r->tv_sec--;
		r->tv_nsec += NSEC_PER_SEC;
	}
}

static void log_stderr(enum log_importance verbosity, const char *fmt,
		va_list args) {
	init_start_time();

	if (verbosity > log_importance) {
		return;
	}

	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	timespec_sub(&ts, &ts, &start_time);

	fprintf(stderr, "%02d:%02d:%02d.%03ld ", (int)(ts.tv_sec / 60 / 60),
		(int)(ts.tv_sec / 60 % 60), (int)(ts.tv_sec % 60),
		ts.tv_nsec / 1000000);

	unsigned c = (verbosity < LOG_IMPORTANCE_LAST) ? verbosity : LOG_IMPORTANCE_LAST - 1;

	if (colored && isatty(STDERR_FILENO)) {
		fprintf(stderr, "%s", verbosity_colors[c]);
	} else {
		fprintf(stderr, "%s ", verbosity_headers[c]);
	}

	vfprintf(stderr, fmt, args);

	if (colored && isatty(STDERR_FILENO)) {
		fprintf(stderr, "\x1B[0m");
	}
	fprintf(stderr, "\n");
}

static log_func_t log_callback = log_stderr;

void _debug_vlog(enum log_importance verbosity, const char *fmt, va_list args) {
	log_callback(verbosity, fmt, args);
}

void _debug_log(enum log_importance verbosity, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	log_callback(verbosity, fmt, args);
	va_end(args);
}

static void
log_stderr_handler(const char *fmt, va_list arg)
{
	vfprintf(stderr, fmt, arg);
}

void log_init(enum log_importance verbosity, log_func_t callback) {
	init_start_time();

	if (verbosity < LOG_IMPORTANCE_LAST) {
		log_importance = verbosity;
	}
	if (callback) {
		log_callback = callback;
	}

}

