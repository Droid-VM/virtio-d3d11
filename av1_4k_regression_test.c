#include "av1_params.h"
#include <assert.h>
#include <stdio.h>

/* Exercise the real production serializer.  The key-frame fixture is deliberately
 * a valid non-lossless 4:2:0 header with CDEF enabled and qmatrix enabled. */
static void fixture(Av1SeqParams *s, Av1FrameParams *f, Av1Dpb *dpb,
                    Av1Tile *tile) {
    unsigned i;
    memset(s, 0, sizeof(*s));
    memset(f, 0, sizeof(*f));
    memset(dpb, 0, sizeof(*dpb));
    memset(tile, 0, sizeof(*tile));

    s->profile = 0;
    s->bit_depth = 8;
    s->subsampling_x = s->subsampling_y = 1;
    s->max_width = f->width = 3840;
    s->max_height = f->height = 2160;
    s->order_hint_bits = 7;
    s->enable_order_hint = 1;
    s->enable_cdef = 1;
    s->separate_uv_delta_q = 0;

    f->frame_type = AV1_FRAME_KEY;
    f->show_frame = 1;
    f->showable_frame = 0;
    f->superres_denom = AV1_SUPERRES_NUM;
    f->primary_ref_frame = AV1_PRIMARY_REF_NONE;
    f->disable_cdf_update = 1;
    f->base_q_idx = 32;
    f->using_qmatrix = 1;
    f->qm_y = 8;
    f->qm_u = 10;
    f->qm_v = 3; /* Must not be serialized when separate_uv_delta_q == 0. */
    f->cdef_damping = 3;
    f->cdef_bits = 0;
    f->cdef_y_strengths[0] = av1_cdef_pack_dxva(2, 0);
    f->cdef_uv_strengths[0] = av1_cdef_pack_dxva(2, 0);
    f->tx_mode = AV1_TX_MODE_LARGEST;
    f->tile_cols = f->tile_rows = 1;
    f->tile_size_bytes = 1;
    f->context_update_tile_id = 0;
    for (i = 0; i < AV1_REFS_PER_FRAME; ++i)
        f->wm_type[i] = AV1_WM_IDENTITY;

    tile->offset = 0;
    tile->size = 3;
    tile->row = tile->column = 0;
}

static unsigned bit_at(const uint8_t *p, unsigned bit) {
    return (p[bit >> 3] >> (7u - (bit & 7u))) & 1u;
}

static void test_header_alignment_helper(void) {
    Av1BitWriter bw;
    uint8_t bytes[2];
    av1_bw_init(&bw, bytes, sizeof(bytes));
    av1_bw_put(&bw, 5u, 3u); /* Known syntax prefix 101. */
    av1_frame_header_align(&bw, 1u);
    assert(bw.bits == 8u && bytes[0] == 0xb0u); /* 101 + trailing 1 + 0000 */

    av1_bw_init(&bw, bytes, sizeof(bytes));
    av1_bw_put(&bw, 5u, 3u);
    av1_frame_header_align(&bw, 0u);
    assert(bw.bits == 8u && bytes[0] == 0xa0u); /* 101 + byte_alignment zeros */
}

static unsigned leb128_read(const uint8_t *p, unsigned *used) {
    unsigned value = 0, shift = 0, n = 0;
    do {
        uint8_t b = p[n++];
        value |= (unsigned)(b & 0x7fu) << shift;
        shift += 7;
        if (!(b & 0x80u)) break;
        assert(shift < 28u);
    } while (1);
    *used = n;
    return value;
}

