#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
#include "../../av1_tile_identity.h"
int main(void) {
    Av1TileSha256 s; unsigned char block[777]; size_t n; char hex[65];
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    av1_tile_sha_init(&s);
    while((n=fread(block,1,sizeof(block),stdin))) av1_tile_sha_update(&s,block,n);
    if(ferror(stdin)) return 2;
    av1_tile_sha_final(&s,hex);puts(hex);return 0;
}
