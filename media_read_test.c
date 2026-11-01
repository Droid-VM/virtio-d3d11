#include "media_read_state.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include "media_upload_scratch.h"
static void *scratch_thread(void *arg) {
 unsigned char pattern=(unsigned char)(uintptr_t)arg;
 unsigned char source[12],*owned=malloc(24); unsigned i;
 assert(owned);memset(source,pattern,sizeof(source));
 assert(mu_pad(owned,24,source,4,2,4));
 for(i=0;i<8;i++)assert(owned[i]==pattern);
 for(i=16;i<20;i++)assert(owned[i]==pattern);
 assert(!mu_pad(NULL,24,source,4,2,4));
 free(owned);return NULL;
}
#define FAIL ((int32_t)0x80004005u)
#define BUSY ((int32_t)0x887a000au)
static void ready(MR_STATE *s,uint64_t tag) {s->owner.tag=tag;s->state=MR_MEDIA_READY;}
int main(void) {
 MR_STATE a={0},b={0}; MT_JOB old; uint64_t v; unsigned i;
 /* T1 exact two slice identities, reverse media readiness. */
 assert(mr_begin(&a,1,1,5));assert(mr_begin(&b,2,1,5));
 ready(&b,12);assert(mr_read(&a)==MR_BUSY);assert(mr_read(&b)==MR_BUSY);
 b.owner.state=MT_READY;assert(mr_pin(&b,&b.owner,b.version));
 assert(mr_publish(&b,&b.owner,b.version,0));assert(mr_read(&b)==MR_ALLOW);
 /* T2 NOWAIT never sticky and does not perform upload; upload explicit. */
 assert(mr_gate(0,BUSY,BUSY)==0);ready(&a,11);assert(mr_read(&a)==MR_BUSY);
 assert(mr_pin(&a,&a.owner,a.version));assert(mr_read(&a)==MR_BUSY);
 assert(!mr_begin(&a,3,1,5));assert(mr_publish(&a,&a.owner,a.version,0));
 assert(mr_read(&a)==MR_ALLOW);
 /* T3 persistent failure independent of ring, repeated empty polls pending. */
 assert(mr_begin(&a,3,1,5));for(i=0;i<100;i++)assert(mr_read(&a)==MR_BUSY);
 mr_fail(&a,FAIL);assert(mr_read(&a)==MR_ERROR);assert(a.failure==FAIL);
 old=a.owner;v=a.version;mr_fail(&a,(int32_t)0x80070079u);assert(a.failure==FAIL);
 /* T4 whole requires both states allow; failed slice cannot be omitted. */
 assert(mr_read(&a)!=MR_ALLOW && mr_read(&b)==MR_ALLOW);
 /* T5 version rejects stale publish/timeout; active upload forbids reuse. */
 assert(mr_begin(&a,4,2,6));assert(!mr_current(&a,&old,v));
 ready(&a,14);assert(mr_pin(&a,&a.owner,a.version));assert(!mr_begin(&a,5,2,6));
 assert(!mr_publish(&a,&old,v,0));assert(mr_publish(&a,&a.owner,a.version,FAIL));
 assert(mr_read(&a)==MR_ERROR);
 /* T6 same production sticky decision for all nine execution entry gates. */
 for(i=0;i<9;i++)assert(mr_gate(FAIL,0,BUSY)==FAIL);
 assert(mr_gate(0,FAIL,BUSY)==FAIL);assert(mr_gate(0,0,BUSY)==0);
 /* T7 model only: finite first-surface deadline fails, no neutral success;
  * independent 18-AU feed may later mark exact tag ready. */
 { MR_STATE q={0};assert(mr_begin(&q,1,7,8));
   for(i=0;i<17;i++)assert(mr_read(&q)==MR_BUSY);
   mr_fail(&q,(int32_t)0x80070079u);assert(mr_read(&q)==MR_ERROR);
 }
 { MR_TICKET t[3]={{0}}; uint64_t next=0,x,y,z;
   x=mr_ticket_add(t,3,100,&next);y=mr_ticket_add(t,3,100,&next);
   assert(x && y && x!=y && mr_resource_pinned(t,3,100));
   assert(!mr_ticket_remove(t,3,101,x));
   assert(mr_ticket_remove(t,3,100,x));assert(!mr_ticket_remove(t,3,100,x));
   assert(mr_resource_pinned(t,3,100));
   assert(mr_ticket_remove(t,3,100,y));assert(!mr_resource_pinned(t,3,100));
   z=mr_ticket_add(t,3,999,&next);assert(z && mr_resource_pinned(t,3,999));
   next=UINT64_MAX;assert(!mr_ticket_add(t,3,1,&next));
 }
 { pthread_t first,second;
   assert(!pthread_create(&first,NULL,scratch_thread,(void *)(uintptr_t)23));
   assert(!pthread_create(&second,NULL,scratch_thread,(void *)(uintptr_t)91));
   assert(!pthread_join(first,NULL));assert(!pthread_join(second,NULL));
 }
 puts("PASS T1 exact/reverse T2 nowait/upload T3 persistent/poll T4 whole T5 lease T6 sticky9 T7 model-only");
 return 0;
}
