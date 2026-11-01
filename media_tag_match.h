/* Pure production identity state machine. Caller serializes with pipe.lock.
 * A capture is a unique receive event; reject means caller releases that event,
 * never a mapping already owned by an accepted job. */
#ifndef MEDIA_TAG_MATCH_H
#define MEDIA_TAG_MATCH_H
#include <stdint.h>
typedef struct { uint64_t tag, seq, generation; uint32_t token; unsigned state; } MT_JOB;
enum { MT_EMPTY, MT_SENDING, MT_SENT, MT_EARLY, MT_READY, MT_FAILED };
enum { MT_RELEASE, MT_STASH, MT_CLAIM };
static inline int mt_register(MT_JOB *j, uint64_t tag, uint64_t seq, uint64_t gen, uint32_t token) {
 if (!tag || tag > INT64_MAX || !token) return 0;
 j->tag=tag; j->seq=seq; j->generation=gen; j->token=token; j->state=MT_SENDING; return 1;
}
static inline unsigned mt_capture(MT_JOB *j, uint64_t tag, uint64_t gen, uint32_t token) {
 if (!j || j->tag!=tag || j->generation!=gen || j->token!=token) return MT_RELEASE;
 if (j->state==MT_SENDING) { j->state=MT_EARLY; return MT_STASH; }
 if (j->state==MT_SENT) { j->state=MT_READY; return MT_CLAIM; }
 return MT_RELEASE;
}
/* Returns claim for an early capture only after real SEND success. */
static inline unsigned mt_sent(MT_JOB *j, int success) {
 unsigned early=j->state==MT_EARLY;
 if (j->state!=MT_SENDING && !early) return MT_RELEASE;
 j->state=success ? (early ? MT_READY : MT_SENT) : MT_FAILED;
 return early && success ? MT_CLAIM : MT_RELEASE;
}
/* Re-delivery of an index already leased to a job is not another lease.
 * Releasing it would revoke the accepted frame and cause a double release. */
static inline int mt_should_release(uint32_t received_index, uint32_t owned_index,
                                    int owned) {
 return !owned || received_index != owned_index;
}
typedef struct { uint32_t index, token; uint64_t generation; int owned; } MT_LEASE;
/* -1 admits a new capture. Otherwise the returned existing owner retains the
 * only release obligation; caller must reject without releasing that lease. */
static inline int mt_lease_conflict(const MT_LEASE *leases, unsigned count,
 uint32_t index, uint32_t token, uint64_t generation) {
 unsigned i;
 for(i=0;i<count;++i)
  if(leases[i].token==token && leases[i].generation==generation &&
     !mt_should_release(index,leases[i].index,leases[i].owned)) return (int)i;
 return -1;
}
static inline int mt_same(const MT_JOB *a, const MT_JOB *b) {
 return a->tag==b->tag && a->seq==b->seq && a->generation==b->generation && a->token==b->token;
}
#endif
