/* P-BATCH AV1: rebuild the AV1 OBUs a D3D11 AV1 client cannot send.
 *
 * A D3D11 AV1 decoder is a STATELESS hardware-decoder interface: Chromium's
 * D3D11AV1Accelerator (media/gpu/windows/d3d11_av1_accelerator.cc, checked
 * against tag 136.0.7103.59) submits
 *
 *   PICTURE_PARAMETERS = DXVA_PicParams_AV1   (the parsed sequence + frame
 *                                              header, field by field)
 *   SLICE_CONTROL      = DXVA_Tile_AV1[]      (offset/size/row/column of each
 *                                              tile inside the bitstream buffer)
 *   BITSTREAM          = the raw tile payloads, concatenated, and NOTHING else
 *
 * The host side of this UMD is a *stateful* V4L2 decoder (c2.qti.av1.decoder on
 * the Android host): it wants the original temporal unit, i.e.
 *
 *   temporal_delimiter OBU
 *   [ sequence_header OBU ]        (first frame / on a new sequence)
 *   frame_header OBU               (rebuilt from DXVA_PicParams_AV1)
 *   tile_group OBU                 (the tile payloads, with tile size fields)
 *
 * so this file is the missing half: it serialises the sequence header and the
 * uncompressed frame header from the plain-C field set below (filled from
 * DXVA_PicParams_AV1 by d3d11_video.c) and wraps the tile payloads in a
 * tile_group OBU.  The syntax order and every condition is a transcription of
 * the AV1 specification (5.5.1/5.5.2/5.9.x/5.11.1), cross-checked against
 * ffmpeg's cbs_av1 reader/writer (libavcodec/cbs_av1_syntax_template.c), and
 * the offline test (tests/av1_params_test.c) has ffmpeg's trace_headers parse
 * every OBU we emit and compare the fields.
 *
 * The one field DXVA does not carry is refresh_frame_flags (which of the eight
 * DPB slots a frame writes itself into).  It is exactly derivable from the NEXT
 * frame's RefFrameMapTextureIndex -- the DPB as that frame sees it -- which is
 * what av1_derive_refresh_flags() does.  The UMD therefore sends each inter
 * frame once with refresh_frame_flags = 0 (the only non-destructive mask), gets
 * its output, and re-sends it as a hidden "DPB catch-up" frame ahead of the
 * next temporal unit once the true mask is known.  See d3d11_video.c.
 *
 * Only 8-bit 4:2:0 (profile 0) is advertised, which is the only thing the NV12
 * output path can hold; the struct still carries bit depth/profile so the UMD
 * refuses a mismatched client before this code sees it.
 */
#ifndef VIRTIO_WDDM_AV1_PARAMS_H
#define VIRTIO_WDDM_AV1_PARAMS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Constants (AV1 6.10.x / Annex A)                                          */
/* ------------------------------------------------------------------------- */

#define AV1_NUM_REF_FRAMES 8
#define AV1_REFS_PER_FRAME 7
#define AV1_TOTAL_REFS_PER_FRAME 8
#define AV1_MAX_SEGMENTS 8
#define AV1_SEG_LVL_MAX 8
#define AV1_PRIMARY_REF_NONE 7
#define AV1_MAX_TILE_COLS 64
#define AV1_MAX_TILE_ROWS 64
#define AV1_MAX_TILES 512 /* bitstream conformance limit (5.9.15) */
#define AV1_SUPERRES_NUM 8
#define AV1_SUPERRES_DENOM_MIN 9
#define AV1_MAX_TILE_WIDTH 4096
#define AV1_MAX_TILE_AREA (4096 * 2304)

/* Frame types (5.9.2). */
#define AV1_FRAME_KEY 0
#define AV1_FRAME_INTER 1
#define AV1_FRAME_INTRA_ONLY 2
#define AV1_FRAME_SWITCH 3

/* OBU types (6.2.2). */
#define AV1_OBU_SEQUENCE_HEADER 1
#define AV1_OBU_TEMPORAL_DELIMITER 2
#define AV1_OBU_FRAME_HEADER 3
#define AV1_OBU_TILE_GROUP 4
#define AV1_OBU_METADATA 5
#define AV1_OBU_FRAME 6

/* Restoration types (5.9.20). */
#define AV1_RESTORE_NONE 0

/* Interpolation filters (5.9.11). */
/* AV1/GStreamer/DXVA interpolation-filter enum (5.9.14 InterpFilter):
 *   0 EIGHTTAP_SMOOTH, 1 EIGHTTAP, 2 EIGHTTAP_SHARP, 3 BILINEAR,
 *   4 SWITCHABLE.
 * DXVA_PicParams_AV1::interp_filter uses these values, so SWITCHABLE is 4 --
 * comparing it against 3 (this constant used to be 3) made every switchable
 * frame code "is_filter_switchable=0 + interpolation_filter=0", which desyncs
 * the tile entropy decoder from the first block on.  t6_av1's 890 inter
 * pictures are all switchable. */
#define AV1_INTERP_SWITCHABLE 4

/* TX modes (5.9.21). */
#define AV1_ONLY_4X4 0
#define AV1_TX_MODE_LARGEST 1
#define AV1_TX_MODE_SELECT 2

/* Global motion transform types (5.9.24). */
#define AV1_WM_IDENTITY 0
#define AV1_WM_TRANSLATION 1
#define AV1_WM_ROTZOOM 2
#define AV1_WM_AFFINE 3

/* Global motion parameter widths (5.9.24). */
#define AV1_GM_ABS_ALPHA_BITS 12
#define AV1_GM_ABS_TRANS_ONLY_BITS 9
#define AV1_GM_ABS_TRANS_BITS 12
/* Fixed-point precisions the global-motion syntax is relative to (5.9.24; the
 * reference decoder calls them WARPEDMODEL_PREC_BITS / GM_ALPHA_PREC_BITS /
 * GM_TRANS_PREC_BITS).  Each gm parameter is coded as a signed subexp DELTA from
 * the SAME parameter of the primary reference frame, in these precisions. */
#define AV1_WARPEDMODEL_PREC_BITS 12
#define AV1_GM_ALPHA_PREC_BITS 12
#define AV1_GM_TRANS_PREC_BITS 6

/* Segmentation feature ids (5.9.18). */
#define AV1_SEG_LVL_ALT_Q 0
#define AV1_SEG_LVL_ALT_LF_Y_V 1
#define AV1_SEG_LVL_ALT_LF_Y_H 2
#define AV1_SEG_LVL_ALT_LF_U 3
#define AV1_SEG_LVL_ALT_LF_V 4
#define AV1_SEG_LVL_REF_FRAME 5
#define AV1_SEG_LVL_SKIP 6
#define AV1_SEG_LVL_GLOBALMV 7

/* A superblock is 64x64 unless use_128x128_superblock is set; all tile sizes
 * are in superblocks.  sb_shift is log2(sb_size) - 2 (4 or 5). */
#define AV1_SB_SHIFT_64 4
#define AV1_SB_SHIFT_128 5

/* ------------------------------------------------------------------------- */
/* Plain-C description of one picture, filled from DXVA_PicParams_AV1         */
/* ------------------------------------------------------------------------- */

/* Sequence-level values: everything that goes into the sequence header.  One
 * of these per decoder; it changes only with a new sequence (a key frame with
 * different parameters), and the UMD re-emits the header then. */
typedef struct {
    unsigned profile;   /* 0 = Main (8-bit 4:2:0) */
    unsigned bit_depth; /* 8 */
    unsigned mono_chrome;
    unsigned subsampling_x;
    unsigned subsampling_y;

    unsigned max_width; /* sequence max_frame_width  (pixels) */
    unsigned max_height;

    unsigned use_128x128_superblock;
    unsigned enable_filter_intra;
    unsigned enable_intra_edge_filter;
    unsigned enable_interintra_compound;
    unsigned enable_masked_compound;
    unsigned enable_dual_filter;
    unsigned enable_jnt_comp;
    unsigned enable_ref_frame_mvs;
    unsigned enable_order_hint;
    unsigned order_hint_bits; /* 0 when enable_order_hint == 0 */
    unsigned enable_cdef;
    unsigned enable_restoration;
    unsigned film_grain_params_present;
    /* Both of these are sequence-header syntax elements that DXVA does not
     * carry, so they are DERIVED from the picture and re-emitted when they
     * change.  They used to be hardcoded to 1, which cost two bits in every
     * frame header and made our temporal units differ from the source stream
     * for no reason (round 26 byte diff: the only remaining difference). */
    unsigned enable_superres;     /* 0 unless some frame is superres-scaled */
    unsigned separate_uv_delta_q; /* 0 unless the U/V delta_q values differ */

    unsigned color_range; /* 0 = studio (DXVA carries no range) */
    unsigned level_idx;   /* seq_level_idx; av1_level_from_size() picks one */
} Av1SeqParams;

/* One tile of the frame: where its payload sits in the client's BITSTREAM
 * buffer (offset/size) and where it goes in the frame (row/column). */
typedef struct {
    uint32_t offset;
    uint32_t size;
    uint16_t row;
    uint16_t column;
} Av1Tile;

/* Frame-level values: everything that goes into the frame header for one
 * picture.  refresh_frame_flags is NOT here: it is a parameter of the writer
 * because the UMD emits the same picture twice (provisional, then hidden
 * catch-up) with two different masks. */
