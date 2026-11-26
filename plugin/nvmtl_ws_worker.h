/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * nvmtl_ws_worker.h — Dedicated fixed-priority Mach threads for WindowServer's
 * submission, submit-ahead, callback, and presentation lanes (nitro-perf v10).
 *
 * Inside WindowServer (_windowserver), ws_main_thread and Apple's own
 * com.apple.windowserver.root_queue run at fixed priority 79R, whereas normal
 * libdispatch QOS_CLASS_USER_INTERACTIVE queues run on _pthread_wqthread at
 * timesharing priority 37T — lower than normal apps (47-54). Under CPU load,
 * ws_main_thread (79R) stalls in commitAndWaitUntilSubmitted / waitUntilScheduled
 * waiting on starved 37T worker threads. Persistent pthreads opted out of
 * timesharing (THREAD_EXTENDED_POLICY timeshare=0 + THREAD_PRECEDENCE_POLICY
 * importance=48, fallback 32) eliminate both priority inversion and wqthread
 * wake/teardown churn without hard RT watchdog risks.
 */
#ifndef NVMTL_WS_WORKER_H
#define NVMTL_WS_WORKER_H

#include <crt_externs.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Pure policy decision (host-tested): enabled by default in WindowServer,
 * overrideable via NVMTL_WS_HIPRIO=0/1. */
static inline int nvmtl_ws_hiprio_mode(const char *env, const char *prog)
{
    if (env && env[0]) return env[0] != '0';
    return (prog && !strcmp(prog, "WindowServer")) ? 1 : 0;
}

/* Extended policy (nitro-perf v12):
 *   0 = disabled (normal app/renderer — uses libdispatch)
 *   1 = WindowServer (fixed-priority importance=48 -> 79R)
 *   2 = Browser/Electron GPU helper process (--type=gpu-process, Firefox GPU Helper,
 *       WebKit.GPU, *Helper (GPU)) -> fixed-priority importance=32 -> 63R so GPU
 *       command completion never starves behind 100% CPU PDFium/JS renderer threads,
 *       while staying strictly below WindowServer (79R). */
static inline int nvmtl_gpu_helper_hiprio_mode(const char *env, const char *prog,
                                               int argc, const char *const *argv)
{
    if (env && env[0]) {
        if (env[0] == '0') return 0;
        return (prog && !strcmp(prog, "WindowServer")) ? 1 : 2;
    }
    if (prog && !strcmp(prog, "WindowServer")) return 1;
    if (prog && (!strcmp(prog, "Firefox GPU Helper") ||
                 !strcmp(prog, "com.apple.WebKit.GPU") ||
                 strstr(prog, "Helper (GPU)") != NULL))
        return 2;
    if (argv && argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (argv[i] && !strcmp(argv[i], "--type=gpu-process")) return 2;
        }
    }
    return 0;
}

/* Opt the calling dedicated pthread out of timesharing and raise its Mach
 * precedence to fixed-priority (`importance` = 48 -> up to 79R in WindowServer,
 * 32 -> 63R in GPU helpers). Returns 0 if fixed-priority policy was accepted. */
static inline int nvmtl_ws_boost_current_thread_pri(const char *name, int importance)
{
    if (name && name[0]) pthread_setname_np(name);
    const thread_t self = mach_thread_self();
    thread_extended_policy_data_t ext;
    ext.timeshare = FALSE;
    const kern_return_t krExt = thread_policy_set(
        self, THREAD_EXTENDED_POLICY, (thread_policy_t)&ext, THREAD_EXTENDED_POLICY_COUNT);
    thread_precedence_policy_data_t prec;
    prec.importance = importance > 0 ? importance : 32;
    kern_return_t krPrec = thread_policy_set(
        self, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&prec, THREAD_PRECEDENCE_POLICY_COUNT);
    if (krPrec != KERN_SUCCESS && prec.importance > 32) {
        prec.importance = 32;
        krPrec = thread_policy_set(
            self, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&prec, THREAD_PRECEDENCE_POLICY_COUNT);
    }
    mach_port_deallocate(mach_task_self(), self);
    return (krExt == KERN_SUCCESS && krPrec == KERN_SUCCESS) ? 0 : -1;
}

static inline int nvmtl_ws_boost_current_thread(const char *name)
{
    return nvmtl_ws_boost_current_thread_pri(name, 48);
}

/* ---------- 1. Coalescing fixed-priority kick worker ---------- */

typedef struct nvmtl_ws_kick {
    pthread_t thr;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    void (*fn)(void *);
    void *ctx;
    const char *name;
    int pending;
    int stop;
    int started;
    int importance;
} nvmtl_ws_kick_t;

