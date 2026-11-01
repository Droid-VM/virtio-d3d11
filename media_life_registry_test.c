#define _POSIX_C_SOURCE 200809L
#include "media_life_state.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>
/* Forced last-pin interleaving reproduces production ordering, NOT extracted
 * Win32 functions: shared lock/test/pin; exclusive remove; then refs predicate. */
static pthread_rwlock_t registry=PTHREAD_RWLOCK_INITIALIZER;
static atomic_int tested,allow_pin,remove_started,removed,release_reader,freed;
static atomic_long refs=1;
static void *reader(void *unused) {
    (void)unused;
    assert(!pthread_rwlock_rdlock(&registry));
    atomic_store(&tested,1); /* detached was false, but ref not incremented yet */
    while(!atomic_load(&allow_pin)) sched_yield();
    assert(!atomic_load(&freed)); atomic_fetch_add(&refs,1);
    assert(!pthread_rwlock_unlock(&registry));
    while(!atomic_load(&release_reader)) sched_yield();
    assert(!atomic_load(&freed));
    atomic_fetch_sub(&refs,1); /* final ctx access */
    return NULL;
}
static void *cleaner(void *unused) {
    (void)unused; atomic_store(&remove_started,1);
    assert(!pthread_rwlock_wrlock(&registry));
    atomic_store(&removed,1); /* no subsequent reader can obtain this ctx */
    assert(!pthread_rwlock_unlock(&registry));
    while(!ml_cleanup_ready(atomic_load(&refs),1)) sched_yield();
    atomic_store(&freed,1);
    return NULL;
}
int main(void) {
    pthread_t r,c;
    assert(!pthread_create(&r,NULL,reader,NULL));
    while(!atomic_load(&tested)) sched_yield();
    assert(!pthread_create(&c,NULL,cleaner,NULL));
    while(!atomic_load(&remove_started)) sched_yield();
    assert(!atomic_load(&removed) && !atomic_load(&freed));
    atomic_store(&allow_pin,1);
    while(!atomic_load(&removed)) sched_yield();
    assert(atomic_load(&refs)==2 && !atomic_load(&freed));
    atomic_store(&release_reader,1);
    assert(!pthread_join(r,NULL)); assert(!pthread_join(c,NULL));
    assert(atomic_load(&freed));
    puts("PASS forced registry late-pin/remove-before-ref-test ordering MODEL; not Win32 execution");
    return 0;
}