typedef struct {
    unsigned frame_type; /* AV1_FRAME_* */
    unsigned error_resilient_mode;
    unsigned disable_cdf_update;
    unsigned allow_screen_content_tools;
    unsigned force_integer_mv;
    unsigned order_hint;
    unsigned primary_ref_frame; /* 0..7, 7 = NONE */
    /* show_frame / showable_frame come from the CLIENT (DXVA_PicParams_AV1
     * format.show_frame / format.showable_frame).  They used to be forced to 1,
     * which made the host produce -- or rather try to produce -- a picture for
     * every hidden alt-ref frame the client was never going to display, and the
     * UMD then waited 2 x 1000 ms for a CAPTURE buffer that never came (round 26
     * measured 62 AUs in 60 s = 0.97 s per AU, exactly one timeout each). */
    unsigned show_frame;
    unsigned showable_frame;    /* only coded when show_frame == 0 (5.9.2) */

    unsigned width;  /* upscaled frame width  (pixels) */
    unsigned height; /* upscaled frame height (pixels) */
    unsigned superres_denom; /* 8 = no superres, else 9..16 */

    unsigned ref_frame_idx[AV1_REFS_PER_FRAME]; /* DPB slot per reference */
    /* DPB identity mirror (AV1 stateful port, 2026-10-27).  DXVA names every
     * picture by a TEXTURE INDEX, and the client's RefFrameMapTextureIndex[] is
     * its view of the DPB *before* this picture updates it -- exactly the
     * information the reference implementation derives refresh_frame_flags and
     * the reference remap from.  `texture` is this picture's own index
     * (CurrPicTextureIndex), `ref_texture` the eight slots of that map. */
    unsigned texture;
    uint8_t ref_texture[AV1_NUM_REF_FRAMES];

    unsigned allow_high_precision_mv;
    unsigned interpolation_filter;
    unsigned is_motion_mode_switchable;
    unsigned use_ref_frame_mvs;
    unsigned allow_intrabc;
    unsigned disable_frame_end_update_cdf;
    unsigned allow_warped_motion;
    unsigned reduced_tx_set;

    /* Tile info.  widths/heights are in SUPERBLOCKS (DXVA_PicParams_AV1's
     * tiles.widths/heights), one entry per column/row. */
    unsigned tile_cols;
    unsigned tile_rows;
    unsigned context_update_tile_id;
    /* Bytes per tile size field in the tile group; 0 = derive from the tile
     * records (the UMD sets it once so a later re-emit of this picture -- the
     * hidden catch-up frame -- does not need the records again). */
    unsigned tile_size_bytes;
    unsigned tile_width_sb[AV1_MAX_TILE_COLS];
    unsigned tile_height_sb[AV1_MAX_TILE_ROWS];

    /* Quantization (5.9.12). */
    unsigned base_q_idx;
    int delta_q_y_dc, delta_q_u_dc, delta_q_u_ac, delta_q_v_dc, delta_q_v_ac;
    unsigned using_qmatrix, qm_y, qm_u, qm_v;
    unsigned delta_q_present, delta_q_res;

    /* Segmentation (5.9.18).  feature_mask is one bit per AV1_SEG_LVL_*,
     * feature_data the final value the decoder must end up with. */
    unsigned segmentation_enabled;
    unsigned segmentation_update_map;
    unsigned segmentation_temporal_update;
    unsigned feature_mask[AV1_MAX_SEGMENTS];
    int feature_data[AV1_MAX_SEGMENTS][AV1_SEG_LVL_MAX];

    /* Loop filter (5.9.19). */
    unsigned filter_level[4];
    unsigned sharpness_level;
    unsigned mode_ref_delta_enabled;
    int ref_deltas[AV1_TOTAL_REFS_PER_FRAME];
    int mode_deltas[2];
    unsigned delta_lf_present, delta_lf_res, delta_lf_multi;
    unsigned frame_restoration_type[3];
    unsigned log2_restoration_unit_size[3];

    /* CDEF (5.9.19). */
    unsigned cdef_damping; /* full value, not minus 3 */
    unsigned cdef_bits;
    unsigned cdef_y_strengths[8]; /* combined primary:6 | secondary:2 */
    unsigned cdef_uv_strengths[8];

    unsigned tx_mode;
    unsigned reference_select;
    unsigned skip_mode_present;

    /* Global motion, one entry per AV1 reference (LAST..ALTREF = 0..6). */
    unsigned wm_type[AV1_REFS_PER_FRAME];
    int wm_params[AV1_REFS_PER_FRAME][6];

    /* Film grain (5.9.30). */
    unsigned apply_grain;
    unsigned grain_seed;
    unsigned chroma_scaling_from_luma;
    unsigned num_y_points;
    unsigned point_y_value[14], point_y_scaling[14];
    unsigned num_cb_points, num_cr_points;
    unsigned point_cb_value[10], point_cb_scaling[10];
    unsigned point_cr_value[10], point_cr_scaling[10];
    unsigned scaling_shift_minus8;
    unsigned ar_coeff_lag;
    int ar_coeffs_y[24];
    int ar_coeffs_cb[25];
    int ar_coeffs_cr[25];
    unsigned ar_coeff_shift_minus6;
    unsigned grain_scale_shift;
    unsigned cb_mult, cb_luma_mult, cb_offset;
    unsigned cr_mult, cr_luma_mult, cr_offset;
    unsigned overlap_flag;
    unsigned clip_to_restricted_range;
} Av1FrameParams;

/* Decoder picture buffer mirror: what the host decoder's DPB holds after the
 * pictures forwarded so far.
 *
 * AV1 STATEFUL PORT (2026-10-27; libva-v4l2 src/stateful/av1_bitstream.h
 * `Av1Dpb::Slot`): a slot now carries what the DECODER derives from it, not just
 * the order hint:
 *
 *   texture     -- the DPB slot's identity, i.e. the picture that lives there.
 *                  derive_refresh_frame_flags() and choose_refresh_flags() are
 *                  both defined in terms of it, and the reference remap in
 *                  av1_frame_prepare() resolves a client slot to the slot we
 *                  actually refreshed.
 *   order_hint  -- skip_mode presence and the error-resilient ref_order_hint
 *                  syntax (unchanged).
 *   gm          -- the saved global-motion matrix of the picture in the slot
 *                  ([ref][6], ref = LAST..ALTREF).  global_motion_params()
 *                  codes each parameter as a signed subexp DELTA from the
 *                  primary reference frame's value (5.9.24), so the writer
 *                  needs the slot's copy; without it every frame was coded
 *                  against the identity defaults. */
typedef struct {
    unsigned valid[AV1_NUM_REF_FRAMES];
    unsigned order_hint[AV1_NUM_REF_FRAMES];
    unsigned texture[AV1_NUM_REF_FRAMES];
    int gm[AV1_NUM_REF_FRAMES][AV1_TOTAL_REFS_PER_FRAME][6];
} Av1Dpb;

/* ------------------------------------------------------------------------- */
/* Bit writer (MSB first, zero-initialised, overflow-checked)                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    unsigned cap;  /* bytes available */
    unsigned bits; /* bits written so far */
    int overflow;  /* set when a write did not fit; the result is unusable */
} Av1BitWriter;

static inline void av1_bw_init(Av1BitWriter *bw, uint8_t *buf, unsigned cap) {
    bw->buf = buf;
    bw->cap = cap;
    bw->bits = 0;
    bw->overflow = 0;
    if (buf && cap)
        memset(buf, 0, cap);
}

static inline void av1_bw_put(Av1BitWriter *bw, uint32_t value, unsigned nbits) {
    unsigned i;

    if (nbits == 0)
        return;
    if (nbits > 32 || !bw->buf ||
        ((uint64_t)bw->bits + nbits + 7u) / 8u > (uint64_t)bw->cap) {
        bw->overflow = 1;
        bw->bits += nbits;
        return;
    }
    for (i = 0; i < nbits; i++) {
        unsigned pos = bw->bits + i;
        unsigned bit = (value >> (nbits - 1u - i)) & 1u;
        if (bit)
            bw->buf[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7u));
        /* zero bits are already clear: the buffer is zero-initialised */
    }
    bw->bits += nbits;
}

/* su(n): two's complement in n bits (what dav1d/ffmpeg parse, 4.10.5). */
static inline void av1_bw_put_su(Av1BitWriter *bw, int value, unsigned nbits) {
    uint32_t mask = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1u);
    av1_bw_put(bw, (uint32_t)value & mask, nbits);
}

/* ns(n): non-symmetric unsigned coding (used for tile widths). */
static inline void av1_bw_put_ns(Av1BitWriter *bw, uint32_t n, uint32_t value) {
    unsigned w = 0;
    uint32_t m, v, extra;

    if (n == 0)
        return;
    while (w < 31 && (1u << w) <= n)
        w++;
    m = (1u << w) - n;
    if (value < m) {
        av1_bw_put(bw, value, w - 1u);
    } else {
        v = m + ((value - m) >> 1);
        extra = (value - m) & 1u;
        av1_bw_put(bw, v, w - 1u);
        av1_bw_put(bw, extra, 1);
    }
}

/* increment(min, max): write "1" while the value has not been reached. */
static inline void av1_bw_put_increment(Av1BitWriter *bw, unsigned min,
                                        unsigned max, unsigned value) {
    unsigned v;
    for (v = min; v < max; v++)
        av1_bw_put(bw, (v < value) ? 1u : 0u, 1);
}

/* subexp(max): the exponential-Golomb-like coding used for gm_params. */
static inline void av1_bw_put_subexp(Av1BitWriter *bw, uint32_t range_max,
                                     uint32_t value) {
    unsigned max_len, len, range_bits, log2 = 0;
    uint32_t range_offset;

    if (range_max < 8)
        return; /* only used with the gm ranges, all >= 1025 */
    while ((1ull << (log2 + 1)) <= (uint64_t)(range_max - 1))
        log2++;
    max_len = log2 - 3u;
    if (value < 8) {
        len = 0;
        range_bits = 3;
        range_offset = 0;
    } else {
        unsigned l = 0;
        while (l < 31u && (1u << (l + 1)) <= value)
            l++;
        range_bits = l;
        len = range_bits - 2u;
        if (len > max_len) {
            range_bits--;
            len = max_len;
        }
        range_offset = 1u << range_bits;
    }
    av1_bw_put_increment(bw, 0, max_len, len);
    if (len < max_len)
        av1_bw_put(bw, value - range_offset, range_bits);
    else
        av1_bw_put_ns(bw, range_max - range_offset, value - range_offset);
}

/* recenter_finite_nonneg() / recenter_nonneg() (5.9.24, the encoder half of
 * decode_signed_subexp_with_ref): map an absolute value onto the symbol the
 * decoder will inverse-map, biasing towards values close to the reference. */
static inline uint32_t av1_recenter_nonneg(uint32_t r, uint32_t v) {
    if (v > (r << 1))
        return v;
    if (v >= r)
        return (v - r) << 1;
    return ((r - v) << 1) - 1;
}

static inline uint32_t av1_recenter_finite_nonneg(uint32_t n, uint32_t r,
                                                  uint32_t v) {
    if ((r << 1) <= n)
        return av1_recenter_nonneg(r, v);
    return av1_recenter_nonneg(n - 1u - r, n - 1u - v);
}

/* write_signed_primitive_refsubexpfin(n, k, ref, v) -- the AV1 syntax element
 * that codes a SIGNED value in [-n+1, n-1] as a delta from `ref`:
 *
 *     coded = recenter_finite_nonneg(2n-1, ref + n-1, v + n-1)
 *     subexp(2n-1, k=3, coded)
 *
 * This is what global_motion_params() (5.9.24) uses for every gm parameter.
 * The old code wrote the value biased by the midpoint through a plain subexp
 * (an ABSOLUTE code), which is a different symbol whenever the reference frame
 * carries a non-identity transform: the decoder inverse-centred our symbol
 * against ITS reference and landed on a different matrix.  Identity-only
 * content codes no bits at all either way, which is why this never showed up
 * on real streams. */
static inline void av1_bw_put_signed_refsubexp(Av1BitWriter *bw, uint32_t n,
                                               int32_t ref, int32_t value) {
    uint32_t scaled_n, r, v;

    if (n <= 1)
        return;
    scaled_n = (n << 1) - 1u;
    r = (uint32_t)(ref + (int32_t)n - 1);
    v = (uint32_t)(value + (int32_t)n - 1);
    if (r > scaled_n)
        r = scaled_n; /* defensive: ref/v are in [0, 2n-1] by construction */
    if (v > scaled_n)
        v = scaled_n;
    av1_bw_put_subexp(bw, scaled_n,
                      av1_recenter_finite_nonneg(scaled_n, r, v));
}