static inline void *nvmtl_ws_kick_main(void *arg)
{
    nvmtl_ws_kick_t *w = (nvmtl_ws_kick_t *)arg;
    (void)nvmtl_ws_boost_current_thread_pri(w->name, w->importance > 0 ? w->importance : 48);
    pthread_mutex_lock(&w->mu);
    for (;;) {
        while (!w->pending && !w->stop) pthread_cond_wait(&w->cv, &w->mu);
        if (!w->pending && w->stop) break;
        w->pending = 0;
        pthread_mutex_unlock(&w->mu);
        w->fn(w->ctx);
        pthread_mutex_lock(&w->mu);
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

static inline int nvmtl_ws_kick_init_pri(nvmtl_ws_kick_t *w, const char *name, void (*fn)(void *), void *ctx, int importance)
{
    if (!w || !fn) return -1;
    memset(w, 0, sizeof *w);
    w->fn = fn;
    w->ctx = ctx;
    w->name = name;
    w->importance = importance > 0 ? importance : 48;
    if (pthread_mutex_init(&w->mu, NULL) != 0) return -1;
    if (pthread_cond_init(&w->cv, NULL) != 0) {
        pthread_mutex_destroy(&w->mu);
        return -1;
    }
    if (pthread_create(&w->thr, NULL, nvmtl_ws_kick_main, w) != 0) {
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        return -1;
    }
    w->started = 1;
    return 0;
}

static inline int nvmtl_ws_kick_init(nvmtl_ws_kick_t *w, const char *name, void (*fn)(void *), void *ctx)
{
    return nvmtl_ws_kick_init_pri(w, name, fn, ctx, 48);
}

static inline void nvmtl_ws_kick_signal(nvmtl_ws_kick_t *w)
{
    if (!w || !w->started) return;
    pthread_mutex_lock(&w->mu);
    w->pending = 1;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static inline void nvmtl_ws_kick_destroy(nvmtl_ws_kick_t *w)
{
    if (!w || !w->started) return;
    pthread_mutex_lock(&w->mu);
    w->stop = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thr, NULL);
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    w->started = 0;
}

/* ---------- 2. FIFO fixed-priority serial task queue ---------- */

#define NVMTL_WS_SERIAL_SLAB 64

typedef struct nvmtl_ws_task {
    void (*fn)(void *);
    void *arg;
    struct nvmtl_ws_task *next;
    int from_slab;
} nvmtl_ws_task_t;

typedef struct nvmtl_ws_serial {
    pthread_t thr;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    const char *name;
    nvmtl_ws_task_t *head;
    nvmtl_ws_task_t *tail;
    nvmtl_ws_task_t *free_slab;
    nvmtl_ws_task_t slab[NVMTL_WS_SERIAL_SLAB];
    int stopping;
    int self_free;
    int importance;
} nvmtl_ws_serial_t;

static inline void *nvmtl_ws_serial_main(void *arg)
{
    nvmtl_ws_serial_t *q = (nvmtl_ws_serial_t *)arg;
    (void)nvmtl_ws_boost_current_thread_pri(q->name, q->importance > 0 ? q->importance : 48);
    int do_self_free = 0;
    pthread_mutex_lock(&q->mu);
    for (;;) {
        while (!q->head && !q->stopping) pthread_cond_wait(&q->cv, &q->mu);
        nvmtl_ws_task_t *node = q->head;
        if (!node) {
            do_self_free = q->self_free;
            break;
        }
        q->head = node->next;
        if (!q->head) q->tail = NULL;
        void (*fn)(void *) = node->fn;
        void *targ = node->arg;
        if (node->from_slab) {
            node->next = q->free_slab;
            q->free_slab = node;
        } else {
            free(node);
        }
        pthread_mutex_unlock(&q->mu);
        fn(targ);
        pthread_mutex_lock(&q->mu);
    }
    pthread_mutex_unlock(&q->mu);
    if (do_self_free) {
        pthread_cond_destroy(&q->cv);
        pthread_mutex_destroy(&q->mu);
        free(q);
    }
    return NULL;
}

static inline nvmtl_ws_serial_t *nvmtl_ws_serial_create_pri(const char *name, int importance)
{
    nvmtl_ws_serial_t *q = (nvmtl_ws_serial_t *)calloc(1, sizeof *q);
    if (!q) return NULL;
    q->name = name;
    q->importance = importance > 0 ? importance : 48;
    for (int i = 0; i < NVMTL_WS_SERIAL_SLAB; i++) {
        q->slab[i].from_slab = 1;
        q->slab[i].next = (i + 1 < NVMTL_WS_SERIAL_SLAB) ? &q->slab[i + 1] : NULL;
    }
    q->free_slab = &q->slab[0];
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->cv, NULL) != 0) {
        pthread_mutex_destroy(&q->mu);
        free(q);
        return NULL;
    }
    if (pthread_create(&q->thr, NULL, nvmtl_ws_serial_main, q) != 0) {
        pthread_cond_destroy(&q->cv);
        pthread_mutex_destroy(&q->mu);
        free(q);
        return NULL;
    }
    return q;
}

static inline nvmtl_ws_serial_t *nvmtl_ws_serial_create(const char *name)
{
    return nvmtl_ws_serial_create_pri(name, 48);
}

static inline int nvmtl_ws_serial_enqueue(nvmtl_ws_serial_t *q, void (*fn)(void *), void *arg)
{
    if (!q || !fn) return -1;
    pthread_mutex_lock(&q->mu);
    if (q->stopping) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    nvmtl_ws_task_t *node = q->free_slab;
    if (node) {
        q->free_slab = node->next;
    } else {
        node = (nvmtl_ws_task_t *)malloc(sizeof *node);
        if (!node) {
            pthread_mutex_unlock(&q->mu);
            return -1;
        }
        node->from_slab = 0;
    }
    node->fn = fn;
    node->arg = arg;
    node->next = NULL;
    if (q->tail) q->tail->next = node;
    else q->head = node;
    q->tail = node;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

/* Drains all already-queued tasks in FIFO order and stops the thread. Safe even
 * if invoked from inside a task running on the queue's own worker thread. */
static inline void nvmtl_ws_serial_destroy(nvmtl_ws_serial_t *q)
{
    if (!q) return;
    const int on_self = pthread_equal(pthread_self(), q->thr);
    pthread_mutex_lock(&q->mu);
    q->stopping = 1;
    if (on_self) q->self_free = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    if (on_self) {
        pthread_detach(q->thr);
        return;
    }
    pthread_join(q->thr, NULL);
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mu);
    free(q);
}

#endif /* NVMTL_WS_WORKER_H */
