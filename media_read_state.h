#ifndef MEDIA_READ_STATE_H
#define MEDIA_READ_STATE_H
#include <stdint.h>
#include "media_tag_match.h"
enum { MR_PENDING=1,MR_MEDIA_READY,MR_UPLOADING,MR_QUEUED,MR_FAILED };
enum { MR_ALLOW,MR_BUSY,MR_ERROR };
typedef struct { MT_JOB owner; uint64_t version; unsigned state; int32_t failure; } MR_STATE;
static inline int mr_begin(MR_STATE *s, uint64_t seq,uint64_t gen,uint32_t token) {
 if(s->state && s->state!=MR_QUEUED && s->state!=MR_FAILED) return 0;
 if(s->version==UINT64_MAX) return 0;
 s->version++; s->owner=(MT_JOB){0}; s->owner.seq=seq;
 s->owner.generation=gen; s->owner.token=token; s->state=MR_PENDING; s->failure=0; return 1;
}
static inline int mr_current(const MR_STATE *s,const MT_JOB *j,uint64_t version) {
 return s->version==version && mt_same(&s->owner,j);
}
static inline void mr_fail(MR_STATE *s,int32_t error) {
 if(s->state!=MR_FAILED) { s->failure=error; s->state=MR_FAILED; }
}
static inline unsigned mr_read(const MR_STATE *s) {
 return s->state==MR_QUEUED ? MR_ALLOW : s->state==MR_FAILED ? MR_ERROR : MR_BUSY;
}
static inline int mr_pin(MR_STATE *s,const MT_JOB *j,uint64_t version) {
 if(!mr_current(s,j,version)||s->state!=MR_MEDIA_READY) return 0;
 s->state=MR_UPLOADING; return 1;
}
static inline int mr_publish(MR_STATE *s,const MT_JOB *j,uint64_t version,int32_t hr) {
 if(!mr_current(s,j,version)||s->state!=MR_UPLOADING) return 0;
 if(hr<0) mr_fail(s,hr); else s->state=MR_QUEUED; return 1;
}
static inline int32_t mr_gate(int32_t sticky,int32_t read_hr,int32_t busy_hr) {
 if(sticky<0) return sticky;
 return read_hr<0 && read_hr!=busy_hr ? read_hr : 0;
}
typedef struct { uintptr_t resource; unsigned subresource,pins; MR_STATE media; } MR_ADMISSION;
static inline unsigned mr_admit(MR_ADMISSION *a,unsigned count,uintptr_t resource,
 unsigned sub,int whole,int release) {
 unsigned i;
 if(!release) for(i=0;i<count;++i)
  if(a[i].resource==resource && (whole || a[i].subresource==sub) && mr_read(&a[i].media)!=MR_ALLOW)
   return mr_read(&a[i].media);
 for(i=0;i<count;++i) if(a[i].resource==resource) {
  if(release) { if(a[i].pins) a[i].pins--; } else a[i].pins++;
 }
 return MR_ALLOW;
}
typedef struct { uint64_t ticket; uintptr_t resource; } MR_TICKET;
static inline int mr_resource_pinned(const MR_TICKET *t,unsigned n,uintptr_t resource) {
 unsigned i;for(i=0;i<n;++i)if(t[i].ticket && t[i].resource==resource)return 1;return 0;
}
static inline uint64_t mr_ticket_add(MR_TICKET *t,unsigned n,uintptr_t resource,uint64_t *next) {
 unsigned i;if(*next==UINT64_MAX)return 0;
 for(i=0;i<n;++i) {
  if(!t[i].ticket){t[i].resource=resource;t[i].ticket=++*next;return t[i].ticket;}
 }
 return 0;
}
static inline int mr_ticket_remove(MR_TICKET *t,unsigned n,uintptr_t resource,uint64_t ticket) {
 unsigned i;if(!ticket)return 0;
 for(i=0;i<n;++i) {
  if(t[i].ticket==ticket && t[i].resource==resource){t[i].ticket=0;t[i].resource=0;return 1;}
 }
 return 0;
}
#endif