/* trailing_bits() + byte alignment: a one bit, then zeros to the byte. */
static inline void av1_bw_trailing_bits(Av1BitWriter *bw) {
    av1_bw_put(bw, 1, 1);
    while (bw->bits & 7u)
        av1_bw_put(bw, 0, 1);
}

/* A complete type-3 OBU ends in trailing_bits; an embedded type-6 frame
 * header only byte-aligns before tile_group payload. */
static inline void av1_frame_header_align(Av1BitWriter *bw,
                                          unsigned standalone_header) {
    if (standalone_header) {
        av1_bw_trailing_bits(bw);
    } else {
        while (bw->bits & 7u)
            av1_bw_put(bw, 0, 1);
    }
}

/* DXVA CDEF union: low six bits are primary, high two are secondary. The
 * serializer stores primary in the syntax nibble and secondary in two bits. */
static inline unsigned av1_cdef_pack_dxva(unsigned primary, unsigned secondary) {
    return ((primary & 0x3fu) << 2) | (secondary & 0x3u);
}

/* One production qmatrix syntax writer shared by the full header and tests. */
static inline void av1_write_qmatrix_params(Av1BitWriter *bw,
                                             const Av1SeqParams *seq,
                                             const Av1FrameParams *f) {
    av1_bw_put(bw, f->using_qmatrix ? 1u : 0u, 1);
    if (f->using_qmatrix) {
        av1_bw_put(bw, f->qm_y & 15u, 4);
        av1_bw_put(bw, f->qm_u & 15u, 4);
        if (seq->separate_uv_delta_q)
            av1_bw_put(bw, f->qm_v & 15u, 4);
    }
}

/* Account an accepted direct tag mapping. Kept as the same production helper
 * used by the collector so the native test can exercise the real increments. */
static inline void av1_note_tag_mapping(uint64_t *direct_maps,
                                        unsigned *mappings_out) {
    if (direct_maps)
        ++*direct_maps;
    if (mappings_out)
        ++*mappings_out;
}

static inline unsigned av1_bw_bytes(const Av1BitWriter *bw) {
    return (bw->bits + 7u) / 8u;
}

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static inline unsigned av1_floor_log2(uint32_t v) {
    unsigned n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

/* Number of bits needed to hold v (v = 0 -> 1 bit). */
static inline unsigned av1_dim_bits(uint32_t v) {
    return av1_floor_log2(v) + 1u;
}

/* ceil_log2(n), n >= 1. */
static inline unsigned av1_ceil_log2(unsigned n) {
    unsigned k = 0;
    while (k < 31 && (1u << k) < n)
        k++;
    return k;
}

/* tile_log2(blksize, target) (5.9.15): smallest k with blksize << k >= target. */
static inline unsigned av1_tile_log2(unsigned blksize, unsigned target) {
    unsigned k = 0;
    while (k < 31 && (uint64_t)blksize << k < target)
        k++;
    return k;
}

static inline unsigned av1_min_u(unsigned a, unsigned b) { return a < b ? a : b; }
static inline unsigned av1_max_u(unsigned a, unsigned b) { return a > b ? a : b; }

/* LEB128 (obu_size). */
static inline unsigned av1_leb128(uint8_t *out, unsigned cap, uint64_t value) {
    unsigned n = 0;
    do {
        uint8_t byte = (uint8_t)(value & 0x7Fu);
        value >>= 7;
        if (value)
            byte |= 0x80u;
        if (n < cap)
            out[n] = byte;
        n++;
    } while (value);
    return n <= cap ? n : 0;
}

/* obu_header + obu_size + payload.  0 when it does not fit. */
static inline unsigned av1_write_obu(uint8_t *out, unsigned cap, unsigned type,
                                     const uint8_t *payload, unsigned size) {
    unsigned n, at;

    if (!out || size == 0 || cap < 2u + size)
        return 0;
    out[0] = (uint8_t)((type << 3) | 0x02u);
    n = av1_leb128(out + 1, cap - 1u, size);
    if (n == 0)
        return 0;
    at = 1u + n;
    if (at + size > cap)
        return 0;
    memcpy(out + at, payload, size);
    return at + size;
}

/* ------------------------------------------------------------------------- */
/* Sequence header (5.5.1)                                                    */
/* ------------------------------------------------------------------------- */

/* seq_level_idx for a picture of w x h at ~30 fps with the given tiling.  The
 * client's DXVA_PicParams_AV1 does not carry the stream's level, so one has to
 * be picked; Annex A's limits are used so the value is legal for the size we
 * are about to decode.  A too-high level can make a strict host decoder reject
 * the stream, so this is deliberately the smallest matching level. */
static inline unsigned av1_level_from_size(unsigned w, unsigned h,
                                           unsigned tile_cols,
                                           unsigned tile_rows) {
    static const struct {
        unsigned idx, pic_size, h_size, v_size;
        uint64_t display_rate;
        unsigned max_tiles, max_tile_cols;
    } levels[] = {
        { 0, 147456, 2048, 1152, 4423680ull, 8, 4 },
        { 1, 278784, 2816, 1584, 8363520ull, 8, 4 },
        { 4, 665856, 4352, 2448, 19975680ull, 16, 6 },
        { 5, 1065024, 5504, 3096, 31950720ull, 16, 6 },
        { 8, 2359296, 6144, 3456, 70778880ull, 32, 8 },
        { 9, 2359296, 6144, 3456, 141557760ull, 32, 8 },
        { 12, 8912896, 8192, 4352, 267386880ull, 64, 8 },
        { 13, 8912896, 8192, 4352, 534773760ull, 64, 8 },
        { 14, 8912896, 8192, 4352, 1069547520ull, 64, 8 },
        { 16, 35651584, 16384, 8704, 1069547520ull, 128, 16 },
        { 18, 35651584, 16384, 8704, 4278190080ull, 128, 16 },
    };
    unsigned i, pics = w * h, tiles = av1_max_u(1u, tile_cols * tile_rows);
    uint64_t rate = (uint64_t)pics * 30u;

    for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (pics > levels[i].pic_size || w > levels[i].h_size ||
            h > levels[i].v_size)
            continue;
        if (rate > levels[i].display_rate)
            continue;
        if (tiles > levels[i].max_tiles || tile_cols > levels[i].max_tile_cols)
            continue;
        return levels[i].idx;
    }
    return 18; /* level 6.2: above every size we accept, still legal */
}

/* seq_level_idx is > 7 for every level above 4.1, which is when seq_tier is
 * coded.  The tier is always Main (0): we never ask for a High-tier rate. */
static inline unsigned av1_sequence_header_payload(const Av1SeqParams *seq,
                                                   uint8_t *out, unsigned cap) {
    Av1BitWriter bw;
    unsigned wbits = av1_dim_bits(seq->max_width ? seq->max_width - 1u : 0);
    unsigned hbits = av1_dim_bits(seq->max_height ? seq->max_height - 1u : 0);
    unsigned level = (seq->level_idx <= 23u)
                         ? seq->level_idx
                         : av1_level_from_size(seq->max_width, seq->max_height, 1, 1);
    uint32_t max_w = seq->max_width ? seq->max_width - 1u : 0;
    uint32_t max_h = seq->max_height ? seq->max_height - 1u : 0;

    av1_bw_init(&bw, out, cap);

    av1_bw_put(&bw, seq->profile, 3); /* seq_profile */
    av1_bw_put(&bw, 0, 1);            /* still_picture */
    av1_bw_put(&bw, 0, 1);            /* reduced_still_picture_header */
    av1_bw_put(&bw, 0, 1);            /* timing_info_present_flag */
    av1_bw_put(&bw, 0, 1);            /* initial_display_delay_present_flag */
    av1_bw_put(&bw, 0, 5);            /* operating_points_cnt_minus_1 */
    av1_bw_put(&bw, 0, 12);           /* operating_point_idc[0] */
    av1_bw_put(&bw, level, 5);        /* seq_level_idx[0] */
    if (level > 7)
        av1_bw_put(&bw, 0, 1); /* seq_tier[0] = Main */

    av1_bw_put(&bw, wbits - 1u, 4); /* frame_width_bits_minus_1 */
    av1_bw_put(&bw, hbits - 1u, 4); /* frame_height_bits_minus_1 */
    av1_bw_put(&bw, max_w, wbits);
    av1_bw_put(&bw, max_h, hbits);
    av1_bw_put(&bw, 0, 1); /* frame_id_numbers_present_flag */

    av1_bw_put(&bw, seq->use_128x128_superblock ? 1u : 0u, 1);
    av1_bw_put(&bw, seq->enable_filter_intra ? 1u : 0u, 1);
    av1_bw_put(&bw, seq->enable_intra_edge_filter ? 1u : 0u, 1);
    av1_bw_put(&bw, seq->enable_interintra_compound ? 1u : 0u, 1);
    av1_bw_put(&bw, seq->enable_masked_compound ? 1u : 0u, 1);
    /* DXVA carries the per-frame allow_warped_motion but not the sequence
     * enable_warped_motion, so the sequence says "may be present" and the
     * frame header carries the value the client did give us.  enable_superres
     * and enable_restoration are handled the same way (DXVA gives the
     * frame-level use_superres / per-plane restoration types). */
    av1_bw_put(&bw, 1, 1); /* enable_warped_motion */
    av1_bw_put(&bw, seq->enable_dual_filter ? 1u : 0u, 1);
    av1_bw_put(&bw, seq->enable_order_hint ? 1u : 0u, 1);
    if (seq->enable_order_hint) {
        av1_bw_put(&bw, seq->enable_jnt_comp ? 1u : 0u, 1);
        av1_bw_put(&bw, seq->enable_ref_frame_mvs ? 1u : 0u, 1);
    }
    /* Both "choose" flags set: the FRAME header carries
     * allow_screen_content_tools and force_integer_mv, which are fields DXVA
     * does provide (coding.screen_content_tools / coding.integer_mv). */
    av1_bw_put(&bw, 1, 1); /* seq_choose_screen_content_tools */
    av1_bw_put(&bw, 1, 1); /* seq_choose_integer_mv */
    if (seq->enable_order_hint) {
        unsigned bits = seq->order_hint_bits ? seq->order_hint_bits : 1u;
        av1_bw_put(&bw, bits - 1u, 3); /* order_hint_bits_minus_1 */
    }
    av1_bw_put(&bw, seq->enable_superres ? 1u : 0u, 1);
    av1_bw_put(&bw, seq->enable_cdef ? 1u : 0u, 1);
    av1_bw_put(&bw, seq->enable_restoration ? 1u : 0u, 1);

    /* color_config().  DXVA_PicParams_AV1 carries no colour description, so the
     * BT.709 / SDR triplet is emitted (AV1FIX round 26: it used to emit
     * color_description_present_flag = 0, which is legal but made our sequence
     * header 3 bytes shorter than every encoder's and left the host without the
     * matrix coefficients; t6_av1.mkv's own header says 1/1/1). */
    av1_bw_put(&bw, seq->bit_depth > 8 ? 1u : 0u, 1); /* high_bitdepth */
    if (seq->profile == 2 && seq->bit_depth > 8)
        av1_bw_put(&bw, seq->bit_depth == 12 ? 1u : 0u, 1); /* twelve_bit */
    if (seq->profile != 1)
        av1_bw_put(&bw, seq->mono_chrome ? 1u : 0u, 1);
    av1_bw_put(&bw, 1, 1);     /* color_description_present_flag */
    av1_bw_put(&bw, 1, 8);     /* color_primaries          = CP_BT_709   */
    av1_bw_put(&bw, 1, 8);     /* transfer_characteristics = TC_BT_709   */
    av1_bw_put(&bw, 1, 8);     /* matrix_coefficients      = MC_BT_709   */
    if (seq->mono_chrome) {
        av1_bw_put(&bw, seq->color_range ? 1u : 0u, 1);
    } else {
        av1_bw_put(&bw, seq->color_range ? 1u : 0u, 1);
        if (seq->profile == 2 && seq->bit_depth == 12) {
            av1_bw_put(&bw, seq->subsampling_x ? 1u : 0u, 1);
            if (seq->subsampling_x)
                av1_bw_put(&bw, seq->subsampling_y ? 1u : 0u, 1);
        }
        if (seq->subsampling_x && seq->subsampling_y)
            av1_bw_put(&bw, 1, 2); /* chroma_sample_position = CSP_VERTICAL */
        av1_bw_put(&bw, seq->separate_uv_delta_q ? 1u : 0u, 1);
    }
    av1_bw_put(&bw, seq->film_grain_params_present ? 1u : 0u, 1);

    av1_bw_trailing_bits(&bw);
    return bw.overflow ? 0u : av1_bw_bytes(&bw);
}

