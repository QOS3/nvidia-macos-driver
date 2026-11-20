/* nvmtl_obj_memo.h - the same library, specialized function and compute pipeline handed back when an application asks for
   them again in one process. CMPhoto (every HEIC picture ImageIO decodes, so App Store, Photos, Safari, Finder previews)
   opens its default.metallib, specializes the same kernel and builds a compute pipeline from it once per picture. Each
   time this driver re-read the library, re-specialized the kernel and, at the first dispatch, re-translated a sampler
   variant and built another Vulkan pipeline: tens of milliseconds per picture on the thread that Core Animation's commit
   waits for. A fast scroll through App Store held its main thread 130-260 ms at a time, and those frames never came.
   All three objects are immutable once made (their lazily built parts are made under @synchronized), so the one made
   first serves every later request with the same inputs:
     library   - a file, keyed by path and the file's identity (device, inode, size, mtime, ctime): a rewritten file misses
     function  - a library object, a function name and the exact constant values (index, width, bytes)
     pipeline  - a function object, for newComputePipelineStateWithFunction: only. A pipeline from a descriptor carries
                 per-descriptor state set after creation (threadgroup limits, ICB support, label) and is never shared.
   Each table is bounded (oldest dropped first), so what an application stops using is freed. NVMTL_OBJ_MEMO=0 turns
   the three off. */
#ifndef NVMTL_OBJ_MEMO_H
#define NVMTL_OBJ_MEMO_H
#import <Foundation/Foundation.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdlib.h>

enum { NVMTL_MEMO_LIB = 0, NVMTL_MEMO_FN = 1, NVMTL_MEMO_CPS = 2, NVMTL_MEMO_KINDS = 3 };

static inline int nvmtl_obj_memo_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_OBJ_MEMO"); on = !(e && e[0] == '0'); }
    return on;
}

static os_unfair_lock gNvmtlMemoLock = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary *gNvmtlMemo[NVMTL_MEMO_KINDS];
static NSMutableArray *gNvmtlMemoOrder[NVMTL_MEMO_KINDS];
static _Atomic unsigned long gNvmtlMemoHits[NVMTL_MEMO_KINDS], gNvmtlMemoMiss[NVMTL_MEMO_KINDS];
static const NSUInteger kNvmtlMemoCap[NVMTL_MEMO_KINDS] = { 32, 256, 128 };
static const char *const kNvmtlMemoName[NVMTL_MEMO_KINDS] = { "library", "function", "compute pipeline" };

static inline id nvmtl_obj_memo_get(int kind, id<NSCopying> key)
{
    if (!key || !nvmtl_obj_memo_on()) return nil;
    id hit;
    os_unfair_lock_lock(&gNvmtlMemoLock);
    hit = gNvmtlMemo[kind][key];
    os_unfair_lock_unlock(&gNvmtlMemoLock);
    if (hit) {
        const unsigned long h = atomic_fetch_add(&gNvmtlMemoHits[kind], 1) + 1;
        if (!(h & (h - 1))) nvlog("object memo: %s served again %lu times (%lu made)", kNvmtlMemoName[kind], h, atomic_load(&gNvmtlMemoMiss[kind]));
    }
    return hit;
}

static inline void nvmtl_obj_memo_put(int kind, id<NSCopying> key, id obj)
{
    if (!key || !obj || !nvmtl_obj_memo_on()) return;
    atomic_fetch_add(&gNvmtlMemoMiss[kind], 1);
    id dropped = nil;   // released after the lock: dropping a pipeline can free Vulkan objects
    os_unfair_lock_lock(&gNvmtlMemoLock);
    if (!gNvmtlMemo[kind]) { gNvmtlMemo[kind] = [NSMutableDictionary new]; gNvmtlMemoOrder[kind] = [NSMutableArray new]; }
    if (!gNvmtlMemo[kind][key]) {
        if (gNvmtlMemoOrder[kind].count >= kNvmtlMemoCap[kind]) {
            id old = gNvmtlMemoOrder[kind][0];
            dropped = gNvmtlMemo[kind][old];
            [gNvmtlMemo[kind] removeObjectForKey:old]; [gNvmtlMemoOrder[kind] removeObjectAtIndex:0];
        }
        gNvmtlMemo[kind][key] = obj; [gNvmtlMemoOrder[kind] addObject:key];
    }
    os_unfair_lock_unlock(&gNvmtlMemoLock);
    dropped = nil;
}

/* a file's identity: a rewritten or replaced file never matches the old key */
static inline NSString *nvmtl_obj_memo_file_key(NSString *path, const struct stat *s)
{
    if (!path.length || !s) return nil;
    return [NSString stringWithFormat:@"%@|%llu|%llu|%lld|%ld.%ld|%ld.%ld", path, (unsigned long long)s->st_dev, (unsigned long long)s->st_ino,
            (long long)s->st_size, (long)s->st_mtimespec.tv_sec, (long)s->st_mtimespec.tv_nsec, (long)s->st_ctimespec.tv_sec, (long)s->st_ctimespec.tv_nsec];
}
#endif
