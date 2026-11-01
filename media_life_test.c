#define _POSIX_C_SOURCE 200809L
#include "media_life_state.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* Helper fault model, not Windows/DDI execution. The production helper controls
 * deadline/admission/cleanup readiness and exact late mapping release identity. */
typedef struct Ctx {
    atomic_long refs;
    atomic_int detached,gate,done,closes,releases,module_refs;
    ML_MAP_LEASE late;
} Ctx;
static uint64_t now_ms(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static void pause_ms(unsigned n) { struct timespec t={n/1000,(long)(n%1000)*1000000}; nanosleep(&t,NULL); }
static void *worker(void *arg) {
    Ctx *c=arg;
    while(!atomic_load(&c->gate)) pause_ms(1); /* VM MAP blocking, cancellation fails */
    c->late=(ML_MAP_LEASE){37,123,9,1};
    assert(atomic_load(&c->detached));
    assert(ml_release_result(&c->late,0)==0 && c->late.owned); /* no early free */
    assert(c->late.index==37 && c->late.token==123 && c->late.generation==9);
    assert(ml_release_result(&c->late,1)); atomic_fetch_add(&c->releases,1);
    assert(!ml_release_result(&c->late,1)); /* exactly once */
    atomic_fetch_sub(&c->module_refs,1); /* helper model of atomic unload+exit */
    atomic_fetch_sub(&c->refs,1); /* last operation, no ctx access afterwards */
    return NULL;
}
static void *cleanup(void *arg) {
    Ctx *c=arg;
    while(!ml_cleanup_ready(atomic_load(&c->refs),atomic_load(&c->detached))) pause_ms(1);
    assert(!c->late.owned);
    atomic_fetch_add(&c->closes,1);
    atomic_fetch_sub(&c->module_refs,1);
    atomic_store(&c->done,1);
    return NULL;
}
int main(void) {
    Ctx *c=calloc(1,sizeof(*c)); pthread_t w,k;
    unsigned char *runtime=malloc(65536);
    uint64_t start,deadline;
    assert(c && runtime); memset(runtime,0x19,65536);
    atomic_init(&c->refs,3); atomic_init(&c->module_refs,2);
    assert(!pthread_create(&k,NULL,cleanup,c)); assert(!pthread_create(&w,NULL,worker,c));
    start=now_ms(); deadline=start+30;
    atomic_store(&c->detached,1);
    while(ml_remaining(now_ms(),deadline)) pause_ms(1); /* two waits share budget */
    assert(!ml_remaining(deadline+50,deadline));
    atomic_fetch_sub(&c->refs,1); /* owner drop, does not close */
    assert(now_ms()-start<200 && !atomic_load(&c->closes));
    memset(runtime,0xdd,65536); free(runtime); /* workers have no runtime pointer */
    pause_ms(5); assert(!atomic_load(&c->done) && atomic_load(&c->module_refs)==2);
    atomic_store(&c->gate,1);
    assert(!pthread_join(w,NULL)); assert(!pthread_join(k,NULL));
    assert(atomic_load(&c->done) && atomic_load(&c->closes)==1);
    assert(atomic_load(&c->releases)==1 && atomic_load(&c->module_refs)==0);
    assert(ml_admission_allowed(16,16) && !ml_admission_allowed(17,16));
    /* External read drop delayed before final RMW: cleanup cannot pass it. */
    assert(!ml_cleanup_ready(2,1)); assert(ml_cleanup_ready(1,1));
    free(c);
    puts("PASS LIFE helper: deadline/blocked MAP/poison storage/exact release/admission/ref cleanup/module lease MODEL; Windows unload NOT RUN");
    return 0;
}
