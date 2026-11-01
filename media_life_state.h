#ifndef MEDIA_LIFE_STATE_H
#define MEDIA_LIFE_STATE_H
#include <stdint.h>
/* Production arithmetic shared with native fault model. Time is monotonic ms;
 * every blocking boundary receives the SAME absolute deadline. */
static inline uint32_t ml_remaining(uint64_t now, uint64_t deadline) {
    uint64_t left=now<deadline ? deadline-now : 0;
    return left>UINT32_MAX ? UINT32_MAX : (uint32_t)left;
}
static inline int ml_cleanup_ready(long refs, int detached) {
    return detached && refs==1; /* cleanup's reference is deliberately excluded */
}
static inline int ml_admission_allowed(long reserved, unsigned limit) {
    return reserved>0 && (unsigned long)reserved<=limit;
}
/* A failed release is NOT a transfer. Keep the exact index/token/generation. */
typedef struct ML_MAP_LEASE {
    uint32_t index,token;
    uint64_t generation;
    int owned;
} ML_MAP_LEASE;
static inline int ml_release_result(ML_MAP_LEASE *lease,int succeeded) {
    if(!lease->owned) return 0;
    if(succeeded) lease->owned=0;
    return succeeded;
}
#endif
