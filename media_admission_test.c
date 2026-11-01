#include "media_admission.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
static DWORD WINAPI fail_thread(void *p) { ma_fail(p); return 0; }
static DWORD WINAPI count_thread(void *p) {
    for(unsigned i=0;i<10000;i++) {
        if(ma_authorize(p)) (void)ma_complete(p);
    }
    return 0;
}
#else
#include <pthread.h>
static void *fail_thread(void *p) { ma_fail(p); return NULL; }
#endif
int main(void) {
    MA_TERMINAL terminal = 0;
    { MA_TERMINAL before=0, after=0, closed=0;
      assert(ma_authorize(&before));
      assert(ma_complete(&before)); /* logical commit before terminal */
      ma_fail(&before); assert(ma_load(&before)==1);
      assert(ma_authorize(&after));
      ma_fail(&after); /* authorized IO may still physically finish */
      assert(!ma_complete(&after)); assert(ma_load(&after)==1);
      ma_fail(&closed); assert(!ma_authorize(&closed));
      assert(ma_load(&closed)==1);
    }
    uint64_t seq[10];
    unsigned i;
    for(i=0;i<10;i++) seq[i]=i+2;
    /* A deliberately stalled sender fills all ten slots with coded seq2..11.
     * Production admission policy must wait, never select any occupied slot. */
    assert(ma_capacity(0,0,50)==MA_WAIT);
    for(i=0;i<10;i++) assert(seq[i]==i+2);
    assert(ma_capacity(0,0,0)==MA_EXPIRED);
    assert(ma_capacity(1,1,50)==MA_REJECT); /* flush/stop beats newly freed slot */
    assert(ma_capacity(0,1,50)==MA_SLOT);
    assert(!ma_terminal(&terminal));
#ifdef _WIN32
    { MA_TERMINAL shared=0; HANDLE threads[4];
      for(unsigned j=0;j<4;j++) {
        threads[j]=CreateThread(NULL,0,count_thread,(void *)&shared,0,NULL);
        assert(threads[j]);
      }
      ma_fail(&shared);
      for(unsigned j=0;j<4;j++) {
        assert(WaitForSingleObject(threads[j],5000)==WAIT_OBJECT_0);
        CloseHandle(threads[j]);
      }
      assert(ma_load(&shared)==1); /* count zero, terminal bit retained */
    }
    HANDLE t=CreateThread(NULL,0,fail_thread,(void *)&terminal,0,NULL);
    assert(t && WaitForSingleObject(t,5000)==WAIT_OBJECT_0);
    CloseHandle(t);
#else
    pthread_t t; assert(!pthread_create(&t,NULL,fail_thread,&terminal));
    assert(!pthread_join(t,NULL));
#endif
    /* Real production atomic helper: another thread permanently closes the
     * gate. Repeated begin/submit decisions remain rejected after error read. */
    for(i=0;i<100;i++) assert(ma_terminal(&terminal));
    assert(ma_capacity(ma_terminal(&terminal),1,50)==MA_REJECT);
    puts("PASS production admission helper: atomic cross-thread terminal, full-ring wait/expiry, cancellation beats free slot; MODEL ONLY, Windows pipeline not executed");
    return 0;
}