/* Full sequence_header OBU. */
static inline unsigned av1_sequence_header_obu(const Av1SeqParams *seq,
                                               uint8_t *out, unsigned cap) {
    uint8_t payload[128];
    unsigned size = av1_sequence_header_payload(seq, payload, sizeof(payload));

    if (size == 0)
        return 0;
    return av1_write_obu(out, cap, AV1_OBU_SEQUENCE_HEADER, payload, size);
}

/* temporal_delimiter OBU: header + obu_size = 0, no payload. */
static inline unsigned av1_temporal_delimiter_obu(uint8_t *out, unsigned cap) {
    if (!out || cap < 2)
        return 0;
    out[0] = (uint8_t)((AV1_OBU_TEMPORAL_DELIMITER << 3) | 0x02u);
    out[1] = 0;
    return 2;
}

/* ------------------------------------------------------------------------- */
/* Frame header helpers                                                       */
/* ------------------------------------------------------------------------- */

static inline unsigned av1_tile_size_bytes(const Av1Tile *tiles, unsigned ntiles) {
    uint32_t max_tile = 0;
    unsigned i, bytes = 1;

    for (i = 0; i < ntiles; i++) {
        if (tiles[i].size > max_tile)
            max_tile = tiles[i].size;
    }
    if (max_tile > 1u)
        bytes = (av1_dim_bits(max_tile - 1u) + 7u) / 8u;
    return bytes > 4u ? 4u : bytes;
}

/* superres_params() (5.9.6).  use_superres is only coded when the sequence
 * header enabled superres, and the sequence header only enables it when a frame
 * actually uses it (see av1_seq_from_pic). */
static inline void av1_write_superres(Av1BitWriter *bw, const Av1SeqParams *seq,
                                      const Av1FrameParams *f) {
    unsigned denom = f->superres_denom ? f->superres_denom : AV1_SUPERRES_NUM;
    unsigned use = denom != AV1_SUPERRES_NUM;

    if (!seq->enable_superres)
        return;
    av1_bw_put(bw, use, 1);
    if (use)
        av1_bw_put(bw, denom - AV1_SUPERRES_DENOM_MIN, 3);
}

/* frame_size() (5.9.7).  The caller decides frame_size_override_flag; when it
 * is 0 the size is the sequence maximum and nothing is coded. */
static inline void av1_write_frame_size(Av1BitWriter *bw, const Av1SeqParams *seq,
                                        const Av1FrameParams *f,
                                        unsigned override) {
    if (override) {
        av1_bw_put(bw, f->width ? f->width - 1u : 0,
                   av1_dim_bits(seq->max_width ? seq->max_width - 1u : 0));
        av1_bw_put(bw, f->height ? f->height - 1u : 0,
                   av1_dim_bits(seq->max_height ? seq->max_height - 1u : 0));
    }
    av1_write_superres(bw, seq, f);
}

static inline void av1_write_render_size(Av1BitWriter *bw) {
    av1_bw_put(bw, 0, 1); /* render_and_frame_size_different */
}

/* coded_lossless (5.9.12), needed by loop_filter/cdef/lr/tx_mode. */
static inline unsigned av1_coded_lossless(const Av1FrameParams *f) {
    unsigned i, lossless = 1;

    for (i = 0; i < AV1_MAX_SEGMENTS; i++) {
        int q = (int)f->base_q_idx;
        if (f->segmentation_enabled &&
            (f->feature_mask[i] & (1u << AV1_SEG_LVL_ALT_Q)))
            q += f->feature_data[i][AV1_SEG_LVL_ALT_Q];
        if (q < 0)
            q = 0;
        if (q > 255)
            q = 255;
        if (q || f->delta_q_y_dc || f->delta_q_u_ac || f->delta_q_u_dc ||
            f->delta_q_v_ac || f->delta_q_v_dc)
            lossless = 0;
    }
    return lossless;
}

/* get_relative_dist() (5.9.2). */
static inline int av1_relative_dist(const Av1SeqParams *seq, int a, int b) {
    int diff, m;
    if (!seq->enable_order_hint || seq->order_hint_bits == 0)
        return 0;
    diff = a - b;
    m = 1 << (seq->order_hint_bits - 1u);
    return (diff & (m - 1)) - (diff & m);
}

/* skip_mode_allowed (5.9.22): the decoder derives it from the DPB order hints
 * and the reference indices, so the writer must reach the same answer or every
 * following bit shifts. */
static inline int av1_skip_mode_allowed(const Av1SeqParams *seq,
                                        const Av1FrameParams *f,
                                        const Av1Dpb *dpb) {
    int forward_idx = -1, backward_idx = -1;
    int forward_hint = 0, backward_hint = 0;
    unsigned i;

    if (f->frame_type != AV1_FRAME_INTER || !f->reference_select ||
        !seq->enable_order_hint || seq->order_hint_bits == 0)
        return 0;

    for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
        int ref_hint = (int)dpb->order_hint[f->ref_frame_idx[i] & 7u];
        int dist = av1_relative_dist(seq, ref_hint, (int)f->order_hint);
        if (dist < 0) {
            if (forward_idx < 0 ||
                av1_relative_dist(seq, ref_hint, forward_hint) > 0) {
                forward_idx = (int)i;
                forward_hint = ref_hint;
            }
        } else if (dist > 0) {
            if (backward_idx < 0 ||
                av1_relative_dist(seq, ref_hint, backward_hint) < 0) {
                backward_idx = (int)i;
                backward_hint = ref_hint;
            }
        }
    }
    if (forward_idx < 0)
        return 0;
    if (backward_idx >= 0)
        return 1;
    {
        int second = -1, second_hint = 0;
        for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
            int ref_hint = (int)dpb->order_hint[f->ref_frame_idx[i] & 7u];
            if (av1_relative_dist(seq, ref_hint, forward_hint) < 0) {
                if (second < 0 ||
                    av1_relative_dist(seq, ref_hint, second_hint) > 0) {
                    second = (int)i;
                    second_hint = ref_hint;
                }
            }
        }
        return second >= 0;
    }
}

/* tile_info() (5.9.15).  We always signal NON-uniform spacing: DXVA gives the
 * width/height of every tile in superblocks, which represents the uniform case
 * as well, and the entropy decoder only ever sees the resulting boundaries.
 * The max_width/max_height values are what the decoder derives, so they must
 * be recomputed exactly (ns() codes relative to them). */
static inline void av1_write_tile_info(Av1BitWriter *bw, const Av1SeqParams *seq,
                                       const Av1FrameParams *f,
                                       const Av1Tile *tiles, unsigned ntiles) {
    unsigned sb_shift = seq->use_128x128_superblock ? AV1_SB_SHIFT_128
                                                    : AV1_SB_SHIFT_64;
    unsigned sb_size = sb_shift + 2u;
    unsigned mi_cols = 2u * ((f->width + 7u) >> 3);
    unsigned mi_rows = 2u * ((f->height + 7u) >> 3);
    unsigned sb_cols = seq->use_128x128_superblock ? ((mi_cols + 31u) >> 5)
                                                   : ((mi_cols + 15u) >> 4);
    unsigned sb_rows = seq->use_128x128_superblock ? ((mi_rows + 31u) >> 5)
                                                   : ((mi_rows + 15u) >> 4);
    unsigned max_tile_width_sb = AV1_MAX_TILE_WIDTH >> sb_size;
    unsigned max_tile_area_sb = AV1_MAX_TILE_AREA >> (2u * sb_size);
    unsigned min_log2_tile_cols = av1_tile_log2(max_tile_width_sb, sb_cols);
    unsigned min_log2_tiles =
        av1_max_u(min_log2_tile_cols,
                  av1_tile_log2(max_tile_area_sb, sb_rows * sb_cols));
    unsigned cols = av1_max_u(1u, av1_min_u(f->tile_cols, sb_cols));
    unsigned rows = av1_max_u(1u, av1_min_u(f->tile_rows, sb_rows));
    unsigned cols_log2, rows_log2, bytes, start, i, widest = 0;
    unsigned max_tile_height_sb;
    unsigned max_log2_tile_cols =
        av1_tile_log2(1u, av1_min_u(sb_cols, AV1_MAX_TILE_COLS));
    unsigned max_log2_tile_rows =
        av1_tile_log2(1u, av1_min_u(sb_rows, AV1_MAX_TILE_ROWS));

    if (cols > AV1_MAX_TILE_COLS)
        cols = AV1_MAX_TILE_COLS;
    if (rows > AV1_MAX_TILE_ROWS)
        rows = AV1_MAX_TILE_ROWS;

    /* AV1FIX (2026-10-26, round 26): a 1x1 grid has a 3-bit uniform encoding
     * (uniform_tile_spacing_flag = 1 plus the two zero log2 increments) and an
     * 11-bit non-uniform one.  Both describe the same single tile, so the
     * decoder did not care, but the encoder's form is the cheap one and using it
     * makes our header byte-for-byte identical to the source stream for the
     * common single-tile case (verified against ffmpeg's trace_headers: TU0
     * uniform_tile_spacing_flag=1, tile_cols_log2=0, tile_rows_log2=0). */
    if (cols == 1u && rows == 1u && min_log2_tile_cols == 0 &&
        min_log2_tiles == 0) {
        av1_bw_put(bw, 1, 1); /* uniform_tile_spacing_flag */
        if (max_log2_tile_cols > 0)
            av1_bw_put(bw, 0, 1); /* increment_tile_cols_log2: stays 0 */
        if (max_log2_tile_rows > 0)
            av1_bw_put(bw, 0, 1); /* increment_tile_rows_log2: stays 0 */
        return;
    }

    av1_bw_put(bw, 0, 1); /* uniform_tile_spacing_flag */

    start = 0;
    for (i = 0; i < cols; i++) {
        unsigned max_width = av1_min_u(sb_cols - start, max_tile_width_sb);
        unsigned w;
        if (i + 1u == cols) {
            w = sb_cols - start;
        } else {
            w = f->tile_width_sb[i];
            if (w == 0)
                w = max_width;
            if (w > max_width)
                w = max_width;
            if (start + w >= sb_cols)
                w = sb_cols - start;
        }
        av1_bw_put_ns(bw, max_width, w - 1u);
        widest = av1_max_u(widest, w);
        start += w;
    }
    cols_log2 = av1_ceil_log2(cols);

    if (min_log2_tiles > 0)
        max_tile_area_sb = (sb_rows * sb_cols) >> (min_log2_tiles + 1u);
    else
        max_tile_area_sb = sb_rows * sb_cols;
    if (widest == 0)
        widest = 1;
    max_tile_height_sb = av1_max_u(max_tile_area_sb / widest, 1u);

    start = 0;
    for (i = 0; i < rows; i++) {
        unsigned max_height = av1_min_u(sb_rows - start, max_tile_height_sb);
        unsigned h;
        if (i + 1u == rows) {
            h = sb_rows - start;
        } else {
            h = f->tile_height_sb[i];
            if (h == 0)
                h = max_height;
            if (h > max_height)
                h = max_height;
            if (start + h >= sb_rows)
                h = sb_rows - start;
        }
        av1_bw_put_ns(bw, max_height, h - 1u);
        start += h;
    }
    rows_log2 = av1_ceil_log2(rows);

    if (cols_log2 > 0 || rows_log2 > 0) {
        unsigned ctx = f->context_update_tile_id;
        unsigned ntiles_max = cols * rows;
        (void)ntiles;
        if (ctx >= ntiles_max)
            ctx = 0;
        av1_bw_put(bw, ctx, cols_log2 + rows_log2);
        bytes = f->tile_size_bytes ? f->tile_size_bytes
                                  : av1_tile_size_bytes(tiles, ntiles);
        av1_bw_put(bw, bytes - 1u, 2); /* tile_size_bytes_minus_1 */
    }
}

