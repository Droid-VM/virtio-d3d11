#ifndef MEDIA_UPLOAD_SCRATCH_H
#define MEDIA_UPLOAD_SCRATCH_H
#include <stdint.h>
#include <string.h>
static inline int mu_pad(uint8_t *dst,uint64_t capacity,const uint8_t *src,
 uint32_t stride,uint32_t height,uint32_t full_height) {
 uint64_t y=(uint64_t)stride*height,full=(uint64_t)stride*full_height;
 if(!dst || !src || full_height<height || capacity<full*3/2) return 0;
 memset(dst,16,(size_t)full);memcpy(dst,src,(size_t)y);
 memset(dst+full,128,(size_t)(full/2));memcpy(dst+full,src+y,(size_t)(y/2));return 1;
}
#endif
