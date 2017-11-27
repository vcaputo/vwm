#ifndef _UTIL_H
#define _UTIL_H

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define VWM_ERROR(_fmt, _args...)	fprintf(stderr, "%s:%i\t%s() "_fmt"\n", __FILE__, __LINE__, __FUNCTION__, ##_args)
#define VWM_PERROR(_fmt, _args...)	fprintf(stderr, "%s:%i\t%s() "_fmt"; %s\n", __FILE__, __LINE__, __FUNCTION__, ##_args, strerror(errno))
#define VWM_BUG(_fmt, _args...)		VWM_ERROR("BUG: "_fmt, ##_args)

#ifdef TRACE
#define VWM_TRACE(_fmt, _args...)	fprintf(stderr, "%s:%i\t%s() "_fmt"\n", __FILE__, __LINE__, __FUNCTION__, ##_args)
#else
#define VWM_TRACE(_fmt, _args...)	do { } while(0)
#endif

#define VWM_TRACE_WIN(_win, _fmt, _args...) \
					VWM_TRACE("win=%#"PRIx64": "_fmt, (uint64_t)_win, ##_args)

#define MIN(_a, _b)			((_a) < (_b) ? (_a) : (_b))
#define MAX(_a, _b)			((_a) > (_b) ? (_a) : (_b))
#define NELEMS(_a)			(sizeof(_a) / sizeof(_a[0]))

#endif