/* ------------------------------------------------------------------------- */
/* Frame header (5.9.2 uncompressed_header)                                   */
/* ------------------------------------------------------------------------- */

static inline unsigned av1_frame_header_payload(const Av1SeqParams *seq,
                                                const Av1FrameParams *f,
                                                const Av1Dpb *dpb,
                                                const Av1Tile *tiles,
                                                unsigned ntiles,
                                                unsigned refresh_frame_flags,
                                                unsigned standalone_header,
                                                uint8_t *out, unsigned cap) {
    Av1BitWriter bw;
    unsigned is_intra, order_bits;
    unsigned coded_lossless, all_lossless, override;
    unsigned i, j;

    is_intra = (f->frame_type == AV1_FRAME_KEY ||
                f->frame_type == AV1_FRAME_INTRA_ONLY);
    order_bits = (seq->enable_order_hint && seq->order_hint_bits)
                     ? seq->order_hint_bits
                     : 0u;
    override = (f->width != seq->max_width || f->height != seq->max_height) ? 1u : 0u;

    av1_bw_init(&bw, out, cap);

    av1_bw_put(&bw, 0, 1); /* show_existing_frame */
    av1_bw_put(&bw, f->frame_type, 2);
    av1_bw_put(&bw, f->show_frame ? 1u : 0u, 1);
    /* showable_frame is INFERRED when show_frame == 1 (5.9.2: "if (show_frame)
     * showable_frame = frame_type != KEY_FRAME"), and CODED when it is 0.  The
     * previous version never wrote it, so every hidden catch-up frame's header
     * was one bit short and everything after it (beginning with
     * error_resilient_mode) was misparsed. */
    if (!f->show_frame)
        av1_bw_put(&bw, f->showable_frame ? 1u : 0u, 1);
    if (f->frame_type == AV1_FRAME_SWITCH ||
        (f->frame_type == AV1_FRAME_KEY && f->show_frame)) {
        /* error_resilient_mode inferred 1 */
    } else {
        av1_bw_put(&bw, f->error_resilient_mode ? 1u : 0u, 1);
    }
    av1_bw_put(&bw, f->disable_cdf_update ? 1u : 0u, 1);

    /* seq_choose_screen_content_tools == 1 in the sequence header we emit. */
    av1_bw_put(&bw, f->allow_screen_content_tools ? 1u : 0u, 1);
    if (f->allow_screen_content_tools)
        av1_bw_put(&bw, f->force_integer_mv ? 1u : 0u, 1);
    /* frame_id_numbers_present_flag == 0, so no current_frame_id. */

    if (f->frame_type == AV1_FRAME_SWITCH)
        ; /* frame_size_override_flag inferred 1 */
    else
        av1_bw_put(&bw, override, 1);

    if (order_bits)
        av1_bw_put(&bw, f->order_hint & ((1u << order_bits) - 1u), order_bits);

    if (is_intra || f->error_resilient_mode)
        ; /* primary_ref_frame inferred AV1_PRIMARY_REF_NONE */
    else
        av1_bw_put(&bw, f->primary_ref_frame, 3);

    if (f->frame_type == AV1_FRAME_SWITCH ||
        (f->frame_type == AV1_FRAME_KEY && f->show_frame))
        ; /* refresh_frame_flags inferred 0xFF */
    else
        av1_bw_put(&bw, refresh_frame_flags & 0xFFu, 8);

    /* ref_order_hint[]: coded only for an error-resilient non-intra frame. */
    if ((!is_intra || refresh_frame_flags != 0xFFu) && order_bits &&
        f->error_resilient_mode) {
        for (i = 0; i < AV1_NUM_REF_FRAMES; i++)
            av1_bw_put(&bw, dpb->order_hint[i] & ((1u << order_bits) - 1u),
                       order_bits);
    }

    if (is_intra) {
        av1_write_frame_size(&bw, seq, f, override);
        av1_write_render_size(&bw);
        /* allow_intrabc() is coded when the frame is not superres-scaled. */
        if (f->allow_screen_content_tools && f->superres_denom == AV1_SUPERRES_NUM)
            av1_bw_put(&bw, f->allow_intrabc ? 1u : 0u, 1);
    } else {
        if (!seq->enable_order_hint)
            ; /* frame_refs_short_signaling inferred 0 */
        else
            av1_bw_put(&bw, 0, 1); /* frame_refs_short_signaling */
        for (i = 0; i < AV1_REFS_PER_FRAME; i++)
            av1_bw_put(&bw, f->ref_frame_idx[i] & 7u, 3);
        if (override && !f->error_resilient_mode) {
            /* frame_size_with_refs(): no reference matches (found_ref = 0 for
             * all seven), so the explicit size follows. */
            for (i = 0; i < AV1_REFS_PER_FRAME; i++)
                av1_bw_put(&bw, 0, 1);
            av1_write_frame_size(&bw, seq, f, override);
            av1_write_render_size(&bw);
        } else {
            av1_write_frame_size(&bw, seq, f, override);
            av1_write_render_size(&bw);
        }
        if (f->force_integer_mv)
            ; /* allow_high_precision_mv inferred 0 */
        else
            av1_bw_put(&bw, f->allow_high_precision_mv ? 1u : 0u, 1);
        /* interpolation_filter() */
        if (f->interpolation_filter == AV1_INTERP_SWITCHABLE) {
            av1_bw_put(&bw, 1, 1); /* is_filter_switchable */
        } else {
            av1_bw_put(&bw, 0, 1);
            av1_bw_put(&bw, f->interpolation_filter & 3u, 2);
        }
        av1_bw_put(&bw, f->is_motion_mode_switchable ? 1u : 0u, 1);
        if (f->error_resilient_mode || !seq->enable_ref_frame_mvs)
            ; /* use_ref_frame_mvs inferred 0 */
        else
            av1_bw_put(&bw, f->use_ref_frame_mvs ? 1u : 0u, 1);
    }

    if (f->disable_cdf_update)
        ; /* disable_frame_end_update_cdf inferred 1 */
    else
        av1_bw_put(&bw, f->disable_frame_end_update_cdf ? 1u : 0u, 1);

    av1_write_tile_info(&bw, seq, f, tiles, ntiles);

    /* quantization_params() (5.9.17) */
    av1_bw_put(&bw, f->base_q_idx & 0xFFu, 8);
    {
        int y_dc = f->delta_q_y_dc;
        int u_dc = f->delta_q_u_dc, u_ac = f->delta_q_u_ac;
        int v_dc = f->delta_q_v_dc, v_ac = f->delta_q_v_ac;
        int diff_uv = (u_dc != v_dc || u_ac != v_ac);

        /* delta_q(): 0 -> "0", else "1" + su(7). */
        if (y_dc) {
            av1_bw_put(&bw, 1, 1);
            av1_bw_put_su(&bw, y_dc, 7);
        } else {
            av1_bw_put(&bw, 0, 1);
        }
        /* diff_uv_delta is only coded when the SEQUENCE header set
         * separate_uv_delta_q (5.9.17); av1_seq_from_pic() derives that from
         * these very deltas. */
        if (seq->separate_uv_delta_q) {
            av1_bw_put(&bw, diff_uv ? 1u : 0u, 1);
        } else {
            diff_uv = 0;
        }
        if (u_dc) {
            av1_bw_put(&bw, 1, 1);
            av1_bw_put_su(&bw, u_dc, 7);
        } else {
            av1_bw_put(&bw, 0, 1);
        }
        if (u_ac) {
            av1_bw_put(&bw, 1, 1);
            av1_bw_put_su(&bw, u_ac, 7);
        } else {
            av1_bw_put(&bw, 0, 1);
        }
        if (diff_uv) {
            if (v_dc) {
                av1_bw_put(&bw, 1, 1);
                av1_bw_put_su(&bw, v_dc, 7);
            } else {
                av1_bw_put(&bw, 0, 1);
            }
            if (v_ac) {
                av1_bw_put(&bw, 1, 1);
                av1_bw_put_su(&bw, v_ac, 7);
            } else {
                av1_bw_put(&bw, 0, 1);
            }
        }
    }
    av1_write_qmatrix_params(&bw, seq, f);

    /* segmentation_params() (5.9.18).  update_data is FORCED to 1 (and coded
     * as such) so every feature value comes from this header: the DXVA values
     * are final, while update_data == 0 would make the decoder inherit them
     * from the primary reference's saved state, which this UMD does not
     * mirror.  update_map / temporal_update DO affect the tile entropy data and
     * are taken from DXVA unchanged. */
    av1_bw_put(&bw, f->segmentation_enabled ? 1u : 0u, 1);
    if (f->segmentation_enabled) {
        unsigned primary_none = (f->primary_ref_frame == AV1_PRIMARY_REF_NONE ||
                                 is_intra || f->error_resilient_mode)
                                    ? 1u
                                    : 0u;
        if (primary_none) {
            /* update_map=1, temporal_update=0, update_data=1 inferred. */
        } else {
            av1_bw_put(&bw, f->segmentation_update_map ? 1u : 0u, 1);
            if (f->segmentation_update_map)
                av1_bw_put(&bw, f->segmentation_temporal_update ? 1u : 0u, 1);
            av1_bw_put(&bw, 1, 1); /* segmentation_update_data */
        }
        {
            static const unsigned bits[AV1_SEG_LVL_MAX] = { 8, 6, 6, 6, 6, 3, 0, 0 };
            static const unsigned sign[AV1_SEG_LVL_MAX] = { 1, 1, 1, 1, 1, 0, 0, 0 };
            for (i = 0; i < AV1_MAX_SEGMENTS; i++) {
                for (j = 0; j < AV1_SEG_LVL_MAX; j++) {
                    unsigned enabled = (f->feature_mask[i] >> j) & 1u;
                    av1_bw_put(&bw, enabled, 1);
                    if (enabled && bits[j] > 0) {
                        if (sign[j])
                            av1_bw_put_su(&bw, f->feature_data[i][j], bits[j] + 1u);
                        else
                            av1_bw_put(&bw, (unsigned)f->feature_data[i][j] &
                                               ((1u << bits[j]) - 1u),
                                       bits[j]);
                    }
                }
            }
        }
    }

    /* delta_q_params() / delta_lf_params() (5.9.19). */
    if (f->base_q_idx > 0)
        av1_bw_put(&bw, f->delta_q_present ? 1u : 0u, 1);
    if (f->base_q_idx > 0 && f->delta_q_present) {
        av1_bw_put(&bw, f->delta_q_res & 3u, 2);
        if (!f->allow_intrabc) {
            av1_bw_put(&bw, f->delta_lf_present ? 1u : 0u, 1);
            if (f->delta_lf_present) {
                av1_bw_put(&bw, f->delta_lf_res & 3u, 2);
                av1_bw_put(&bw, f->delta_lf_multi ? 1u : 0u, 1);
            }
        }
    }

    /* loop_filter_params() (5.9.19). */
    coded_lossless = av1_coded_lossless(f);
    all_lossless = (coded_lossless && f->superres_denom == AV1_SUPERRES_NUM) ? 1u : 0u;
    if (!coded_lossless && !f->allow_intrabc) {
        av1_bw_put(&bw, f->filter_level[0] & 63u, 6);
        av1_bw_put(&bw, f->filter_level[1] & 63u, 6);
        if (f->filter_level[0] || f->filter_level[1]) {
            av1_bw_put(&bw, f->filter_level[2] & 63u, 6);
            av1_bw_put(&bw, f->filter_level[3] & 63u, 6);
        }
        av1_bw_put(&bw, f->sharpness_level & 7u, 3);
        av1_bw_put(&bw, f->mode_ref_delta_enabled ? 1u : 0u, 1);
        if (f->mode_ref_delta_enabled) {
            /* delta_update is forced to 1 and every delta coded explicitly:
             * the DXVA values are the FINAL ones, while update == 0 would make
             * the decoder inherit them from the primary reference's saved
             * state, which this UMD does not mirror. */
            av1_bw_put(&bw, 1, 1);
            for (i = 0; i < AV1_TOTAL_REFS_PER_FRAME; i++) {
                av1_bw_put(&bw, 1, 1);
                av1_bw_put_su(&bw, f->ref_deltas[i], 7);
            }
            for (i = 0; i < 2; i++) {
                av1_bw_put(&bw, 1, 1);
                av1_bw_put_su(&bw, f->mode_deltas[i], 7);
            }
        }
    }

    /* cdef_params() (5.9.19). */
    if (!coded_lossless && !f->allow_intrabc && seq->enable_cdef) {
        unsigned damping = f->cdef_damping >= 3u ? f->cdef_damping - 3u : 0u;
        av1_bw_put(&bw, damping & 3u, 2);
        av1_bw_put(&bw, f->cdef_bits & 3u, 2);
        for (i = 0; i < (1u << (f->cdef_bits & 3u)); i++) {
            av1_bw_put(&bw, (f->cdef_y_strengths[i] >> 2) & 15u, 4);
            av1_bw_put(&bw, f->cdef_y_strengths[i] & 3u, 2);
            av1_bw_put(&bw, (f->cdef_uv_strengths[i] >> 2) & 15u, 4);
            av1_bw_put(&bw, f->cdef_uv_strengths[i] & 3u, 2);
        }
    }

    /* lr_params() (5.9.20). */
    if (!all_lossless && !f->allow_intrabc && seq->enable_restoration) {
        unsigned uses_lr = 0, uses_chroma_lr = 0;
        for (i = 0; i < 3; i++) {
            av1_bw_put(&bw, f->frame_restoration_type[i] & 3u, 2);
            if (f->frame_restoration_type[i] != AV1_RESTORE_NONE) {
                uses_lr = 1;
                if (i > 0)
                    uses_chroma_lr = 1;
            }
        }
        if (uses_lr) {
            unsigned base = seq->use_128x128_superblock ? 7u : 6u;
            unsigned log2 = f->log2_restoration_unit_size[0];
            unsigned shift;
            if (log2 < base)
                log2 = base;
            if (log2 > base + 2u)
                log2 = base + 2u;
            shift = log2 - base;
            if (seq->use_128x128_superblock) {
                av1_bw_put(&bw, shift ? 1u : 0u, 1);
            } else if (shift == 0) {
                av1_bw_put(&bw, 0, 1);
            } else {
                av1_bw_put(&bw, 1, 1);
                av1_bw_put(&bw, shift == 2u ? 1u : 0u, 1);
            }
            if (seq->subsampling_x && seq->subsampling_y && uses_chroma_lr) {
                unsigned l1 = f->log2_restoration_unit_size[1];
                av1_bw_put(&bw, (l1 < log2) ? 1u : 0u, 1); /* lr_uv_shift */
            }
        }
    }

    /* read_tx_mode() (5.9.21): increment(TX_MODE_LARGEST, TX_MODE_SELECT). */
    if (!coded_lossless) {
        unsigned tx = f->tx_mode;
        if (tx < AV1_TX_MODE_LARGEST)
            tx = AV1_TX_MODE_LARGEST;
        if (tx > AV1_TX_MODE_SELECT)
            tx = AV1_TX_MODE_SELECT;
        av1_bw_put_increment(&bw, AV1_TX_MODE_LARGEST, AV1_TX_MODE_SELECT, tx);
    }

    /* frame_reference_mode() (5.9.22). */
    if (!is_intra)
        av1_bw_put(&bw, f->reference_select ? 1u : 0u, 1);

    /* skip_mode_params() (5.9.22). */
    if (av1_skip_mode_allowed(seq, f, dpb))
        av1_bw_put(&bw, f->skip_mode_present ? 1u : 0u, 1);

    /* allow_warped_motion / reduced_tx_set. */
    if (!is_intra && !f->error_resilient_mode)
        av1_bw_put(&bw, f->allow_warped_motion ? 1u : 0u, 1);
    av1_bw_put(&bw, f->reduced_tx_set ? 1u : 0u, 1);

    /* global_motion_params() (5.9.24).  The syntax order per reference is
     * 2,3 (rot/zoom) then 0,1 (translation) then 4,5 (affine shear), NOT
     * 0..5: ffmpeg's cbs_av1 and libaom both read it that way, and the old
     * 0,1,2,3,4,5 order mis-coded every reference that carried global motion
     * (the widths are the same, so the header stayed the same length and the
     * damage only showed up in the decoded picture).
     *
     * AV1 STATEFUL PORT (2026-10-27; libva-v4l2 av1_bitstream.cc
     * write_global_motion_params()): every parameter is coded as a SIGNED
     * subexp DELTA from the same parameter of the frame's PRIMARY REFERENCE
     * (the DPB slot ref_frame_idx[primary_ref_frame] holds), with the identity
     * defaults as the base when primary_ref_frame == PRIMARY_REF_NONE.
     * `>> trans_prec_diff` / `>> alpha_prec_diff` convert our precision-12
     * matrix into the precision the syntax codes. */
    if (!is_intra) {
        const int *base = NULL;
        int prev[6];

        if (f->primary_ref_frame != AV1_PRIMARY_REF_NONE) {
            unsigned slot = f->ref_frame_idx[f->primary_ref_frame] & 7u;
            if (dpb->valid[slot])
                base = &dpb->gm[slot][0][0];
        }
        for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
            unsigned type = f->wm_type[i];
            int32_t wm[6];
            int32_t alpha_prec_diff;
            int32_t trans_prec_diff;
            int32_t trans_bits;

            if (type > AV1_WM_AFFINE)
                type = AV1_WM_IDENTITY;
            for (j = 0; j < 6; j++) {
                wm[j] = f->wm_params[i][j];
                /* Identity default (5.9.24): the two diagonal scale entries are
                 * 1.0 in precision-12 fixed point, everything else is 0. */
                prev[j] = base ? base[i * 6 + j]
                               : (((j % 3u) == 2u)
                                      ? (1 << AV1_WARPEDMODEL_PREC_BITS)
                                      : 0);
            }
            av1_bw_put(&bw, (type != AV1_WM_IDENTITY) ? 1u : 0u, 1); /* is_global */
            if (type == AV1_WM_IDENTITY)
                continue;
            if (type == AV1_WM_ROTZOOM) {
                av1_bw_put(&bw, 1, 1); /* is_rot_zoom */
            } else {
                av1_bw_put(&bw, 0, 1);
                av1_bw_put(&bw, (type == AV1_WM_AFFINE) ? 0u : 1u, 1);
            }

            alpha_prec_diff = AV1_WARPEDMODEL_PREC_BITS - AV1_GM_ALPHA_PREC_BITS;
            if (type >= AV1_WM_ROTZOOM) {
                /* alpha (2, 3): rot/zoom, centred on +-1.0 */
                av1_bw_put_signed_refsubexp(
                    &bw, (1u << AV1_GM_ABS_ALPHA_BITS) + 1u,
                    (prev[2] >> alpha_prec_diff) - (1 << AV1_GM_ALPHA_PREC_BITS),
                    (wm[2] >> alpha_prec_diff) - (1 << AV1_GM_ALPHA_PREC_BITS));
                av1_bw_put_signed_refsubexp(
                    &bw, (1u << AV1_GM_ABS_ALPHA_BITS) + 1u,
                    (prev[3] >> alpha_prec_diff),
                    (wm[3] >> alpha_prec_diff));
            }
            if (type >= AV1_WM_AFFINE) {
                /* alpha (4, 5): affine shear */
                av1_bw_put_signed_refsubexp(
                    &bw, (1u << AV1_GM_ABS_ALPHA_BITS) + 1u,
                    (prev[4] >> alpha_prec_diff),
                    (wm[4] >> alpha_prec_diff));
                av1_bw_put_signed_refsubexp(
                    &bw, (1u << AV1_GM_ABS_ALPHA_BITS) + 1u,
                    (prev[5] >> alpha_prec_diff) - (1 << AV1_GM_ALPHA_PREC_BITS),
                    (wm[5] >> alpha_prec_diff) - (1 << AV1_GM_ALPHA_PREC_BITS));
            }
            trans_bits = (type == AV1_WM_TRANSLATION)
                             ? (int32_t)AV1_GM_ABS_TRANS_ONLY_BITS -
                                   (f->allow_high_precision_mv ? 0 : 1)
                             : (int32_t)AV1_GM_ABS_TRANS_BITS;
            trans_prec_diff = (type == AV1_WM_TRANSLATION)
                                  ? (int32_t)AV1_WARPEDMODEL_PREC_BITS - 3 +
                                        (f->allow_high_precision_mv ? 0 : 1)
                                  : (int32_t)AV1_WARPEDMODEL_PREC_BITS -
                                        (int32_t)AV1_GM_TRANS_PREC_BITS;
            av1_bw_put_signed_refsubexp(&bw, (1u << trans_bits) + 1u,
                                        prev[0] >> trans_prec_diff,
                                        wm[0] >> trans_prec_diff);
            av1_bw_put_signed_refsubexp(&bw, (1u << trans_bits) + 1u,
                                        prev[1] >> trans_prec_diff,
                                        wm[1] >> trans_prec_diff);
        }
    }

    /* film_grain_params() (5.9.30): coded when show_frame || showable_frame. */
    if (seq->film_grain_params_present && (f->show_frame || f->showable_frame)) {
        av1_bw_put(&bw, f->apply_grain ? 1u : 0u, 1);
        if (f->apply_grain) {
            unsigned num_pos_luma, num_pos_chroma;
            av1_bw_put(&bw, f->grain_seed & 0xFFFFu, 16);
            if (f->frame_type == AV1_FRAME_INTER)
                av1_bw_put(&bw, 1, 1); /* update_grain */
            av1_bw_put(&bw, f->num_y_points & 15u, 4);
            for (i = 0; i < f->num_y_points && i < 14u; i++) {
                av1_bw_put(&bw, f->point_y_value[i] & 0xFFu, 8);
                av1_bw_put(&bw, f->point_y_scaling[i] & 0xFFu, 8);
            }
            if (!seq->mono_chrome)
                av1_bw_put(&bw, f->chroma_scaling_from_luma ? 1u : 0u, 1);
            if (!(seq->mono_chrome || f->chroma_scaling_from_luma ||
                  (seq->subsampling_x && seq->subsampling_y && f->num_y_points == 0))) {
                av1_bw_put(&bw, f->num_cb_points & 15u, 4);
                for (i = 0; i < f->num_cb_points && i < 10u; i++) {
                    av1_bw_put(&bw, f->point_cb_value[i] & 0xFFu, 8);
                    av1_bw_put(&bw, f->point_cb_scaling[i] & 0xFFu, 8);
                }
                av1_bw_put(&bw, f->num_cr_points & 15u, 4);
                for (i = 0; i < f->num_cr_points && i < 10u; i++) {
                    av1_bw_put(&bw, f->point_cr_value[i] & 0xFFu, 8);
                    av1_bw_put(&bw, f->point_cr_scaling[i] & 0xFFu, 8);
                }
            }
            av1_bw_put(&bw, f->scaling_shift_minus8 & 3u, 2);
            av1_bw_put(&bw, f->ar_coeff_lag & 3u, 2);
            num_pos_luma = 2u * f->ar_coeff_lag * (f->ar_coeff_lag + 1u);
            num_pos_chroma = f->num_y_points ? num_pos_luma + 1u : num_pos_luma;
            if (f->num_y_points) {
                for (i = 0; i < num_pos_luma && i < 24u; i++)
                    av1_bw_put(&bw, (uint32_t)f->ar_coeffs_y[i] & 0xFFu, 8);
            }
            if (f->chroma_scaling_from_luma || f->num_cb_points) {
                for (i = 0; i < num_pos_chroma && i < 25u; i++)
                    av1_bw_put(&bw, (uint32_t)f->ar_coeffs_cb[i] & 0xFFu, 8);
            }
            if (f->chroma_scaling_from_luma || f->num_cr_points) {
                for (i = 0; i < num_pos_chroma && i < 25u; i++)
                    av1_bw_put(&bw, (uint32_t)f->ar_coeffs_cr[i] & 0xFFu, 8);
            }
            av1_bw_put(&bw, f->ar_coeff_shift_minus6 & 3u, 2);
            av1_bw_put(&bw, f->grain_scale_shift & 3u, 2);
            if (f->num_cb_points) {
                av1_bw_put(&bw, f->cb_mult & 0xFFu, 8);
                av1_bw_put(&bw, f->cb_luma_mult & 0xFFu, 8);
                av1_bw_put(&bw, f->cb_offset & 0x1FFu, 9);
            }
            if (f->num_cr_points) {
                av1_bw_put(&bw, f->cr_mult & 0xFFu, 8);
                av1_bw_put(&bw, f->cr_luma_mult & 0xFFu, 8);
                av1_bw_put(&bw, f->cr_offset & 0x1FFu, 9);
            }
            av1_bw_put(&bw, f->overlap_flag ? 1u : 0u, 1);
            av1_bw_put(&bw, f->clip_to_restricted_range ? 1u : 0u, 1);
        }
    }

    av1_frame_header_align(&bw, standalone_header);
    return bw.overflow ? 0u : av1_bw_bytes(&bw);
}

