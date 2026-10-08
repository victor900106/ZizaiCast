/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *=================================================================
 * modified by fduncanh 2022
 */

#ifndef THREADS_H
#define THREADS_H

/* Always use pthread library */

#include <pthread.h>
#include <unistd.h>

#define sleepms(x) usleep((x)*1000)

#if defined(_MSC_VER)
/* PM: pthreads4w's pthread_t is a struct, but UxPlay treats thread handles as
 * scalars (handle = 0, !handle). Use a heap-allocated pthread_t* instead;
 * THREAD_JOIN frees it and resets the handle to NULL. */
#include <stdlib.h>
typedef pthread_t *thread_handle_t;

#define THREAD_RETVAL void *
#define THREAD_CREATE(handle, func, arg) \
	do { (handle) = (pthread_t *) malloc(sizeof(pthread_t)); \
	     if ((handle) && pthread_create((handle), NULL, func, arg)) { free(handle); (handle) = NULL; } } while (0)
#define THREAD_JOIN(handle) \
	do { if (handle) { pthread_join(*(handle), NULL); free(handle); (handle) = NULL; } } while (0)
#else
typedef pthread_t thread_handle_t;

#define THREAD_RETVAL void *
#define THREAD_CREATE(handle, func, arg) \
	if (pthread_create(&(handle), NULL, func, arg)) handle = 0
#define THREAD_JOIN(handle) pthread_join(handle, NULL)
#endif

typedef pthread_mutex_t mutex_handle_t;

typedef pthread_cond_t cond_handle_t;

#define MUTEX_CREATE(handle) pthread_mutex_init(&(handle), NULL)
#define MUTEX_LOCK(handle) pthread_mutex_lock(&(handle))
#define MUTEX_UNLOCK(handle) pthread_mutex_unlock(&(handle))
#define MUTEX_DESTROY(handle) pthread_mutex_destroy(&(handle))

#define COND_CREATE(handle) pthread_cond_init(&(handle), NULL)
#define COND_SIGNAL(handle) pthread_cond_signal(&(handle))
#define COND_DESTROY(handle) pthread_cond_destroy(&(handle))

#endif /* THREADS_H */
