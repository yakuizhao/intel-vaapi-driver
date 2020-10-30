/*******************************************************************************
    Copyright (c) 2019 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/
#ifndef __NV_TIME_H__
#define __NV_TIME_H__

#include "conftest.h"

#include <linux/ktime.h>

#if defined (NV_TIMEVAL_PRESENT)
typedef struct timeval nv_timeval; 
#else
typedef struct __kernel_old_timeval nv_timeval;
#endif

static inline void nv_gettimeofday(nv_timeval *tv)
{
#ifdef NV_DO_GETTIMEOFDAY_PRESENT
    do_gettimeofday(tv);
#else
    struct timespec64 now;

    ktime_get_real_ts64(&now);

    *tv = (nv_timeval) {
        .tv_sec = now.tv_sec,
        .tv_usec = now.tv_nsec/1000,
    };
#endif // NV_DO_GETTIMEOFDAY_PRESENT
}

#endif // __NV_TIME_H__
