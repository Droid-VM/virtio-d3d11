/* Shared authorization protocol. State bit0 is terminal, bits1..30 count
 * authorized operations. CAS authorize is the IO authorization linearization
 * point; CAS complete is the result-commit linearization point. ma_fail sets
 * bit0 without waiting for IO. A commit ordered before fail is logically prior
 * even if its protected fields are written later; terminal read gates prohibit
 * observing those fields as a new success after failure. Never flush-reset. */
#ifndef MEDIA_ADMISSION_H
#define MEDIA_ADMISSION_H
#ifdef _WIN32
#include <windows.h>
typedef volatile LONG MA_TERMINAL;
static inline long ma_load(MA_TERMINAL *p) { return InterlockedCompareExchange(p,0,0); }
static inline long ma_cas(MA_TERMINAL *p,long old,long val) { return InterlockedCompareExchange(p,val,old); }
#else
#include <stdatomic.h>
typedef _Atomic long MA_TERMINAL;
static inline long ma_load(MA_TERMINAL *p) { return atomic_load(p); }
static inline long ma_cas(MA_TERMINAL *p,long old,long val) {
    atomic_compare_exchange_strong(p,&old,val); return old;
}
#endif
static inline int ma_terminal(MA_TERMINAL *p) { return (ma_load(p)&1)!=0; }
static inline void ma_fail(MA_TERMINAL *p) {
    long old; do { old=ma_load(p); } while(ma_cas(p,old,old|1)!=old);
}
static inline int ma_authorize(MA_TERMINAL *p) {
    long old;
    do { old=ma_load(p); if(old&1) return 0;
         if(old>=0x7ffffffcL) { ma_fail(p); return 0; } }
    while(ma_cas(p,old,old+2)!=old);
    return 1;
}
/* Always consumes exactly one authorization. Failure forbids success publication
 * but does not revoke IO buffers, capture ownership, pins or cleanup duties. */
static inline int ma_complete(MA_TERMINAL *p) {
    long old;
    do { old=ma_load(p); if(old<2) return 0; }
    while(ma_cas(p,old,old-2)!=old);
    return !(old&1);
}
enum { MA_REJECT, MA_SLOT, MA_WAIT, MA_EXPIRED };
static inline unsigned ma_capacity(int cancelled,int free_slot,unsigned left_ms) {
    if(cancelled) return MA_REJECT;
    if(free_slot) return MA_SLOT;
    return left_ms ? MA_WAIT : MA_EXPIRED;
}
#endif
