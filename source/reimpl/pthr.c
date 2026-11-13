/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <errno.h>
#include "reimpl/pthr.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"


#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

static SceKernelLwMutexWork pthr_mutex;
static volatile short int pthr_mutex_inited = 0;

#define PTHR_LOCK \
    if (!pthr_mutex_inited) { \
        int ret = sceKernelCreateLwMutex(&pthr_mutex, "pthr_lock", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL); \
        if (ret < 0) { \
            sceClibPrintf("Error: failed to create pthr mutex: 0x%x\n", ret); \
            return 0; \
        } \
        pthr_mutex_inited = 1; \
    } \
    sceKernelLockLwMutex(&pthr_mutex, 1, NULL);

#define PTHR_UNLOCK \
    if (pthr_mutex_inited) { \
        sceKernelUnlockLwMutex(&pthr_mutex, 1); \
    }

/*
 * Bionic mutexes/conds are 4 bytes, so they only hold a pointer to the newlib
 * object (real_ptr). This table records which game objects (by address) we
 * initialized and with which real_ptr. It grows as needed: the TT engine
 * creates far more than a fixed-size list held, and a full list made every
 * lock create a brand-new mutex (no mutual exclusion, cond waits on the wrong
 * mutex, hangs). Comparing real_ptr also catches memory that was freed and
 * reused for a new object without pthread_*_destroy.
 */
typedef struct {
    const void *obj;  // NULL: empty, (void*)1: deleted
    void *real;
} pthr_entry;

static pthr_entry *pthr_table = NULL;
static uint32_t pthr_cap = 0, pthr_used = 0;

#define PTHR_DELETED ((const void *) 1)

static inline uint32_t pthr_hash(const void *p) {
    return ((uint32_t)(uintptr_t) p >> 2) * 2654435761u;
}

static pthr_entry *pthr_find(const void *obj) {
    if (!pthr_cap) return NULL;
    uint32_t i = pthr_hash(obj) & (pthr_cap - 1);
    for (uint32_t n = 0; n < pthr_cap; n++) {
        pthr_entry *e = &pthr_table[i];
        if (e->obj == NULL) return NULL;
        if (e->obj == obj) return e;
        i = (i + 1) & (pthr_cap - 1);
    }
    return NULL;
}

static void pthr_insert(const void *obj, void *real);

static void pthr_grow(void) {
    pthr_entry *old = pthr_table;
    uint32_t old_cap = pthr_cap;
    pthr_cap = old_cap ? old_cap * 2 : 4096;
    pthr_table = calloc(pthr_cap, sizeof(pthr_entry));
    pthr_used = 0;
    for (uint32_t i = 0; i < old_cap; i++)
        if (old[i].obj && old[i].obj != PTHR_DELETED)
            pthr_insert(old[i].obj, old[i].real);
    free(old);
}

static void pthr_insert(const void *obj, void *real) {
    if ((pthr_used + 1) * 2 > pthr_cap) pthr_grow();
    uint32_t i = pthr_hash(obj) & (pthr_cap - 1);
    pthr_entry *tomb = NULL;
    while (pthr_table[i].obj) {
        if (pthr_table[i].obj == obj) {
            pthr_table[i].real = real;
            return;
        }
        if (pthr_table[i].obj == PTHR_DELETED && !tomb) tomb = &pthr_table[i];
        i = (i + 1) & (pthr_cap - 1);
    }
    pthr_entry *e = tomb ? tomb : &pthr_table[i];
    if (!tomb) pthr_used++;
    e->obj = obj;
    e->real = real;
}

int isObjectInitialized(const void * mut) {
    PTHR_LOCK
    pthr_entry *e = pthr_find(mut);
    int r = e && e->real && e->real == *(void * const *) mut;
    PTHR_UNLOCK
    return r;
}

int rememberObject(void * mut) {
    PTHR_LOCK
    pthr_insert(mut, *(void **) mut);
    PTHR_UNLOCK
    return 1;
}

int forgetObject(const void * mut) {
    PTHR_LOCK
    pthr_entry *e = pthr_find(mut);
    if (e) {
        e->obj = PTHR_DELETED;
        e->real = NULL;
    }
    PTHR_UNLOCK
    return e != NULL;
}

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

/*
 * Bionic's pthread_mutexattr_t is a plain 32-bit value: the mutex type sits in
 * the low 4 bits (0 normal, 1 recursive, 2 errorcheck), bit 4 is "shared".
 * newlib's is a pointer to a struct, so the game's attributes must never be
 * handed to newlib's pthread_mutexattr_* functions.
 */
#define BIONIC_MUTEXATTR_TYPE_MASK 0xf

PTHR_INLINE int bionic_mutex_kind(int attr) {
    switch (attr & BIONIC_MUTEXATTR_TYPE_MASK) {
        case 1: return PTHREAD_MUTEX_RECURSIVE;
        case 2: return PTHREAD_MUTEX_ERRORCHECK;
        default: return PTHREAD_MUTEX_NORMAL;
    }
}