/* Full frame_header OBU (the multi-tile form; one tile uses av1_frame_obu). */
static inline unsigned av1_frame_header_obu(const Av1SeqParams *seq,
                                            const Av1FrameParams *f,
                                            const Av1Dpb *dpb,
                                            const Av1Tile *tiles,
                                            unsigned ntiles,
                                            unsigned refresh_frame_flags,
                                            uint8_t *out, unsigned cap) {
    uint8_t payload[4096];
    unsigned size = av1_frame_header_payload(seq, f, dpb, tiles, ntiles,
                                             refresh_frame_flags, 1u, payload,
                                             sizeof(payload));
    if (size == 0)
        return 0;
    return av1_write_obu(out, cap, AV1_OBU_FRAME_HEADER, payload, size);
}

/* frame_obu (5.10): the frame header and its tile group in ONE OBU (type 6).
 *
 * AV1FIX (2026-10-26, round 26): this is the form every AV1 encoder emits, and
 * the only form the source streams contain (all 896 packets of t6_av1.mkv are
 * OBU_FRAME).  The UMD used to emit FRAME_HEADER + TILE_GROUP as two OBUs, which
 * is also legal -- ffmpeg decodes both to identical pictures -- but with the two
 * separate OBUs the host produced a picture for the key frame and for nothing
 * else, so the temporal unit is now byte-for-byte the encoder's shape.
 *
 * Single tile only: with one tile a tile_group has no tg_start/tg_end and no
 * per-tile size fields, so the OBU payload is exactly
 *      frame_header (byte aligned) || tile bytes
 * A multi-tile frame keeps the two-OBU form (av1_tile_group_obu). */
