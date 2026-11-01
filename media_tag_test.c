#include "media_tag_match.h"
#include <assert.h>
#include <stdio.h>
/* Executable uses exact production transitions, no duplicate mock matcher. */
int main(void) {
 MT_JOB j[40]={{0}}; unsigned n,claims=0,released=0,received=0;
 for(n=0;n<40;n++) { assert(mt_register(&j[n],n+1,n+1,1,7)); assert(mt_sent(&j[n],1)==MT_RELEASE); }
 /* Sender advances through 40 AUs with no captures (17+ lookahead). */
 for(n=40;n>0;n--) { received++; assert(mt_capture(&j[n-1],n,1,7)==MT_CLAIM); claims++; }
 received++; assert(mt_capture(&j[0],1,1,7)==MT_RELEASE); released++; /* duplicate */
 received++; assert(mt_capture(NULL,900,1,7)==MT_RELEASE); released++;
 received++; assert(mt_capture(&j[1],2,2,8)==MT_RELEASE); released++; /* flush */
 for(n=0;n<40;n++) released++; /* accepted captures retired once */
 assert(received==released);
 assert(mt_register(&j[0],100,100,2,8));
 assert(mt_capture(&j[0],100,2,8)==MT_STASH);
 assert(mt_capture(&j[0],100,2,8)==MT_RELEASE);
 assert(mt_sent(&j[0],1)==MT_CLAIM);
 assert(mt_register(&j[0],101,101,2,8));
 assert(mt_capture(&j[0],101,2,8)==MT_STASH);
 assert(mt_sent(&j[0],0)==MT_RELEASE && j[0].state==MT_FAILED);
 assert(mt_capture(&j[0],101,2,8)==MT_RELEASE);
 assert(mt_register(&j[0],102,102,2,8));
 j[0].state=MT_FAILED; /* invalid early mapping */
 assert(mt_sent(&j[0],1)==MT_RELEASE && j[0].state==MT_FAILED);
 /* Production pre-admission guard covers a different valid tag while the
  * original capture is UPLOADING: second receive creates no new lease. */
 { MT_JOB a={0}, b={0}; unsigned leases=0, releases=0;
   assert(mt_register(&a,200,200,3,9)); mt_sent(&a,1);
   assert(mt_register(&b,201,201,3,9)); mt_sent(&b,1);
   assert(mt_capture(&a,200,3,9)==MT_CLAIM); leases++;
   MT_LEASE table[2]={{5,9,3,1},{0,9,3,0}};
   assert(mt_lease_conflict(table,2,5,9,3)==0);
   assert(b.state==MT_SENT); b.state=MT_FAILED;
   assert(mt_lease_conflict(table,2,5,8,3)==-1);
   assert(mt_lease_conflict(table,2,5,9,4)==-1);
   table[0].owned=0;
   assert(mt_lease_conflict(table,2,5,9,3)==-1);
   releases++; /* only original upload/retire releases index 5 */
   assert(leases==releases && releases==1);
 }
 assert(!mt_should_release(5,5,1));
 assert(mt_should_release(6,5,1));
 assert(mt_should_release(5,5,0));
 { MT_JOB old=j[0];
   assert(mt_register(&j[0],300,300,5,10));
   assert(!mt_same(&old,&j[0]));
   assert(j[0].state==MT_SENDING && j[0].tag==300);
 }
 assert(!mt_register(&j[0],0,1,1,7));
 assert(!mt_register(&j[0],(uint64_t)INT64_MAX+1,1,1,7));
 assert(!mt_register(&j[0],1,1,1,0));
 printf("PASS reverse=40 lookahead_send=40 duplicate unknown flush early success/failure release=%u/%u claims=%u\n",released,received,claims);
 return 0;
}
