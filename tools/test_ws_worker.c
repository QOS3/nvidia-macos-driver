/*
 * Host unit test for plugin/nvmtl_ws_worker.h (nitro-perf v10).
 * Build: cc -O2 -Wall -Wextra -Werror -Iplugin tools/test_ws_worker.c -lpthread -o /tmp/test_ws_worker && /tmp/test_ws_worker
 */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "nvmtl_ws_worker.h"

static int g_fails;
static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) g_fails++;
}

typedef struct {
    atomic_int runs;
    atomic_int last_timeshare;
    atomic_int last_importance;
    atomic_int target_val;
    atomic_int observed_val;
} kick_ctx_t;

static void kick_fn(void *arg)
{
    kick_ctx_t *c = (kick_ctx_t *)arg;
    const int v = atomic_load_explicit(&c->target_val, memory_order_acquire);
    atomic_store_explicit(&c->observed_val, v, memory_order_release);
    atomic_fetch_add_explicit(&c->runs, 1, memory_order_acq_rel);

    thread_t self = mach_thread_self();
    thread_extended_policy_data_t ext;
    mach_msg_type_number_t cnt = THREAD_EXTENDED_POLICY_COUNT;
    boolean_t get_def = FALSE;
    if (thread_policy_get(self, THREAD_EXTENDED_POLICY, (thread_policy_t)&ext, &cnt, &get_def) == KERN_SUCCESS)
        atomic_store(&c->last_timeshare, ext.timeshare);

    thread_precedence_policy_data_t prec;
    cnt = THREAD_PRECEDENCE_POLICY_COUNT;
    get_def = FALSE;
    if (thread_policy_get(self, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&prec, &cnt, &get_def) == KERN_SUCCESS)
        atomic_store(&c->last_importance, prec.importance);
    mach_port_deallocate(mach_task_self(), self);
}

typedef struct {
    atomic_int seq;
    atomic_int order_ok;
    atomic_int done;
    atomic_int timeshare;
    atomic_int importance;
} fifo_state_t;

typedef struct {
    fifo_state_t *st;
    int idx;
} fifo_item_t;

static void fifo_fn(void *arg)
{
    fifo_item_t *it = (fifo_item_t *)arg;
    const int prev = atomic_fetch_add_explicit(&it->st->seq, 1, memory_order_acq_rel);
    if (prev != it->idx) atomic_store(&it->st->order_ok, 0);
    if (it->idx == 0) {
        thread_t self = mach_thread_self();
        thread_extended_policy_data_t ext;
        mach_msg_type_number_t cnt = THREAD_EXTENDED_POLICY_COUNT;
        boolean_t get_def = FALSE;
        if (thread_policy_get(self, THREAD_EXTENDED_POLICY, (thread_policy_t)&ext, &cnt, &get_def) == KERN_SUCCESS)
            atomic_store(&it->st->timeshare, ext.timeshare);
        thread_precedence_policy_data_t prec;
        cnt = THREAD_PRECEDENCE_POLICY_COUNT;
        get_def = FALSE;
        if (thread_policy_get(self, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&prec, &cnt, &get_def) == KERN_SUCCESS)
            atomic_store(&it->st->importance, prec.importance);
        mach_port_deallocate(mach_task_self(), self);
    }
    atomic_fetch_add_explicit(&it->st->done, 1, memory_order_release);
}

typedef struct {
    nvmtl_ws_serial_t *q;
    atomic_int ran;
} self_destroy_ctx_t;

static void self_destroy_fn(void *arg)
{
    self_destroy_ctx_t *c = (self_destroy_ctx_t *)arg;
    nvmtl_ws_serial_destroy(c->q);
    atomic_store_explicit(&c->ran, 1, memory_order_release);
}