// null check for `mutex` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _mutex_t_static_init(pthread_mutex_t_bionic * mutex, const pthread_mutexattr_t * attr) {
    int ret = 0, kind = PTHREAD_MUTEX_NORMAL;

    // Check and create under one lock: two threads locking the same
    // statically initialized mutex must end up with the same real mutex.
    PTHR_LOCK
    if (isObjectInitialized(mutex)) {
        PTHR_UNLOCK
        return ret;
    }

    if (attr) {
        kind = bionic_mutex_kind(*(const int *) attr);
    } else {
        if (* (int *) mutex == BIONIC_PTHREAD_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_NORMAL;
        else if (* (int *) mutex == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (* (int *) mutex == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
    }

    pthread_mutex_t mut;
    mutex->real_ptr = malloc(sizeof(pthread_mutex_t));
    sceClibMemcpy(mutex->real_ptr, &mut, sizeof(pthread_mutex_t));

    pthread_mutexattr_t mutattr;
    pthread_mutexattr_init(&mutattr);
    pthread_mutexattr_settype(&mutattr, kind);
    ret = pthread_mutex_init(mutex->real_ptr, &mutattr);
    pthread_mutexattr_destroy(&mutattr);

    if (ret == 0) {
        rememberObject(mutex);
    } else {
        l_error("mutex initialization for %p has failed", mutex);
    }
    PTHR_UNLOCK

    return ret;
}

// null check for `cond` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    int ret = 0;

    PTHR_LOCK
    if (isObjectInitialized(cond)) {
        PTHR_UNLOCK
        return ret;
    }

    pthread_cond_t c;
    cond->real_ptr = malloc(sizeof(pthread_cond_t));
    sceClibMemcpy(cond->real_ptr, &c, sizeof(pthread_cond_t));

    ret = pthread_cond_init(cond->real_ptr, attr);

    if (ret == 0) {
        rememberObject(cond);
    } else {
        l_error("cond initialization for %p has failed", cond);
    }
    PTHR_UNLOCK

    return ret;
}

#include "na_threads.h"

typedef struct { void *(*start)(void *); void *param; } na_tramp;

static void *thread_tramp(void *p) {
    na_tramp t = *(na_tramp *) p;
    free(p);
    na_thread_register("pthread", (void *)t.start);
    return t.start(t.param);
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start_game)(void *), void *param_game) {
    int ret;
    na_tramp *tr = malloc(sizeof(*tr));
    tr->start = start_game;
    tr->param = param_game;
    void *(*start)(void *) = thread_tramp;
    void *param = tr;

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 256 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        // 256 KB .. 1 MB: Unity starts ~20 threads, and every stack is real
        // memory on the Vita (no lazily committed pages)
        size_t ss = 0, want;
        pthread_attr_getstacksize(attr->real_ptr, &ss);
        want = ss < 256 * 1024 ? 256 * 1024 : (ss > 1024 * 1024 ? 1024 * 1024 : ss);
        pthread_attr_setstacksize(attr->real_ptr, want);
        ret = pthread_create(thread, attr->real_ptr, start, param);
        if (ret != 0 && want > 256 * 1024) {
            // out of memory: retry with the minimum
            pthread_attr_setstacksize(attr->real_ptr, 256 * 1024);
            ret = pthread_create(thread, attr->real_ptr, start, param);
        }
        l_info("pthread_create(start: %p, stack %u KB requested, %u KB used): %i", start_game,
               (unsigned)(ss / 1024), (unsigned)(want / 1024), ret);
    }

    if (ret != 0) free(tr);
    if (!attr) l_info("pthread_create(start: %p, param: %p): %i", start_game, param_game, ret);
    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    if (!attr) return EINVAL;
    *(int *) attr = 0; // normal, private
    return 0;
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    if (!attr || type < 0 || type > 2) return EINVAL;
    *(int *) attr = (*(int *) attr & ~BIONIC_MUTEXATTR_TYPE_MASK) | type;
    return 0;
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    if (!attr) return EINVAL;
    *(int *) attr = -1;
    return 0;
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    // pthread_mutex_init always creates a fresh mutex, even if this memory
    // held one before (freed and reused without pthread_mutex_destroy).
    forgetObject(uid);
    uid->real_ptr = NULL;
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    forgetObject(mutex);
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    if (mutex->real_ptr) free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    _mutex_t_static_init(mutex, NULL);
    if (pthread_mutex_trylock(mutex->real_ptr) == 0) return 0;
    int r = pthread_mutex_lock(mutex->real_ptr);
    return r;
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    _mutex_t_static_init(mutex, NULL);
    return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (!mutex->real_ptr) return EINVAL;
    return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    int r = pthread_join(thread, value_ptr);
    return r;
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    // Always a fresh condition variable, see pthread_mutex_init_soloader.
    forgetObject(cond);
    cond->real_ptr = NULL;
    return _cond_t_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    forgetObject(cond);
    int ret = pthread_cond_destroy(cond->real_ptr);
    if (cond->real_ptr) free(cond->real_ptr);
    cond->real_ptr = 0x0;
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    _cond_t_static_init(cond, NULL);

    return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;

    _cond_t_static_init(cond, NULL);
    _mutex_t_static_init(mutex, NULL);

    int r = pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
    return r;
}


int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    _cond_t_static_init(cond, NULL);
    _mutex_t_static_init(mutex, NULL);

    int r = pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
    return r;
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    _cond_t_static_init(cond, NULL);

    return pthread_cond_broadcast(cond->real_ptr);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    state = !state; // pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC...
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
    // Android priorities computed from sched_get_priority_min/max don't map
    // onto Vita priorities: a raised worker thread (e.g. the sound thread)
    // can starve the main thread on the same core. Keep the default priority.
    l_info("pthread_setschedparam(policy %i, priority %i): ignored", policy, param ? param->sched_priority : -1);
    return 0;
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;
    if (__sync_lock_test_and_set(once_control, 1) == 0)
        (*init_routine)();
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    l_info("pthread_setname_np(%s)", thread_name);
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    if (!sval) sval = malloc(sizeof(int32_t));
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    if (!abstime) return -1;
    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    if (_timeout-now >= 0) return -1;
    uint timeout_real = _timeout - now;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0)
        return -1;
    return 0;
}

int sem_trywait_soloader (int * uid) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    int r = sceKernelWaitSema(*uid, 1, NULL);
    if (r < 0)
        return -1;
    return 0;
}
