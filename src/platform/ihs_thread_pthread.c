/*
 * pthread backend for ihslib's thread interface (ihs_thread.h).
 *
 * ihslib ships an SDL backend only. Mutexes are recursive here because SDL mutexes are,
 * and ihslib's locking was written against that behaviour.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* pthread_setname_np */
#endif

#include "ihs_thread.h"

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct IHS_Thread {
    pthread_t handle;
    IHS_ThreadFunction *function;
    void *context;
};

static void *ThreadEntry(void *arg) {
    IHS_Thread *thread = arg;
    thread->function(thread->context);
    return NULL;
}

IHS_Thread *IHS_ThreadCreate(IHS_ThreadFunction *function, const char *name, void *context) {
    IHS_Thread *thread = calloc(1, sizeof(IHS_Thread));
    if (thread == NULL) {
        return NULL;
    }
    thread->function = function;
    thread->context = context;
    if (pthread_create(&thread->handle, NULL, ThreadEntry, thread) != 0) {
        free(thread);
        return NULL;
    }
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    if (name != NULL) {
        char shortName[16];
        strncpy(shortName, name, sizeof(shortName) - 1);
        shortName[sizeof(shortName) - 1] = '\0';
        pthread_setname_np(thread->handle, shortName);
    }
#else
    (void) name;
#endif
    return thread;
}

void IHS_ThreadJoin(IHS_Thread *thread) {
    assert(thread != NULL);
    pthread_join(thread->handle, NULL);
    free(thread);
}

IHS_Mutex *IHS_MutexCreate() {
    pthread_mutex_t *mutex = malloc(sizeof(pthread_mutex_t));
    if (mutex == NULL) {
        return NULL;
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    return (IHS_Mutex *) mutex;
}

void IHS_MutexDestroy(IHS_Mutex *mutex) {
    assert(mutex != NULL);
    pthread_mutex_destroy((pthread_mutex_t *) mutex);
    free(mutex);
}

bool IHS_MutexLock(IHS_Mutex *mutex) {
    assert(mutex != NULL);
    return pthread_mutex_lock((pthread_mutex_t *) mutex) == 0;
}

bool IHS_MutexUnlock(IHS_Mutex *mutex) {
    assert(mutex != NULL);
    return pthread_mutex_unlock((pthread_mutex_t *) mutex) == 0;
}

IHS_Cond *IHS_CondCreate() {
    pthread_cond_t *cond = malloc(sizeof(pthread_cond_t));
    if (cond == NULL) {
        return NULL;
    }
    pthread_cond_init(cond, NULL);
    return (IHS_Cond *) cond;
}

void IHS_CondDestroy(IHS_Cond *cond) {
    assert(cond != NULL);
    pthread_cond_destroy((pthread_cond_t *) cond);
    free(cond);
}

void IHS_CondSignal(IHS_Cond *cond) {
    assert(cond != NULL);
    pthread_cond_signal((pthread_cond_t *) cond);
}

bool IHS_CondWait(IHS_Cond *cond, IHS_Mutex *mutex) {
    assert(cond != NULL);
    assert(mutex != NULL);
    return pthread_cond_wait((pthread_cond_t *) cond, (pthread_mutex_t *) mutex) == 0;
}
