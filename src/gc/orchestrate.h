void MVM_gc_enter_from_allocator(MVMThreadContext *tc);
void MVM_gc_enter_from_interrupt(MVMThreadContext *tc);
void MVM_gc_mark_thread_blocked(MVMThreadContext *tc);
void MVM_gc_mark_thread_unblocked(MVMThreadContext *tc);
void MVM_gc_global_destruction(MVMThreadContext *tc);
void MVM_gc_register_child_thread(MVMThreadContext *tc, MVMThreadContext *child_tc);

struct MVMWorkThread {
    MVMThreadContext *tc;
    void             *limit;
};

typedef enum {
    MVM_GC_DEBUG_ORCHESTRATE = 1,
    MVM_GC_DEBUG_COLLECT = 2,
    MVM_GC_DEBUG_GEN2 = 4,
    MVM_GC_DEBUG_PASSEDWORK = 8,
    MVM_GC_DEBUG_GCSTART = 16,
/*    MVM_GC_DEBUG_ = 32,
    MVM_GC_DEBUG_ = 64,
    MVM_GC_DEBUG_ = 128,
    MVM_GC_DEBUG_ = 256,
    MVM_GC_DEBUG_ = 512,
    MVM_GC_DEBUG_ = 1024,
    MVM_GC_DEBUG_ = 2048,
    MVM_GC_DEBUG_ = 4096,
    MVM_GC_DEBUG_ = 8192,
    MVM_GC_DEBUG_ = 16384,
    MVM_GC_DEBUG_ = 32768,
    MVM_GC_DEBUG_ = 65536,
    MVM_GC_DEBUG_ = 131072,
    MVM_GC_DEBUG_ = 262144,
    MVM_GC_DEBUG_ = 524288,
    MVM_GC_DEBUG_ = 1048576,
    MVM_GC_DEBUG_ = 2097152,
    MVM_GC_DEBUG_ = 4194304,
    MVM_GC_DEBUG_ = 8388608,
    MVM_GC_DEBUG_ = 16777216,
    MVM_GC_DEBUG_ = 33554432,
    MVM_GC_DEBUG_ = 67108864,
    MVM_GC_DEBUG_ = 134217728*/
} MVMGCDebugLogFlags;

/* OR together the flags you want to require, or redefine
 * MVM_GC_DEBUG_ENABLED(flags) if you want something more
 * complicated. */
#define MVM_GC_DEBUG_LOG_FLAGS 0
/*  ( MVM_GC_DEBUG_ORCHESTRATE \
    | MVM_GC_DEBUG_PASSEDWORK \
    | MVM_GC_DEBUG_COLLECT \
    | MVM_GC_DEBUG_GEN2 \
    | MVM_GC_DEBUG_GCSTART \
    )*/

#define MVM_GC_DEBUG_ENABLED(flags) \
    ((MVM_GC_DEBUG_LOG_FLAGS) & (flags))

/* Pass 0 as the first parameter if you don't want it to prefix the thread id
 * and gc run details. */
#ifdef _MSC_VER
# define GCDEBUG_LOG(tc, flags, msg, ...) do { \
    if (MVM_GC_DEBUG_ENABLED(flags)) { \
        if (tc) printf(("Thread %d run %d : " msg), (tc)->thread_id, \
            MVM_load(&(tc)->instance->gc_seq_number), __VA_ARGS__); \
        else printf((msg), __VA_ARGS__); \
    } \
} while (0)
#else
# define GCDEBUG_LOG(tc, flags, msg, ...) do { \
    if (MVM_GC_DEBUG_ENABLED(flags)) { \
        if (tc) printf(("Thread %d run %d : " msg), (tc)->thread_id, \
                MVM_load(&(tc)->instance->gc_seq_number) , ##__VA_ARGS__); \
        else printf((msg) , ##__VA_ARGS__); \
    } \
} while (0)
#endif

#define MVM_try_transition(tc, other, tmp, state0, state1) \
    ((tmp = MVM_cas(&other->gc_status, state0, state1)) == state0)

#define MVM_try_steal(tc, other, tmp) \
    MVM_try_transition(tc, other, tmp, MVMGCStatus_UNABLE, MVMGCStatus_STOLEN)

#define MVM_try_interrupt(tc, other, tmp) \
    MVM_try_transition(tc, other, tmp, MVMGCStatus_NONE, MVMGCStatus_INTERRUPT)

#define MVM_try_uninterrupt(tc, other, tmp) \
    MVM_try_transition(tc, other, tmp, MVMGCStatus_INTERRUPT, MVMGCStatus_NONE)

#define MVM_try_mark_blocked(tc, other, tmp) \
    MVM_try_transition(tc, other, tmp, MVMGCStatus_NONE, MVMGCStatus_UNABLE)

#define MVM_try_mark_unblocked(tc, other, tmp) \
    MVM_try_transition(tc, other, tmp, MVMGCStatus_UNABLE, MVMGCStatus_NONE)