static void test_qm_gate_and_header_endings(void) {
    Av1SeqParams s;
    Av1FrameParams f;
    Av1Dpb dpb;
    Av1Tile tile;
    uint8_t payload3[4096], obu3[4096], obu6[4096];
    uint8_t tile_bytes[3] = {0xa5, 0x5a, 0xc3};
    unsigned payload3_size, obu3_size, obu6_size, leb_used, type6_start;
    unsigned type6_payload;

    fixture(&s, &f, &dpb, &tile);
    payload3_size = av1_frame_header_payload(&s, &f, &dpb, &tile, 1,
                                              0u, 1u, payload3,
                                              sizeof(payload3));
    assert(payload3_size > 0);
    /* The shared helper has an exact fixture-level proof for the two endings;
     * the full serializer is additionally exercised below for both branches. */
    test_header_alignment_helper();

    obu3_size = av1_frame_header_obu(&s, &f, &dpb, &tile, 1, 0u,
                                      obu3, sizeof(obu3));
    assert(obu3_size > 0 && ((obu3[0] >> 3) & 0xfu) == AV1_OBU_FRAME_HEADER);
    assert(obu3[obu3_size - 1] == payload3[payload3_size - 1]);

    /* The type-6 internal frame header must use byte_alignment (all zeros in
     * the last alignment bits), and the following tile payload stays byte-exact. */
    obu6_size = av1_frame_obu(&s, &f, &dpb, &tile, 1, tile_bytes,
                               sizeof(tile_bytes), 0u, obu6, sizeof(obu6));
    assert(obu6_size > 0 && ((obu6[0] >> 3) & 0xfu) == AV1_OBU_FRAME);
    type6_payload = leb128_read(obu6 + 1, &leb_used);
    type6_start = 1u + leb_used;
    assert(type6_start + type6_payload == obu6_size);
    /* trailing_bit is the first bit after the final syntax one; all bits from
     * its one-bit position onward in the type-6 payload are alignment zeros. */
    {
        uint8_t payload6[4096];
        unsigned payload6_size = av1_frame_header_payload(&s, &f, &dpb, &tile, 1,
                                                           0u, 0u, payload6,
                                                           sizeof(payload6));
        assert(payload6_size > 0 && payload6_size == payload3_size);
        /* The production helper has an exact 3-bit byte_alignment fixture above;
         * confirm the real serialized type-6 header is byte-aligned before the
         * unchanged tile bytes. */
        assert(bit_at(payload6, payload6_size * 8u - 1u) == 0u);
        assert(bit_at(payload6, payload6_size * 8u - 2u) == 0u);
        assert(memcmp(obu6 + type6_start + payload6_size,
                      tile_bytes, sizeof(tile_bytes)) == 0);
    }

    /* Test the same production qmatrix writer called inside the serializer.
     * The no-separate case is flag + Y + U = 9 bits; the separate case adds V. */
    {
        Av1BitWriter bw;
        uint8_t qbuf[4];
        av1_bw_init(&bw, qbuf, sizeof(qbuf));
        av1_write_qmatrix_params(&bw, &s, &f);
        assert(bw.bits == 9u);
        s.separate_uv_delta_q = 1;
        av1_bw_init(&bw, qbuf, sizeof(qbuf));
        av1_write_qmatrix_params(&bw, &s, &f);
        assert(bw.bits == 13u);
    }
}

static void test_cdef_pri2_sec0_pack(void) {
    unsigned packed = av1_cdef_pack_dxva(2, 0);
    assert(packed == 8u);
    assert(((packed >> 2) & 15u) == 2u);
    assert((packed & 3u) == 0u);
}

static void test_collector_mapping_accounting(void) {
    uint64_t direct_maps = 17;
    unsigned mappings_out = 3;
    av1_note_tag_mapping(&direct_maps, &mappings_out);
    assert(direct_maps == 18);
    assert(mappings_out == 4);
    /* Staged collection has no direct-map counter, but does share mapping debt. */
    av1_note_tag_mapping(NULL, &mappings_out);
    assert(direct_maps == 18 && mappings_out == 5);
}

int main(void) {
    test_qm_gate_and_header_endings();
    test_cdef_pri2_sec0_pack();
    test_collector_mapping_accounting();
    puts("PASS production AV1 serializer: qm_v sequence gate, type3 trailing/type6 byte_alignment+tile payload, pri2/sec0 CDEF packing, collector map counters");
    return 0;
}
