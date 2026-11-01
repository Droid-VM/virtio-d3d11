/* Execute the production helper with a fake COM boundary; no GPU claims. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef int32_t HRESULT;
typedef int BOOL;
typedef struct { uint32_t a; uint16_t b,c; unsigned char d[8]; } IID;
#define TRUE 1
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005u)
#define E_POINTER ((HRESULT)0x80004003u)
#define E_NOINTERFACE ((HRESULT)0x80004002u)
#define FAILED(x) ((x)<0)
typedef struct { int unused; } ID3D11DeviceContext;
typedef struct { BOOL enabled; int refuse, releases, sets; } ID3D10Multithread;
static ID3D10Multithread object;
static HRESULT query_result;
static int null_interface;
static HRESULT query(ID3D11DeviceContext *c, const IID *iid, void **out) {
    assert(c && iid->a==0x9b7e4e00 && iid->d[7]==0xf0);
    *out=null_interface ? NULL : &object;
    return query_result;
}
static BOOL set(ID3D10Multithread *p, BOOL value) {
    BOOL old=p->enabled; p->sets++; if(!p->refuse) p->enabled=value; return old;
}
#define ID3D11DeviceContext_QueryInterface query
#define ID3D10Multithread_GetMultithreadProtected(p) ((p)->enabled)
#define ID3D10Multithread_SetMultithreadProtected set
#define ID3D10Multithread_Release(p) (++(p)->releases)
#define CONTEXT_PROTECTION_TEST_TYPES
#include "include/context_protection.h"
static void reset(void) { object=(ID3D10Multithread){0}; query_result=S_OK; null_interface=0; }
int main(void) {
    ID3D11DeviceContext ctx={0}; ContextProtectionState s;
    reset(); assert(protect_internal_context(NULL,&s)==E_POINTER && !object.sets);
    reset(); query_result=E_NOINTERFACE; null_interface=1;
    assert(protect_internal_context(&ctx,&s)==E_NOINTERFACE && !object.sets && !object.releases);
    reset(); null_interface=1;
    assert(protect_internal_context(&ctx,&s)==E_NOINTERFACE && !object.sets);
    reset(); query_result=E_FAIL; /* defensive malformed QI failure with pointer */
    assert(protect_internal_context(&ctx,&s)==E_FAIL && object.releases==1 && !object.sets);
    reset(); assert(protect_internal_context(&ctx,&s)==S_OK);
    assert(!s.before && !s.previous && s.after && object.sets==1 && object.releases==1);
    reset(); object.enabled=TRUE; assert(protect_internal_context(&ctx,&s)==S_OK);
    assert(s.before && s.previous && s.after && object.releases==1);
    reset(); object.refuse=1; assert(protect_internal_context(&ctx,&s)==E_FAIL);
    assert(!s.after && object.releases==1 && object.sets==1);
    puts("PASS production helper: null/QI failure/null interface/release/previous FALSE/Get TRUE/Get FALSE");
}