static inline unsigned av1_frame_obu(const Av1SeqParams *seq,
                                     const Av1FrameParams *f,
                                     const Av1Dpb *dpb,
                                     const Av1Tile *tiles, unsigned ntiles,
                                     const uint8_t *data, unsigned data_size,
                                     unsigned refresh_frame_flags,
                                     uint8_t *out, unsigned cap) {
    uint8_t fh[4096];
    unsigned fhn, total, leb, at, i;

    if (!out || !tiles || ntiles != 1u || !data || cap < 16u)
        return 0;
    if (tiles[0].size == 0 ||
        (uint64_t)tiles[0].offset + tiles[0].size > (uint64_t)data_size)
        return 0;
    /* Type 6: byte_alignment(), not standalone OBU trailing_bits(). */
    fhn = av1_frame_header_payload(seq, f, dpb, tiles, ntiles,
                                   refresh_frame_flags, 0u, fh, sizeof(fh));
    if (fhn == 0)
        return 0;
    total = fhn + tiles[0].size;
    at = 1u;
    leb = av1_leb128(out + at, cap - at, total);
    if (leb == 0 || at + leb + total > cap)
        return 0;
    out[0] = (uint8_t)((AV1_OBU_FRAME << 3) | 0x02u);
    at += leb;
    for (i = 0; i < fhn; i++)
        out[at + i] = fh[i];
    memcpy(out + at + fhn, data + tiles[0].offset, tiles[0].size);
    return at + total;
}

/* ------------------------------------------------------------------------- */
/* tile_group OBU (5.11.1)                                                    */
/* ------------------------------------------------------------------------- */

/* Build the tile_group OBU.  `tiles` are the client's DXVA_Tile_AV1 records
 * resolved against `data` (the BITSTREAM buffer).  With one tile the payload is
 * just the tile bytes; with several it is the start/end flag byte, then each
 * non-last tile's size-minus-1 in tile_size_bytes little-endian bytes followed
 * by the tile. */
static inline unsigned av1_tile_group_obu(const Av1SeqParams *seq,
                                          const Av1FrameParams *f,
                                          const Av1Tile *tiles, unsigned ntiles,
                                          const uint8_t *data,
                                          unsigned data_size,
                                          uint8_t *out, unsigned cap) {
    uint8_t prefix[8];
    unsigned payload_size = 0, at, i;
    unsigned bytes = f->tile_size_bytes ? f->tile_size_bytes
                                        : av1_tile_size_bytes(tiles, ntiles);
    unsigned cols = av1_max_u(1u, f->tile_cols);
    unsigned order[AV1_MAX_TILES];
    unsigned n = 0;
    unsigned total = 0;
    uint8_t *payload;

    (void)seq;
    if (!tiles || ntiles == 0 || !data || data_size == 0 ||
        ntiles > AV1_MAX_TILES || cap < 16u)
        return 0;
    /* AV1FIX (2026-10-25): one record per tile of the grid the frame header
     * announces.  A short list would produce a tile group with fewer tiles than
     * NumTiles -- the decoder then reads tile-size fields out of tile payload
     * bytes.  Refusing here is the same rule the VP9FIX slice records follow. */
    if (f->tile_cols && f->tile_rows && ntiles != f->tile_cols * f->tile_rows)
        return 0;

    /* Order by TileNum = tile_row * TileCols + tile_column (5.11.1). */
    for (i = 0; i < ntiles; i++) {
        unsigned key = (unsigned)tiles[i].row * cols + tiles[i].column;
        unsigned pos = n;
        while (pos > 0) {
            unsigned pk = (unsigned)tiles[order[pos - 1]].row * cols +
                          tiles[order[pos - 1]].column;
            if (pk <= key)
                break;
            order[pos] = order[pos - 1];
            pos--;
        }
        order[pos] = i;
        n++;
    }
    for (i = 0; i < n; i++) {
        unsigned t = order[i];
        /* AV1FIX: an out-of-bounds or empty record is a broken client buffer, not
         * a tile to skip -- skipping used to shorten the payload silently. */
        if (tiles[t].size == 0 ||
            (uint64_t)tiles[t].offset + tiles[t].size > data_size)
            return 0;
        total += tiles[t].size;
    }
    if (total == 0)
        return 0;

    payload = out + sizeof(prefix);
    if (cap < sizeof(prefix) + total)
        return 0;

    if (n == 1) {
        unsigned t = order[0];
        memcpy(payload, data + tiles[t].offset, tiles[t].size);
        payload_size = tiles[t].size;
    } else {
        at = 0;
        payload[at++] = 0; /* tile_start_and_end_present_flag = 0 + alignment */
        for (i = 0; i < n; i++) {
            unsigned t = order[i];
            unsigned last = (i + 1u == n);
            unsigned size = tiles[t].size;
            if (size == 0 ||
                (uint64_t)tiles[t].offset + tiles[t].size > data_size)
                return 0;
            if (!last) {
                uint32_t sm1 = size - 1u;
                unsigned b;
                for (b = 0; b < bytes; b++)
                    payload[at++] = (uint8_t)((sm1 >> (8u * b)) & 0xFFu);
            }
            memcpy(payload + at, data + tiles[t].offset, size);
            at += size;
        }
        payload_size = at;
    }
    if (payload_size == 0)
        return 0;

    prefix[0] = (uint8_t)((AV1_OBU_TILE_GROUP << 3) | 0x02u);
    {
        unsigned leb = av1_leb128(prefix + 1, sizeof(prefix) - 1u, payload_size);
        unsigned hsize;
        if (leb == 0 || 1u + leb > sizeof(prefix))
            return 0;
        hsize = 1u + leb;
        memmove(out + hsize, payload, payload_size);
        memcpy(out, prefix, hsize);
        return hsize + payload_size;
    }
}