int main(void)
{
    /* 1. Policy selection */
    check("default ON in WindowServer", nvmtl_ws_hiprio_mode(NULL, "WindowServer") == 1);
    check("default OFF in regular app", nvmtl_ws_hiprio_mode(NULL, "Firefox") == 0);
    check("default OFF when prog is NULL", nvmtl_ws_hiprio_mode(NULL, NULL) == 0);
    check("NVMTL_WS_HIPRIO=0 disables in WindowServer", nvmtl_ws_hiprio_mode("0", "WindowServer") == 0);
    check("NVMTL_WS_HIPRIO=1 enables in regular app", nvmtl_ws_hiprio_mode("1", "Telegram") == 1);

    /* 2. Kick worker: Mach fixed-priority boost + coalescing without lost final wakeup */
    kick_ctx_t kctx;
    atomic_init(&kctx.runs, 0);
    atomic_init(&kctx.last_timeshare, -1);
    atomic_init(&kctx.last_importance, -1);
    atomic_init(&kctx.target_val, 0);
    atomic_init(&kctx.observed_val, 0);
    nvmtl_ws_kick_t kw;
    check("kick worker init", nvmtl_ws_kick_init(&kw, "nvmtl.test.kick", kick_fn, &kctx) == 0);
    for (int i = 1; i <= 500; i++) {
        atomic_store_explicit(&kctx.target_val, i, memory_order_release);
        nvmtl_ws_kick_signal(&kw);
    }
    nvmtl_ws_kick_destroy(&kw);
    check("kick worker ran at least once", atomic_load(&kctx.runs) >= 1);
    check("kick worker observed final value 500 (no lost wake)", atomic_load(&kctx.observed_val) == 500);
    check("kick worker opted out of timesharing (timeshare==0)", atomic_load(&kctx.last_timeshare) == 0);
    check("kick worker raised Mach precedence (>=32)", atomic_load(&kctx.last_importance) >= 32);

    /* 3. Serial worker: 512 FIFO tasks (slab + heap overflow) + Mach fixed-priority */
    enum { N = 512 };
    fifo_state_t fst;
    atomic_init(&fst.seq, 0);
    atomic_init(&fst.order_ok, 1);
    atomic_init(&fst.done, 0);
    atomic_init(&fst.timeshare, -1);
    atomic_init(&fst.importance, -1);
    fifo_item_t items[N];
    nvmtl_ws_serial_t *sq = nvmtl_ws_serial_create("nvmtl.test.serial");
    check("serial worker create", sq != NULL);
    int enq_ok = 1;
    for (int i = 0; i < N; i++) {
        items[i].st = &fst;
        items[i].idx = i;
        if (nvmtl_ws_serial_enqueue(sq, fifo_fn, &items[i]) != 0) enq_ok = 0;
    }
    check("serial enqueue 512 tasks (exercises slab + heap)", enq_ok);
    nvmtl_ws_serial_destroy(sq);
    check("serial drained all 512 tasks before destroy returned", atomic_load(&fst.done) == N);
    check("serial preserved strict FIFO order", atomic_load(&fst.order_ok) == 1);
    check("serial worker fixed-priority (timeshare==0)", atomic_load(&fst.timeshare) == 0);
    check("serial worker precedence >=32", atomic_load(&fst.importance) >= 32);

    /* 4. Self-destroy from inside callback (must not deadlock on pthread_join) */
    nvmtl_ws_serial_t *sq2 = nvmtl_ws_serial_create("nvmtl.test.self");
    self_destroy_ctx_t sctx;
    sctx.q = sq2;
    atomic_init(&sctx.ran, 0);
    check("self-destroy enqueue", nvmtl_ws_serial_enqueue(sq2, self_destroy_fn, &sctx) == 0);
    for (int t = 0; t < 200 && !atomic_load_explicit(&sctx.ran, memory_order_acquire); t++) usleep(5000);
    check("self-destroy completed without deadlock", atomic_load(&sctx.ran) == 1);
    usleep(20000); /* allow detached worker exit/free */

    printf("%s (%d failures)\n", g_fails ? "FAIL" : "PASS", g_fails);
    return g_fails ? 1 : 0;
}