/* ------------------------------------------------------------------------- */
/* show_existing_frame temporal unit (5.9.2 / 5.10)                           */
/* ------------------------------------------------------------------------- */

/* A D3D11 AV1 client never asks for a show_existing_frame picture: DXVA has no
 * way to express one (DXVA_PicParams_AV1 has no show_existing field), so the
 * client displays the reference itself and only sends the coded pictures.  The
 * HOST, however, is a stateful V4L2 decoder that only emits a CAPTURE buffer for
 * a picture the bitstream tells it to display.  The source stream of t6_av1.mkv
 * is the proof of what is missing (ffmpeg trace_headers, first 24 TUs):
 *
 *   TU1  key      show_frame=1  order_hint=0   refresh=0xFF (inferred)
 *   TU2  inter    show_frame=0  order_hint=16  refresh=0x01
 *   TU3  inter    show_frame=0  order_hint=8   refresh=0x08
 *   TU4  inter    show_frame=0  order_hint=4   refresh=0x20
 *   TU5  inter    show_frame=0  order_hint=2   refresh=0x40
 *   TU6  inter    show_frame=1  order_hint=1   refresh=0x00
 *   TU7  show_existing_frame  frame_to_show_map_idx=6      <-- we never send this
 *   TU8  inter    show_frame=1  order_hint=3   refresh=0x00
 *   TU9  show_existing_frame  frame_to_show_map_idx=5
 *   ...
 *
 * 445 of the stream's 896 temporal units are show_existing_frame (49.7%), and
 * every displayed alt-ref goes through one.  Without them the host decodes the
 * hidden pictures and never displays them (measured: "Skipping empty output
 * buffer" == hidden sends, and the shown pictures stay inside the decoder's
 * output delay), which is why AV1 showed 0-1 frames while H.264/VP9 showed
 * hundreds.
 *
 * The bytes are exactly what silime/libva-v4l2 src/av1.cpp:45-48 emits
 * (`av1_show_existing`):
 *
 *   0x12 0x00            OBU_TEMPORAL_DELIMITER, has_size=1, size=0
 *   0x1a 0x01            OBU_FRAME_HEADER,     has_size=1, size=1
 *   0x80 | (slot << 4)   uncompressed_header(): show_existing_frame f(1)=1,
 *                        frame_to_show_map_idx f(3)=slot, then RETURN
 *
 * uncompressed_header() returns immediately after those four bits, so the rest
 * of the byte is zero padding: no frame_type, no tile info, no tile group.  The
 * TU carries no compressed data at all -- the host copies the picture it already
 * decoded into that DPB slot and emits it as a CAPTURE buffer.
 *
 * `slot` is a DPB slot that HOLDS the picture: av1_derive_refresh_flags() below
 * recovers exactly that set from the next picture's RefFrameMapTextureIndex (a
 * texture is only in that map because it was written there), so the caller picks
 * one set bit of the derived refresh mask.  The reference implementation makes
 * the same choice (`params.ref_frame_map[slot] == hidden surface`).
 *
 * Preconditions the caller owns: the referenced slot must have been refreshed by
 * the picture just decoded (so the host's DPB holds it), the referenced picture
 * must have been coded with showable_frame=1 (the hidden catch-up is), and the
 * sequence must have film_grain_params_present=0 -- with grain, a show_existing
 * frame header loads the grain parameters of the reference, which is a
 * table lookup and codes no bits, so the 4 bits above are still complete; the
 * caller refuses when our sequence header says grain is on because the reference
 * picture's grain state would then have to match. */
static inline unsigned av1_show_existing_tu(unsigned slot, uint8_t *out,
                                            unsigned cap) {
    if (!out || cap < 5u || slot >= AV1_NUM_REF_FRAMES)
        return 0;
    out[0] = 0x12; /* OBU_TEMPORAL_DELIMITER, size 0 */
    out[1] = 0x00;
    out[2] = (uint8_t)((AV1_OBU_FRAME_HEADER << 3) | 0x02u); /* 0x1a */
    out[3] = 0x01;                                          /* payload size 1 */
    out[4] = (uint8_t)(0x80u | ((slot & 7u) << 4));
    return 5u;
}

/* ------------------------------------------------------------------------- */
/* DPB mirror and refresh mask derivation                                     */
/* ------------------------------------------------------------------------- */

static inline void av1_dpb_reset(Av1Dpb *dpb) {
    memset(dpb, 0, sizeof(*dpb));
}

/* decode_frame_wrapup (7.20): every refreshed slot stores the picture's order
 * hint, its texture index and its saved global-motion matrix; a shown key frame
 * resets the DPB first.
 *
 * AV1 STATEFUL PORT (2026-10-27): the texture index is what makes the mirror
 * usable at all -- derive_refresh_flags() and choose_refresh_flags() are both
 * expressed in terms of WHICH PICTURE a slot holds, and the reference remap
 * (av1_frame_prepare) resolves a client-provided slot through it.  The reference
 * stores exactly the same three things per slot (surface, order_hint,
 * gm_params). */
static inline void av1_dpb_update(Av1Dpb *dpb, const Av1FrameParams *f,
                                  unsigned refresh_frame_flags) {
    unsigned i, r, j;

    if (f->frame_type == AV1_FRAME_KEY && f->show_frame)
        av1_dpb_reset(dpb);
    for (i = 0; i < AV1_NUM_REF_FRAMES; i++) {
        if (refresh_frame_flags & (1u << i)) {
            dpb->valid[i] = 1;
            dpb->order_hint[i] = f->order_hint;
            dpb->texture[i] = f->texture & 0xFFu;
            for (r = 0; r < AV1_NUM_REF_FRAMES; r++)
                for (j = 0; j < 6; j++)
                    dpb->gm[i][r][j] = (r < AV1_REFS_PER_FRAME)
                                           ? f->wm_params[r][j]
                                           : ((j % 3u) == 2u
                                                  ? (1 << AV1_WARPEDMODEL_PREC_BITS)
                                                  : 0);
        }
    }
}

/* The exact refresh mask of frame N, from frame N+1's RefFrameMapTextureIndex
 * (the DPB *before* N+1 updates it): a texture only lands in the map by being
 * written there, so every slot that holds N's texture is one N refreshed. */
static inline unsigned av1_derive_refresh_flags(unsigned curr_pic_texture,
                                                const uint8_t next_map[AV1_NUM_REF_FRAMES]) {
    unsigned mask = 0, i;

    for (i = 0; i < AV1_NUM_REF_FRAMES; i++) {
        if (next_map[i] != 0xFFu && next_map[i] == (curr_pic_texture & 0xFFu))
            mask |= 1u << i;
    }
    return mask;
}

/* choose_refresh_flags(): the BEST-EFFORT mask for a picture that has no
 * successor to derive the exact one from (libva-v4l2
 * Av1AccessUnitBuilder::choose_refresh_flags()).
 *
 * VA/DXVA never carries refresh_frame_flags, so a frame with no successor has
 * no exact answer.  The invariant that keeps every future reference resolvable
 * is "our DPB must still contain every texture the client's map lists", so this
 * refreshes every slot that is EMPTY or holds a texture the map no longer
 * lists; if all eight are live and distinct it evicts the oldest by order hint.
 * Never used while a successor exists. */
static inline unsigned av1_choose_refresh_flags(const Av1Dpb *dpb,
                                                const Av1FrameParams *f) {
    unsigned mask = 0, j, k, oldest;

    for (j = 0; j < AV1_NUM_REF_FRAMES; j++) {
        int in_map = 0;

        for (k = 0; k < AV1_NUM_REF_FRAMES; k++) {
            if (f->ref_texture[k] != 0xFFu &&
                (f->ref_texture[k] & 0xFFu) == (dpb->texture[j] & 0xFFu) &&
                dpb->valid[j])
                in_map = 1;
        }
        if (!dpb->valid[j] || !in_map)
            mask |= 1u << j;
    }
    if (mask != 0)
        return mask;
    oldest = 0;
    for (j = 1; j < AV1_NUM_REF_FRAMES; j++) {
        if ((int)dpb->order_hint[j] < (int)dpb->order_hint[oldest])
            oldest = j;
    }
    return 1u << oldest;
}

/* Resolve a client-provided reference slot to the slot OUR DPB refreshed it in.
 * DXVA names each reference by `ref_frame_idx[i]` = an index into the CLIENT's
 * DPB view; the host's DPB is the one our own refresh masks built, so the two
 * numbering only agree while every mask we sent was the exact derived one.  The
 * reference does this remap (av1_bitstream.cc build_frame_header()) and so do we:
 * look up the texture the client means, then find the slot that holds it. */
static inline void av1_frame_prepare(const Av1Dpb *dpb, Av1FrameParams *f) {
    unsigned i, j;

    if (f->frame_type == AV1_FRAME_KEY || f->frame_type == AV1_FRAME_INTRA_ONLY)
        return;
    for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
        unsigned slot = f->ref_frame_idx[i] & 7u;
        unsigned want = f->ref_texture[slot] & 0xFFu;

        if (want == 0xFFu)
            continue;
        for (j = 0; j < AV1_NUM_REF_FRAMES; j++) {
            if (dpb->valid[j] && (dpb->texture[j] & 0xFFu) == want) {
                f->ref_frame_idx[i] = j;
                break;
            }
        }
    }
}

#endif /* VIRTIO_WDDM_AV1_PARAMS_H */
