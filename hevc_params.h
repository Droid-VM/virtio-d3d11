/* HEVC Annex-B VPS/SPS/PPS synthesis from DXVA_PicParams_HEVC (P-BATCH HEVC).
 *
 * Why this file exists
 * --------------------
 * Same reason h264_params.h exists: a D3D11 client does not put parameter sets
 * into the BITSTREAM decoder buffer.  Chromium's D3D11 HEVC accelerator
 * (media/gpu/windows/d3d11_h265_accelerator.cc) submits slice NALs only and
 * keeps the parsed SPS/PPS values in a DXVA_PicParams_HEVC
 * (PICTURE_PARAMETERS).  The host virtio-media decoder is a V4L2 *stateful*
 * decoder: it is handed exactly the bytes of the BITSTREAM buffer, so without
 * in-band VPS/SPS/PPS it cannot decode anything (the H.264 path hit the same
 * wall in P2-2h: DQBUF bytesused=0 -> eventq timeout -> STATUS_IO_TIMEOUT).
 *
 * DXVA_PicParams_HEVC is a transcription of the SPS and PPS that a hardware
 * decoder needs (chroma format, bit depths, coding-block geometry, tool flags,
 * the whole PPS deblock/quantisation/tile set, the current picture's reference
 * list), which is exactly what makes the reverse mapping below possible:
 *
 *   VPS <- vps_id 0, one layer, profile_tier_level (Main or Main10, from the
 *          sample depth), the same max_dec_pic_buffering as the SPS
 *   SPS <- PicWidthInMinCbsY/PicHeightInMinCbsY (coded size, conformance window
 *          cropped to the surface the runtime asked for), chroma_format_idc,
 *          bit depths, log2_max_pic_order_cnt_lsb_minus4, coding/transform block
 *          geometry, hierarchy depths, SAO/AMP/PCM/scaling-list/temporal-MVP/
 *          strong-intra-smoothing flags, sps_max_dec_pic_buffering_minus1
 *   PPS <- pps_id (from the slice header), dependent_slice_segments,
 *          output_flag_present, num_extra_slice_header_bits, sign-data-hiding,
 *          cabac_init_present, default ref counts, init_qp_minus26, constrained
 *          intra pred, transform skip, cu_qp_delta (+depth), cb/cr qp offsets,
 *          weighted pred/bipred, transquant bypass, tiles (+ explicit column /
 *          row widths), entropy coding sync, loop filter across slices,
 *          deblocking override/disabled + beta/tc offsets, lists modification,
 *          log2_parallel_merge_level_minus2, slice header extension
 *
 * What the host decodes is therefore bit-identical to what the encoder wrote,
 * as long as the slice header syntax only depends on values DXVA carries.  That
 * is true for the streams this UMD reports (Main = 8-bit 4:2:0 and Main10 =
 * 10-bit 4:2:0, no long-term reference pictures, no SPS-level short-term RPS),
 * and the two cases where it is *not* true are refused instead of guessed:
 *
 *   - num_short_term_ref_pic_sets != 0: the reference picture sets live in the
 *     SPS and the slice header carries only an index into them.  DXVA carries
 *     the current picture's explicit reference list, not the SPS's set table,
 *     so the indexes cannot be resolved.  Synthesis is refused and the caller
 *     falls back to the in-band parameter sets (the ffmpeg/PotPlayer style
 *     client sends them, so that path still decodes).
 *   - num_long_term_ref_pics_sps != 0 / long_term_ref_pics_present_flag: the
 *     SPS lists POC LSBs of the long-term sets, and the slice header spells each
 *     long-term picture out; DXVA does not carry the SPS table.
 *
 * Everything else is a faithful copy of the values, including the odd ones
 * (tile column widths, PCM block sizes, scaling-list presence), so the decoder
 * parses the same number of bits the encoder wrote.
 *
 * Deliberate limitations (logged, not papered over):
 *   - VUI is not emitted (vui_parameters_present_flag = 0): no aspect ratio,
 *     timing or colour description.  The host gets timing from virtio-media and
 *     the surface format from S_FMT.
 *   - sps_max_num_reorder_pics / sps_max_latency_increase_plus1 are 0, so the
 *     decoder emits every picture as soon as it is decodable (decode order).
 *     That keeps the 1 access unit -> 1 vm_get_frame() contract of the D3D11
 *     path (the H.264 synthesised SPS has no VUI either, i.e. reorder 0).
 *   - scaling lists: presence is copied, the lists themselves are the flat
 *     defaults (prediction from matrix id 0), because DXVA_PicParams_HEVC does
 *     not carry them.  A stream with custom lists decodes with default weights.
 *   - profile follows the sample depth (HEVC-MAIN10): general_profile_idc 1 /
 *     compatibility flag 1 for Main (bit_depth_luma_minus8 = 0), idc 2 / flag 2
 *     for Main10 (bit_depth_luma_minus8 = 2).  Nothing else in the PTL changes;
 *     the level is derived from the coded size (a level at or above the stream's
 *     own one is legal).  Main12 and the 4:2:2/4:4:4 profiles are refused.
 *
 * Plain C99 (stdint only) so the same code compiles into the UMD (ARM64 PE) and
 * into a Linux test harness that compares the synthesised parameter sets against
 * a real stream with ffmpeg as the oracle.
 */
#ifndef VIRTIO_WDDM_HEVC_PARAMS_H
#define VIRTIO_WDDM_HEVC_PARAMS_H

#include <stdint.h>
#include <string.h> /* memcpy / memset: the header carries its own implementation */

#ifdef __cplusplus
extern "C" {
#endif

/* NAL unit types (HEVC 7.4.2.2, Table 7-1). */
#define HEVC_NAL_TRAIL_N 0u
#define HEVC_NAL_TRAIL_R 1u
#define HEVC_NAL_TSA_N 2u
#define HEVC_NAL_TSA_R 3u
#define HEVC_NAL_STSA_N 4u
#define HEVC_NAL_STSA_R 5u
#define HEVC_NAL_RADL_N 6u
#define HEVC_NAL_RADL_R 7u
#define HEVC_NAL_RASL_N 8u
#define HEVC_NAL_RASL_R 9u
#define HEVC_NAL_BLA_W_LP 16u
#define HEVC_NAL_BLA_W_RADL 17u
#define HEVC_NAL_BLA_N_LP 18u
#define HEVC_NAL_IDR_W_RADL 19u
#define HEVC_NAL_IDR_N_LP 20u
#define HEVC_NAL_CRA 21u
#define HEVC_NAL_VPS 32u
#define HEVC_NAL_SPS 33u
#define HEVC_NAL_PPS 34u
#define HEVC_NAL_AUD 35u
#define HEVC_NAL_EOS 36u
#define HEVC_NAL_EOB 37u
#define HEVC_NAL_FD 38u
#define HEVC_NAL_SEI_PREFIX 39u
#define HEVC_NAL_SEI_SUFFIX 40u

/* Biggest VPS+SPS+PPS this writer can emit (Annex-B, 4-byte start codes each).
 * Real ones are ~120 bytes together; the bound only keeps a caller's buffer
 * large enough for the pathological case (tiles + PCM + scaling lists). */
#define HEVC_PARAMS_MAX_BYTES 1024u

/* Field set the writer consumes.  Units are the ones the SPS/PPS syntax wants,
 * i.e. "minus1"/"minus4" values stay as they are (they are stored that way in
 * DXVA_PicParams_HEVC too). */
typedef struct {
    /* --- identifiers ---------------------------------------------------- */
    unsigned vps_id; /* vps_video_parameter_set_id (0) */
    unsigned sps_id; /* sps_seq_parameter_set_id (0) */
    unsigned pps_id; /* pps_pic_parameter_set_id (from the slice header) */

    /* --- picture geometry / format ------------------------------------- */
    unsigned pic_width_in_min_cbs;  /* PicWidthInMinCbsY  */
    unsigned pic_height_in_min_cbs; /* PicHeightInMinCbsY */
    unsigned chroma_format_idc;     /* 1 = 4:2:0 */
    unsigned separate_colour_plane_flag;
    unsigned bit_depth_luma_minus8;
    unsigned bit_depth_chroma_minus8;

    /* --- SPS coding geometry ------------------------------------------- */
    unsigned log2_max_pic_order_cnt_lsb_minus4;
    unsigned sps_max_dec_pic_buffering_minus1;
    unsigned log2_min_luma_coding_block_size_minus3;
    unsigned log2_diff_max_min_luma_coding_block_size;
    unsigned log2_min_luma_transform_block_size_minus2;
    unsigned log2_diff_max_min_luma_transform_block_size;
    unsigned max_transform_hierarchy_depth_inter;
    unsigned max_transform_hierarchy_depth_intra;
    unsigned num_short_term_ref_pic_sets;
    unsigned num_long_term_ref_pics_sps;
    unsigned long_term_ref_pics_present_flag;

    /* --- SPS tool flags ------------------------------------------------- */
    unsigned scaling_list_enabled_flag;
    unsigned amp_enabled_flag;
    unsigned sample_adaptive_offset_enabled_flag;
    unsigned pcm_enabled_flag;
    unsigned pcm_sample_bit_depth_luma_minus1;
    unsigned pcm_sample_bit_depth_chroma_minus1;
    unsigned log2_min_pcm_luma_coding_block_size_minus3;
    unsigned log2_diff_max_min_pcm_luma_coding_block_size;
    unsigned pcm_loop_filter_disabled_flag;
    unsigned sps_temporal_mvp_enabled_flag;
    unsigned strong_intra_smoothing_enabled_flag;

    /* --- PPS ----------------------------------------------------------- */
    unsigned dependent_slice_segments_enabled_flag;
    unsigned output_flag_present_flag;
    unsigned num_extra_slice_header_bits;
    unsigned sign_data_hiding_enabled_flag;
    unsigned cabac_init_present_flag;
    unsigned num_ref_idx_l0_default_active_minus1;
    unsigned num_ref_idx_l1_default_active_minus1;
    int init_qp_minus26;
    unsigned constrained_intra_pred_flag;
    unsigned transform_skip_enabled_flag;
    unsigned cu_qp_delta_enabled_flag;
    unsigned diff_cu_qp_delta_depth;
    int pps_cb_qp_offset;
    int pps_cr_qp_offset;
    unsigned pps_slice_chroma_qp_offsets_present_flag;
    unsigned weighted_pred_flag;
    unsigned weighted_bipred_flag;
    unsigned transquant_bypass_enabled_flag;
    unsigned tiles_enabled_flag;
    unsigned entropy_coding_sync_enabled_flag;
    unsigned uniform_spacing_flag;
    unsigned loop_filter_across_tiles_enabled_flag;
    unsigned pps_loop_filter_across_slices_enabled_flag;
    unsigned deblocking_filter_override_enabled_flag;
    unsigned pps_deblocking_filter_disabled_flag;
    int pps_beta_offset_div2;
    int pps_tc_offset_div2;
    unsigned lists_modification_present_flag;
    unsigned log2_parallel_merge_level_minus2;
    unsigned slice_segment_header_extension_present_flag;
    unsigned num_tile_columns_minus1; /* <= 19 (DXVA table size) */
    unsigned num_tile_rows_minus1;    /* <= 21 (DXVA table size) */
    unsigned column_width_minus1[19];
    unsigned row_height_minus1[21];

    /* --- presentation -------------------------------------------------- */
    /* Visible size of the surface the runtime asked us to decode into.  When it
     * is smaller than the coded (min-CB aligned) size, a conformance window
     * crops the output to it, so the host frame is exactly as wide/tall as the
     * NV12 texture.  0/0 means "no crop". */
    unsigned visible_width;
    unsigned visible_height;
    /* general_level_idc; 0 = derive from the coded size. */
    unsigned level_idc;
} HevcParamSetFields;

/* ------------------------------------------------------------------------- */
/* Bit writer                                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *out;
    unsigned cap;
    unsigned size;
    uint32_t acc;
    unsigned nbits;
    int overflow;
} HevcBitWriter;

static void hevc_bw_put(HevcBitWriter *w, uint32_t value, unsigned bits) {
    unsigned i;
    for (i = 0; i < bits; i++) {
        w->acc = (w->acc << 1) | ((value >> (bits - 1u - i)) & 1u);
        if (++w->nbits == 8u) {
            if (w->size < w->cap)
                w->out[w->size] = (uint8_t)w->acc;
            else
                w->overflow = 1;
            w->size++;
            w->acc = 0;
            w->nbits = 0;
        }
    }
}

/* ue(v) */
static void hevc_bw_ue(HevcBitWriter *w, unsigned value) {
    uint32_t code = (uint32_t)value + 1u;
    unsigned n = 0;
    uint32_t scan = code;

    while (scan > 1u) {
        scan >>= 1;
        n++;
    }
    if (n)
        hevc_bw_put(w, 0, n);
    hevc_bw_put(w, code, n + 1u);
}

/* se(v) */
static void hevc_bw_se(HevcBitWriter *w, int value) {
    unsigned code = value > 0 ? (unsigned)(2 * value - 1) : (unsigned)(-2 * value);
    hevc_bw_ue(w, code);
}

static void hevc_bw_trailing(HevcBitWriter *w) {
    hevc_bw_put(w, 1u, 1u); /* rbsp_stop_one_bit */
    while (w->nbits != 0)
        hevc_bw_put(w, 0u, 1u);
}

/* Copy an RBSP into an EBSP: after two consecutive zero bytes, any byte <= 3 is
 * preceded by 0x03 (HEVC 7.4.2). Returns bytes written, 0 on overflow. */
static unsigned hevc_escape_rbsp(const uint8_t *rbsp, unsigned size, uint8_t *out,
                                 unsigned cap) {
    unsigned i;
    unsigned w = 0;
    unsigned zeros = 0;

    for (i = 0; i < size; i++) {
        uint8_t b = rbsp[i];
        if (zeros >= 2u && b <= 3u) {
            if (w >= cap)
                return 0;
            out[w++] = 0x03;
            zeros = 0;
        }
        if (w >= cap)
            return 0;
        out[w++] = b;
        if (b == 0)
            zeros++;
        else
            zeros = 0;
    }
    return w;
}

/* Annex-B NAL: 4-byte start code + 2-byte NAL header + escaped RBSP.
 * nuh_layer_id = 0, nuh_temporal_id_plus1 = 1 (temporal id 0). */
static unsigned hevc_emit_nal(unsigned nal_type, const uint8_t *rbsp,
                              unsigned rbsp_size, uint8_t *out, unsigned cap) {
    unsigned escaped;

    if (cap < 4u + 2u)
        return 0;
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 1;
    out[4] = (uint8_t)((nal_type & 0x3fu) << 1); /* forbidden_zero_bit 0, type */
    out[5] = 0x01u;                               /* layer 0, temporal id 0 */
    escaped = hevc_escape_rbsp(rbsp, rbsp_size, out + 6, cap - 6u);
    if (escaped == 0)
        return 0;
    return 4u + 2u + escaped;
}

/* Minimum standard level that covers a picture of w*h luma samples.
 * General_level_idc = 30 * level, so the ladder below is
 * 1 / 2 / 2.1 / 3 / 3.1 / 4 / 5 / 5.1 and it matches what x265 picks for the
 * same sizes (640x360 -> 63, 1280x720 -> 93, 1080p -> 120, 4K -> 150). */
static unsigned hevc_level_from_pixels(unsigned width, unsigned height) {
    unsigned long long ps = (unsigned long long)width * height;

    if (ps <= 36864ull)
        return 30u;
    if (ps <= 122880ull)
        return 60u;
    if (ps <= 245760ull)
        return 63u;
    if (ps <= 552960ull)
        return 90u;
    if (ps <= 983040ull)
        return 93u;
    if (ps <= 2228224ull)
        return 120u;
    if (ps <= 8912896ull)
        return 150u;
    return 156u;
}

/* ------------------------------------------------------------------------- */
/* profile_tier_level                                                        */
/* ------------------------------------------------------------------------- */

/* profile_tier_level( profilePresentFlag = 1, maxNumSubLayersMinus1 = 0 ).
 *
 * The profile follows the sample depth: general_profile_idc 1 / compatibility
 * flag 1 for Main (8-bit), idc 2 / flag 2 for Main10 (10-bit).  Everything else
 * is the same in both: Main tier, progressive / non-packed / frame-only, the
 * 44 reserved zero bits (`general_reserved_zero_44bits` -- the syntax only
 * switches to the long constraint-flag list for profiles 4..11) and
 * general_inbld_flag 0 (Main and Main10 both use the inbld bit). */
static void hevc_write_profile_tier_level(HevcBitWriter *w, unsigned level_idc,
                                          unsigned bit_depth_minus8) {
    unsigned i;
    unsigned profile_idc = bit_depth_minus8 == 0u ? 1u : 2u;

    hevc_bw_put(w, 0, 2); /* general_profile_space */
    hevc_bw_put(w, 0, 1); /* general_tier_flag: Main tier */
    hevc_bw_put(w, profile_idc, 5); /* general_profile_idc: Main / Main10 */
    for (i = 0; i < 32; i++)
        hevc_bw_put(w, i == profile_idc ? 1u : 0u,
                    1); /* general_profile_compatibility_flag */
    hevc_bw_put(w, 1, 1);                        /* general_progressive_source_flag */
    hevc_bw_put(w, 0, 1);                        /* general_interlaced_source_flag */
    hevc_bw_put(w, 1, 1);                        /* general_non_packed_constraint_flag */
    hevc_bw_put(w, 1, 1);                        /* general_frame_only_constraint_flag */
    for (i = 0; i < 43u; i++)
        hevc_bw_put(w, 0, 1); /* general_reserved_zero_43bits */
    hevc_bw_put(w, 0, 1);     /* general_inbld_flag */
    hevc_bw_put(w, level_idc & 0xffu, 8);
}

/* Default (flat) scaling lists: every matrix predicted from its own default.
 * DXVA_PicParams_HEVC carries only scaling_list_enabled_flag, not the lists. */
static void hevc_write_default_scaling_list_data(HevcBitWriter *w) {
    unsigned size_id;
    unsigned matrix_id;

    for (size_id = 0; size_id < 4u; size_id++) {
        unsigned matrices = size_id == 3u ? 2u : 6u;
        for (matrix_id = 0; matrix_id < matrices; matrix_id++) {
            hevc_bw_put(w, 0, 1);  /* scaling_list_pred_mode_flag */
            hevc_bw_ue(w, 0);      /* scaling_list_pred_matrix_id_delta */
        }
    }
}

/* ------------------------------------------------------------------------- */
/* VPS                                                                       */
/* ------------------------------------------------------------------------- */

static unsigned hevc_write_vps(const HevcParamSetFields *f, uint8_t *out,
                               unsigned cap) {
    uint8_t rbsp[128];
    HevcBitWriter w;
    unsigned level_idc = f->level_idc;

    w.out = rbsp;
    w.cap = (unsigned)sizeof(rbsp);
    w.size = 0;
    w.acc = 0;
    w.nbits = 0;
    w.overflow = 0;

    hevc_bw_put(&w, f->vps_id & 0xfu, 4); /* vps_video_parameter_set_id */
    hevc_bw_put(&w, 1, 1);                 /* vps_base_layer_internal_flag */
    hevc_bw_put(&w, 1, 1);                 /* vps_base_layer_available_flag */
    hevc_bw_put(&w, 0, 6);                 /* vps_max_layers_minus1 */
    hevc_bw_put(&w, 0, 3);                 /* vps_max_sub_layers_minus1 */
    hevc_bw_put(&w, 1, 1);                 /* vps_temporal_id_nesting_flag */
    /* vps_max_sub_layers_minus1 == 0 -> vps_reserved_0xffff_16bits (7.3.2.1).
     * x265 emits exactly 0xffff here (VPS starts 40 01 0c 01 ff ff ...). */
    hevc_bw_put(&w, 0xffffu, 16);
    hevc_write_profile_tier_level(&w, level_idc, f->bit_depth_luma_minus8);

    hevc_bw_put(&w, 0, 1); /* vps_sub_layer_ordering_info_present_flag */
    hevc_bw_ue(&w, f->sps_max_dec_pic_buffering_minus1);
    hevc_bw_ue(&w, 0); /* vps_max_num_reorder_pics */
    hevc_bw_ue(&w, 0); /* vps_max_latency_increase_plus1 */
    hevc_bw_put(&w, 0, 6); /* vps_max_layer_id */
    hevc_bw_ue(&w, 0);     /* vps_num_layer_sets_minus1 */
    hevc_bw_put(&w, 0, 1); /* vps_timing_info_present_flag */
    hevc_bw_put(&w, 0, 1); /* vps_extension_flag */

    hevc_bw_trailing(&w);
    if (w.overflow || w.nbits != 0)
        return 0;
    return hevc_emit_nal(HEVC_NAL_VPS, rbsp, w.size, out, cap);
}

/* ------------------------------------------------------------------------- */
/* SPS                                                                       */
/* ------------------------------------------------------------------------- */

/* Coded (min-CB aligned) size implied by PicWidthInMinCbsY. */
static unsigned hevc_coded_width(const HevcParamSetFields *f) {
    unsigned min_cb = 1u << (f->log2_min_luma_coding_block_size_minus3 + 3u);
    return f->pic_width_in_min_cbs * min_cb;
}

static unsigned hevc_coded_height(const HevcParamSetFields *f) {
    unsigned min_cb = 1u << (f->log2_min_luma_coding_block_size_minus3 + 3u);
    return f->pic_height_in_min_cbs * min_cb;
}

static unsigned hevc_write_sps(const HevcParamSetFields *f, uint8_t *out,
                               unsigned cap) {
    uint8_t rbsp[256];
    HevcBitWriter w;
    unsigned width = hevc_coded_width(f);
    unsigned height = hevc_coded_height(f);
    unsigned sub_w = (f->chroma_format_idc == 1u || f->chroma_format_idc == 2u) ? 2u : 1u;
    unsigned sub_h = (f->chroma_format_idc == 1u) ? 2u : 1u;
    unsigned crop_flag = 0;
    unsigned crop_right = 0;
    unsigned crop_bottom = 0;

    if (width == 0 || height == 0)
        return 0;

    /* Conformance window: only the surface the runtime gave us is known, so the
     * crop is taken from the bottom/right edges (what a 1920x1080 stream coded
     * as 1920x1088 uses).  Unrepresentable remainders (odd, or larger than the
     * min-CB padding) are left uncropped; the caller then sees a host frame
     * that is up to one min-CB row/column bigger. */
    if (f->visible_width && f->visible_height && f->visible_width <= width &&
        f->visible_height <= height) {
        unsigned dx = width - f->visible_width;
        unsigned dy = height - f->visible_height;
        if ((dx % sub_w) == 0u && (dy % sub_h) == 0u &&
            dx / sub_w <= 8u && dy / sub_h <= 8u) {
            crop_right = dx / sub_w;
            crop_bottom = dy / sub_h;
            crop_flag = (crop_right || crop_bottom) ? 1u : 0u;
        }
    }

    w.out = rbsp;
    w.cap = (unsigned)sizeof(rbsp);
    w.size = 0;
    w.acc = 0;
    w.nbits = 0;
    w.overflow = 0;

    hevc_bw_put(&w, f->vps_id & 0xfu, 4); /* sps_video_parameter_set_id */
    hevc_bw_put(&w, 0, 3);                /* sps_max_sub_layers_minus1 */
    hevc_bw_put(&w, 1, 1);                /* sps_temporal_id_nesting_flag */
    hevc_write_profile_tier_level(&w, f->level_idc, f->bit_depth_luma_minus8);

    hevc_bw_ue(&w, f->sps_id); /* sps_seq_parameter_set_id */
    hevc_bw_ue(&w, f->chroma_format_idc);
    if (f->chroma_format_idc == 3u)
        hevc_bw_put(&w, f->separate_colour_plane_flag, 1);
    hevc_bw_ue(&w, width);
    hevc_bw_ue(&w, height);
    hevc_bw_put(&w, crop_flag, 1);
    if (crop_flag) {
        hevc_bw_ue(&w, 0); /* conf_win_left_offset */
        hevc_bw_ue(&w, crop_right);
        hevc_bw_ue(&w, 0); /* conf_win_top_offset */
        hevc_bw_ue(&w, crop_bottom);
    }
    hevc_bw_ue(&w, f->bit_depth_luma_minus8);
    hevc_bw_ue(&w, f->bit_depth_chroma_minus8);
    hevc_bw_ue(&w, f->log2_max_pic_order_cnt_lsb_minus4);

    hevc_bw_put(&w, 0, 1); /* sps_sub_layer_ordering_info_present_flag */
    hevc_bw_ue(&w, f->sps_max_dec_pic_buffering_minus1);
    hevc_bw_ue(&w, 0); /* sps_max_num_reorder_pics: decode order out */
    hevc_bw_ue(&w, 0); /* sps_max_latency_increase_plus1 */

    hevc_bw_ue(&w, f->log2_min_luma_coding_block_size_minus3);
    hevc_bw_ue(&w, f->log2_diff_max_min_luma_coding_block_size);
    hevc_bw_ue(&w, f->log2_min_luma_transform_block_size_minus2);
    hevc_bw_ue(&w, f->log2_diff_max_min_luma_transform_block_size);
    hevc_bw_ue(&w, f->max_transform_hierarchy_depth_inter);
    hevc_bw_ue(&w, f->max_transform_hierarchy_depth_intra);

    hevc_bw_put(&w, f->scaling_list_enabled_flag ? 1u : 0u, 1);
    if (f->scaling_list_enabled_flag) {
        hevc_bw_put(&w, 1, 1); /* sps_scaling_list_data_present_flag */
        hevc_write_default_scaling_list_data(&w);
    }
    hevc_bw_put(&w, f->amp_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->sample_adaptive_offset_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->pcm_enabled_flag ? 1u : 0u, 1);
    if (f->pcm_enabled_flag) {
        hevc_bw_put(&w, f->pcm_sample_bit_depth_luma_minus1 & 0xfu, 4);
        hevc_bw_put(&w, f->pcm_sample_bit_depth_chroma_minus1 & 0xfu, 4);
        hevc_bw_ue(&w, f->log2_min_pcm_luma_coding_block_size_minus3);
        hevc_bw_ue(&w, f->log2_diff_max_min_pcm_luma_coding_block_size);
        hevc_bw_put(&w, f->pcm_loop_filter_disabled_flag ? 1u : 0u, 1);
    }

    /* Refused by hevc_synth_vps_sps_pps() when non-zero; 0 emits no set. */
    hevc_bw_ue(&w, f->num_short_term_ref_pic_sets);
    hevc_bw_put(&w, f->long_term_ref_pics_present_flag ? 1u : 0u, 1);
    if (f->long_term_ref_pics_present_flag) {
        unsigned i;
        unsigned bits = f->log2_max_pic_order_cnt_lsb_minus4 + 4u;
        hevc_bw_ue(&w, f->num_long_term_ref_pics_sps);
        for (i = 0; i < f->num_long_term_ref_pics_sps; i++) {
            hevc_bw_put(&w, 0, bits); /* lt_ref_pic_poc_lsb_sps */
            hevc_bw_put(&w, 0, 1);    /* used_by_curr_pic_lt_sps_flag */
        }
    }
    hevc_bw_put(&w, f->sps_temporal_mvp_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->strong_intra_smoothing_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, 0, 1); /* vui_parameters_present_flag */
    hevc_bw_put(&w, 0, 1); /* sps_extension_present_flag */

    hevc_bw_trailing(&w);
    if (w.overflow || w.nbits != 0)
        return 0;
    return hevc_emit_nal(HEVC_NAL_SPS, rbsp, w.size, out, cap);
}

/* ------------------------------------------------------------------------- */
/* PPS                                                                       */
/* ------------------------------------------------------------------------- */

static unsigned hevc_write_pps(const HevcParamSetFields *f, uint8_t *out,
                               unsigned cap) {
    uint8_t rbsp[256];
    HevcBitWriter w;
    unsigned i;

    if (f->num_tile_columns_minus1 > 19u || f->num_tile_rows_minus1 > 21u)
        return 0;

    w.out = rbsp;
    w.cap = (unsigned)sizeof(rbsp);
    w.size = 0;
    w.acc = 0;
    w.nbits = 0;
    w.overflow = 0;

    hevc_bw_ue(&w, f->pps_id);
    hevc_bw_ue(&w, f->sps_id);
    hevc_bw_put(&w, f->dependent_slice_segments_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->output_flag_present_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->num_extra_slice_header_bits & 0x7u, 3);
    hevc_bw_put(&w, f->sign_data_hiding_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->cabac_init_present_flag ? 1u : 0u, 1);
    hevc_bw_ue(&w, f->num_ref_idx_l0_default_active_minus1);
    hevc_bw_ue(&w, f->num_ref_idx_l1_default_active_minus1);
    hevc_bw_se(&w, f->init_qp_minus26);
    hevc_bw_put(&w, f->constrained_intra_pred_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->transform_skip_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->cu_qp_delta_enabled_flag ? 1u : 0u, 1);
    if (f->cu_qp_delta_enabled_flag)
        hevc_bw_ue(&w, f->diff_cu_qp_delta_depth);
    hevc_bw_se(&w, f->pps_cb_qp_offset);
    hevc_bw_se(&w, f->pps_cr_qp_offset);
    hevc_bw_put(&w, f->pps_slice_chroma_qp_offsets_present_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->weighted_pred_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->weighted_bipred_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->transquant_bypass_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->tiles_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->entropy_coding_sync_enabled_flag ? 1u : 0u, 1);
    if (f->tiles_enabled_flag) {
        hevc_bw_ue(&w, f->num_tile_columns_minus1);
        hevc_bw_ue(&w, f->num_tile_rows_minus1);
        hevc_bw_put(&w, f->uniform_spacing_flag ? 1u : 0u, 1);
        if (!f->uniform_spacing_flag) {
            for (i = 0; i < f->num_tile_columns_minus1; i++)
                hevc_bw_ue(&w, f->column_width_minus1[i]);
            for (i = 0; i < f->num_tile_rows_minus1; i++)
                hevc_bw_ue(&w, f->row_height_minus1[i]);
        }
        hevc_bw_put(&w, f->loop_filter_across_tiles_enabled_flag ? 1u : 0u, 1);
    }
    hevc_bw_put(&w, f->pps_loop_filter_across_slices_enabled_flag ? 1u : 0u, 1);
    /* deblocking_filter_control_present_flag is mandated to be 1 (7.4.3.3). */
    hevc_bw_put(&w, 1, 1);
    hevc_bw_put(&w, f->deblocking_filter_override_enabled_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, f->pps_deblocking_filter_disabled_flag ? 1u : 0u, 1);
    if (!f->pps_deblocking_filter_disabled_flag) {
        hevc_bw_se(&w, f->pps_beta_offset_div2);
        hevc_bw_se(&w, f->pps_tc_offset_div2);
    }
    hevc_bw_put(&w, 0, 1); /* pps_scaling_list_data_present_flag */
    hevc_bw_put(&w, f->lists_modification_present_flag ? 1u : 0u, 1);
    hevc_bw_ue(&w, f->log2_parallel_merge_level_minus2);
    hevc_bw_put(&w, f->slice_segment_header_extension_present_flag ? 1u : 0u, 1);
    hevc_bw_put(&w, 0, 1); /* pps_extension_present_flag */

    hevc_bw_trailing(&w);
    if (w.overflow || w.nbits != 0)
        return 0;
    return hevc_emit_nal(HEVC_NAL_PPS, rbsp, w.size, out, cap);
}

/* ------------------------------------------------------------------------- */
/* Synthetic parameter sets                                                  */
/* ------------------------------------------------------------------------- */

/* Writes "00 00 00 01 40 01 <VPS> 00 00 00 01 42 01 <SPS> 00 00 00 01 44 01
 * <PPS>" into out.  Returns the byte count (0 = refused / buffer too small).
 * *vps_bytes / *sps_bytes / *pps_bytes receive the per-NAL sizes including the
 * 4-byte start code and the 2-byte NAL header.
 *
 * Refusals are deliberate: anything this writer cannot reproduce bit-exactly is
 * better declined (the caller then uses the in-band parameter sets or drops the
 * frame with a log line) than sent as a header set that makes the host parse the
 * slice data at the wrong bit offsets. */
unsigned hevc_synth_vps_sps_pps(const HevcParamSetFields *fields, uint8_t *out,
                                unsigned out_cap, unsigned *vps_bytes,
                                unsigned *sps_bytes, unsigned *pps_bytes) {
    unsigned vps;
    unsigned sps;
    unsigned pps;
    unsigned total;

    if (vps_bytes)
        *vps_bytes = 0;
    if (sps_bytes)
        *sps_bytes = 0;
    if (pps_bytes)
        *pps_bytes = 0;
    if (!fields || !out || out_cap < 3u * 16u)
        return 0;

    /* The profiles this UMD reports are Main (8-bit 4:2:0) and Main10 (10-bit
     * 4:2:0), one colour plane.  bit_depth_luma_minus8 is written into the SPS
     * as-is and selects the PTL profile idc, so only the two legal values are
     * accepted, and luma/chroma must agree. */
    if (fields->chroma_format_idc != 1u)
        return 0;
    if (fields->separate_colour_plane_flag)
        return 0;
    if (fields->bit_depth_luma_minus8 != fields->bit_depth_chroma_minus8)
        return 0;
    if (fields->bit_depth_luma_minus8 > 2u)
        return 0;

    /* Reference picture sets in the SPS: the slice header only carries an index,
     * and the SPS table is not part of DXVA.  See the header comment. */
    if (fields->num_short_term_ref_pic_sets != 0u)
        return 0;
    if (fields->long_term_ref_pics_present_flag || fields->num_long_term_ref_pics_sps)
        return 0;
    if (fields->sps_temporal_mvp_enabled_flag && fields->log2_max_pic_order_cnt_lsb_minus4 > 12u)
        return 0; /* log2_max_pic_order_cnt_lsb > 16 is not legal */
    if (fields->log2_min_luma_coding_block_size_minus3 > 3u)
        return 0;
    if (fields->log2_diff_max_min_luma_coding_block_size > 6u)
        return 0;
    if (fields->log2_min_luma_transform_block_size_minus2 > 3u)
        return 0;
    if (fields->log2_diff_max_min_luma_transform_block_size > 3u)
        return 0;
    if (fields->num_extra_slice_header_bits > 7u)
        return 0;
    if (fields->pps_id > 63u || fields->sps_id > 15u || fields->vps_id > 15u)
        return 0;

    vps = hevc_write_vps(fields, out, out_cap);
    if (vps == 0)
        return 0;
    sps = hevc_write_sps(fields, out + vps, out_cap - vps);
    if (sps == 0)
        return 0;
    pps = hevc_write_pps(fields, out + vps + sps, out_cap - vps - sps);
    if (pps == 0)
        return 0;
    total = vps + sps + pps;

    if (vps_bytes)
        *vps_bytes = vps;
    if (sps_bytes)
        *sps_bytes = sps;
    if (pps_bytes)
        *pps_bytes = pps;
    return total;
}

/* Fills fields->level_idc from the coded size when the caller left it 0. */
void hevc_fields_finish(HevcParamSetFields *fields) {
    if (!fields)
        return;
    if (fields->level_idc == 0u)
        fields->level_idc =
            hevc_level_from_pixels(hevc_coded_width(fields), hevc_coded_height(fields));
}

/* ------------------------------------------------------------------------- */
/* Bitstream parsing: NAL scan + slice header + parameter set fields         */
/* ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    unsigned size;
    unsigned pos;
    unsigned cur;
    int bits;
    unsigned zeros;
} HevcBitReader;

/* Next RBSP byte of the NAL payload, removing emulation_prevention_three_byte
 * (0x03 after two zero bytes). Returns -1 at the end. */
static int hevc_br_byte(HevcBitReader *r) {
    while (r->pos < r->size) {
        unsigned b = r->p[r->pos++];
        if (r->zeros >= 2u && b == 0x03u) {
            r->zeros = 0;
            continue;
        }
        if (b == 0)
            r->zeros++;
        else
            r->zeros = 0;
        return (int)b;
    }
    return -1;
}

static int hevc_br_bit(HevcBitReader *r) {
    int b;
    if (r->bits == 0) {
        b = hevc_br_byte(r);
        if (b < 0)
            return -1;
        r->cur = (unsigned)b;
        r->bits = 8;
    }
    r->bits--;
    return (int)((r->cur >> r->bits) & 1u);
}

static int hevc_br_u(HevcBitReader *r, unsigned count, unsigned *value) {
    unsigned v = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        int b = hevc_br_bit(r);
        if (b < 0)
            return 0;
        v = (v << 1) | (unsigned)b;
    }
    *value = v;
    return 1;
}

static int hevc_br_ue(HevcBitReader *r, unsigned *value) {
    unsigned zeros = 0;
    unsigned v = 0;
    unsigned i;
    int b;

    for (;;) {
        b = hevc_br_bit(r);
        if (b < 0)
            return 0;
        if (b != 0)
            break;
        if (++zeros > 31u)
            return 0;
    }
    for (i = 0; i < zeros; i++) {
        b = hevc_br_bit(r);
        if (b < 0)
            return 0;
        v = (v << 1) | (unsigned)b;
    }
    *value = zeros == 0u ? 0u : (((1u << zeros) - 1u) + v);
    return 1;
}

static int hevc_br_se(HevcBitReader *r, int *value) {
    unsigned k;
    if (!hevc_br_ue(r, &k))
        return 0;
    *value = (k & 1u) ? (int)((k + 1u) / 2u) : -(int)(k / 2u);
    return 1;
}

/* skip a string of zero bits */
static int hevc_br_skip(HevcBitReader *r, unsigned count) {
    while (count--) {
        if (hevc_br_bit(r) < 0)
            return 0;
    }
    return 1;
}

/* Finds the next Annex-B start code at or after index `from`; returns the offset
 * of its first zero byte or -1.  *prefix receives 3 or 4. */
static int hevc_find_start_code(const uint8_t *au, unsigned size, unsigned from,
                                unsigned *prefix) {
    unsigned i = from;

    while (i + 3u <= size) {
        if (au[i] == 0 && au[i + 1] == 0) {
            if (au[i + 2] == 1u) {
                *prefix = 3;
                return (int)i;
            }
            if (i + 4u <= size && au[i + 2] == 0u && au[i + 3] == 1u) {
                *prefix = 4;
                return (int)i;
            }
        }
        i++;
    }
    return -1;
}

/* Sequential variant: consumes the start code at or after *pos and moves *pos
 * past it.  Returns the offset of the start code or -1. */
static int hevc_next_start_code(const uint8_t *au, unsigned size, unsigned *pos,
                                unsigned *prefix) {
    int start = hevc_find_start_code(au, size, *pos, prefix);

    if (start < 0) {
        *pos = size;
        return -1;
    }
    *pos = (unsigned)start + *prefix;
    return start;
}

/* NAL unit type of a 2-byte HEVC NAL header. */
static unsigned hevc_nal_type_of(const uint8_t *nal) {
    return (unsigned)((nal[0] >> 1) & 0x3fu);
}

/* IRAP picture (BLA/IDR/CRA): a new coded video sequence can start here. */
static int hevc_nal_is_irap(unsigned type) {
    return type >= HEVC_NAL_BLA_W_LP && type <= 23u;
}

/* IDR (or BLA) picture: all reference pictures are discarded. */
static int hevc_nal_is_idr(unsigned type) {
    return type == HEVC_NAL_IDR_W_RADL || type == HEVC_NAL_IDR_N_LP ||
           type == HEVC_NAL_BLA_W_LP || type == HEVC_NAL_BLA_W_RADL ||
           type == HEVC_NAL_BLA_N_LP;
}

/* Iterate the NALs of an Annex-B buffer.  Returns 1 for each NAL: *nal_offset is
 * the first payload byte (after the start code), *nal_end one past the last
 * payload byte.  Start with *pos = 0 and stop when the function returns 0.
 * The scan for this NAL's end does NOT consume the next start code, so every
 * NAL of the buffer is visited exactly once. */
static int hevc_next_nal(const uint8_t *au, unsigned size, unsigned *pos,
                         unsigned *nal_offset, unsigned *nal_end) {
    unsigned prefix = 0;
    unsigned start;
    unsigned payload;
    int next_start;

    start = (unsigned)hevc_next_start_code(au, size, pos, &prefix);
    if ((int)start < 0)
        return 0;
    payload = start + prefix;
    if (payload + 2u > size)
        return 0;
    /* The next NAL's start code begins the run of zeros that terminates this
     * one; scanning from payload + 2 (past the 2-byte NAL header) cannot match
     * the header itself. */
    next_start = hevc_find_start_code(au, size, payload + 2u, &prefix);
    if (next_start >= 0) {
        *nal_end = (unsigned)next_start;
        *pos = (unsigned)next_start;
    } else {
        *nal_end = size;
        *pos = size;
    }
    *nal_offset = payload;
    return *nal_end > payload;
}

/* pps_pic_parameter_set_id of the first slice segment in an access unit.
 * *ok is 1 when a slice was found and its header parsed. */
unsigned hevc_first_slice_pps_id(const uint8_t *au, unsigned size, int *ok) {
    unsigned pos = 0;
    unsigned off;
    unsigned end;

    if (ok)
        *ok = 0;
    if (!au || size < 6u)
        return 0;

    while (hevc_next_nal(au, size, &pos, &off, &end)) {
        unsigned type = hevc_nal_type_of(au + off);
        HevcBitReader r;
        unsigned first;
        unsigned pps_id;

        if (type > 31u)
            continue;
        if (end <= off + 2u)
            continue;
        memset(&r, 0, sizeof(r));
        r.p = au + off + 2u;
        r.size = end - (off + 2u);
        if (!hevc_br_u(&r, 1, &first))
            continue;
        if (hevc_nal_is_irap(type) && !hevc_br_skip(&r, 1)) /* no_output_of_prior_pics_flag */
            continue;
        if (!hevc_br_ue(&r, &pps_id))
            continue;
        if (pps_id > 63u)
            continue;
        if (ok)
            *ok = 1;
        return pps_id;
    }
    return 0;
}

/* NAL unit type of the first VCL NAL in an access unit, or 0xffffffff when the
 * access unit carries no slice. */
unsigned hevc_first_slice_nal_type(const uint8_t *au, unsigned size) {
    unsigned pos = 0;
    unsigned off;
    unsigned end;

    if (!au)
        return 0xffffffffu;
    while (hevc_next_nal(au, size, &pos, &off, &end)) {
        unsigned type = hevc_nal_type_of(au + off);
        if (type <= 31u)
            return type;
    }
    return 0xffffffffu;
}

/* The parameter sets of an access unit, concatenated as Annex-B (4-byte start
 * codes, original NAL payloads untouched).  This is the "ffmpeg / PotPlayer"
 * client style: the access unit carries VPS/SPS/PPS itself, so forwarding them
 * is exact -- including streams the synthesiser refuses. */
typedef struct {
    unsigned vps_present;
    unsigned sps_present;
    unsigned pps_present;
    unsigned total; /* bytes written into out */
} HevcCollectedParams;

unsigned hevc_collect_parameter_sets(const uint8_t *au, unsigned size, uint8_t *out,
                                     unsigned cap, HevcCollectedParams *info) {
    unsigned pos = 0;
    unsigned off;
    unsigned end;

    if (!info)
        return 0;
    memset(info, 0, sizeof(*info));
    if (!au || !out)
        return 0;

    while (hevc_next_nal(au, size, &pos, &off, &end)) {
        unsigned type = hevc_nal_type_of(au + off);
        unsigned nal_size = end - off;
        unsigned want;

        if (type != HEVC_NAL_VPS && type != HEVC_NAL_SPS && type != HEVC_NAL_PPS)
            continue;
        /* 256 KiB is far above any real parameter set; refuse garbage. */
        if (nal_size == 0 || nal_size > 256u * 1024u)
            continue;
        want = 4u + nal_size;
        if (info->total + want > cap)
            return info->total;
        out[info->total + 0] = 0;
        out[info->total + 1] = 0;
        out[info->total + 2] = 0;
        out[info->total + 3] = 1;
        memcpy(out + info->total + 4, au + off, nal_size);
        info->total += want;
        if (type == HEVC_NAL_VPS)
            info->vps_present = 1;
        else if (type == HEVC_NAL_SPS)
            info->sps_present = 1;
        else
            info->pps_present = 1;
    }
    return info->total;
}

/* ------------------------------------------------------------------------- */
/* Parameter set parsers (diagnostics + the test harness' oracle)            */
/* ------------------------------------------------------------------------- */

static int hevc_parse_ptl(HevcBitReader *r, unsigned *profile_idc,
                          unsigned *level_idc) {
    unsigned v;
    unsigned compat_main;

    if (!hevc_br_u(r, 2, &v)) /* general_profile_space */
        return 0;
    if (!hevc_br_u(r, 1, &v)) /* general_tier_flag */
        return 0;
    if (!hevc_br_u(r, 5, &v)) /* general_profile_idc */
        return 0;
    *profile_idc = v;
    if (!hevc_br_u(r, 1, &v)) /* general_profile_compatibility_flag[0] */
        return 0;
    if (!hevc_br_u(r, 1, &compat_main)) /* [1] */
        return 0;
    if (!hevc_br_skip(r, 30u)) /* [2..31] */
        return 0;
    if (!hevc_br_skip(r, 4u)) /* source / constraint flags */
        return 0;
    if (!hevc_br_skip(r, 44u)) /* 43 reserved + inbld */
        return 0;
    if (!hevc_br_u(r, 8, level_idc))
        return 0;
    (void)compat_main;
    return 1;
}

/* Parses a VPS NAL (payload starts after the 2-byte header).  Only the ids are
 * of interest here; the rest is skipped so a malformed VPS is still noticeable. */
int hevc_parse_vps(const uint8_t *nal, unsigned size, unsigned *vps_id,
                   unsigned *level_idc) {
    HevcBitReader r;
    unsigned v;

    memset(&r, 0, sizeof(r));
    if (vps_id)
        *vps_id = 0;
    if (level_idc)
        *level_idc = 0;
    if (!nal || size < 3u || hevc_nal_type_of(nal) != HEVC_NAL_VPS)
        return 0;
    r.p = nal + 2u;
    r.size = size - 2u;
    if (!hevc_br_u(&r, 4, &v))
        return 0;
    if (vps_id)
        *vps_id = v;
    if (!hevc_br_skip(&r, 2u)) /* base_layer_internal/available */
        return 0;
    if (!hevc_br_skip(&r, 6u)) /* max_layers_minus1 */
        return 0;
    if (!hevc_br_skip(&r, 3u)) /* max_sub_layers_minus1 */
        return 0;
    if (!hevc_br_skip(&r, 1u)) /* temporal_id_nesting */
        return 0;
    if (!hevc_br_skip(&r, 16u)) /* vps_reserved_0xffff_16bits */
        return 0;
    return hevc_parse_ptl(&r, &v, level_idc);
}

int hevc_parse_sps(const uint8_t *nal, unsigned size, HevcParamSetFields *f) {
    HevcBitReader r;
    unsigned v;
    unsigned sub_layers_minus1;
    unsigned profile_idc;
    unsigned width = 0;  /* pic_width_in_luma_samples (coded, min-CB aligned) */
    unsigned height = 0; /* pic_height_in_luma_samples */

    if (!f)
        return 0;
    memset(f, 0, sizeof(*f));
    memset(&r, 0, sizeof(r));
    if (!nal || size < 4u || hevc_nal_type_of(nal) != HEVC_NAL_SPS)
        return 0;
    r.p = nal + 2u;
    r.size = size - 2u;

    if (!hevc_br_u(&r, 4, &v)) /* sps_video_parameter_set_id */
        return 0;
    f->vps_id = v;
    if (!hevc_br_u(&r, 3, &sub_layers_minus1))
        return 0;
    if (!hevc_br_skip(&r, 1u)) /* sps_temporal_id_nesting_flag */
        return 0;
    if (!hevc_parse_ptl(&r, &profile_idc, &f->level_idc))
        return 0;
    if (!hevc_br_ue(&r, &f->sps_id))
        return 0;
    if (!hevc_br_ue(&r, &f->chroma_format_idc))
        return 0;
    if (f->chroma_format_idc == 3u) {
        if (!hevc_br_u(&r, 1, &f->separate_colour_plane_flag))
            return 0;
    }
    {
        unsigned crop_flag = 0;
        unsigned left = 0, right = 0, top = 0, bottom = 0;
        unsigned sub_w = (f->chroma_format_idc == 1u || f->chroma_format_idc == 2u) ? 2u : 1u;
        unsigned sub_h = (f->chroma_format_idc == 1u) ? 2u : 1u;

        if (!hevc_br_ue(&r, &width) || !hevc_br_ue(&r, &height))
            return 0;
        if (!hevc_br_u(&r, 1, &crop_flag)) /* conformance_window_flag */
            return 0;
        if (crop_flag) {
            if (!hevc_br_ue(&r, &left) || !hevc_br_ue(&r, &right) ||
                !hevc_br_ue(&r, &top) || !hevc_br_ue(&r, &bottom))
                return 0;
        }
        if (width <= (left + right) * sub_w || height <= (top + bottom) * sub_h)
            return 0;
        /* What the decoder really outputs after the conformance window. */
        f->visible_width = width - (left + right) * sub_w;
        f->visible_height = height - (top + bottom) * sub_h;
    }
    if (!hevc_br_ue(&r, &f->bit_depth_luma_minus8))
        return 0;
    if (!hevc_br_ue(&r, &f->bit_depth_chroma_minus8))
        return 0;
    if (!hevc_br_ue(&r, &f->log2_max_pic_order_cnt_lsb_minus4))
        return 0;
    if (!hevc_br_u(&r, 1, &v)) /* sps_sub_layer_ordering_info_present_flag */
        return 0;
    {
        unsigned i;
        unsigned first = v ? 0u : sub_layers_minus1;
        for (i = first; i <= sub_layers_minus1; i++) {
            unsigned b, reorder, latency;
            if (!hevc_br_ue(&r, &b) || !hevc_br_ue(&r, &reorder) ||
                !hevc_br_ue(&r, &latency))
                return 0;
            if (i == first)
                f->sps_max_dec_pic_buffering_minus1 = b;
        }
    }
    if (!hevc_br_ue(&r, &f->log2_min_luma_coding_block_size_minus3))
        return 0;
    /* PicWidthInMinCbsY is what DXVA carries: ceil(width / MinCbSizeY). The SPS
     * width above is the coded one (min-CB aligned), so this is exact. */
    {
        unsigned min_cb = 1u << (f->log2_min_luma_coding_block_size_minus3 + 3u);
        f->pic_width_in_min_cbs = (width + min_cb - 1u) / min_cb;
        f->pic_height_in_min_cbs = (height + min_cb - 1u) / min_cb;
    }
    if (!hevc_br_ue(&r, &f->log2_diff_max_min_luma_coding_block_size))
        return 0;
    if (!hevc_br_ue(&r, &f->log2_min_luma_transform_block_size_minus2))
        return 0;
    if (!hevc_br_ue(&r, &f->log2_diff_max_min_luma_transform_block_size))
        return 0;
    if (!hevc_br_ue(&r, &f->max_transform_hierarchy_depth_inter))
        return 0;
    if (!hevc_br_ue(&r, &f->max_transform_hierarchy_depth_intra))
        return 0;
    if (!hevc_br_u(&r, 1, &f->scaling_list_enabled_flag))
        return 0;
    if (f->scaling_list_enabled_flag) {
        unsigned present;
        if (!hevc_br_u(&r, 1, &present))
            return 0;
        if (present) {
            /* scaling_list_data(): skip the predicted/expanded forms. */
            unsigned size_id;
            for (size_id = 0; size_id < 4u; size_id++) {
                unsigned matrices = size_id == 3u ? 2u : 6u;
                unsigned matrix_id;
                for (matrix_id = 0; matrix_id < matrices; matrix_id++) {
                    unsigned pred_mode;
                    if (!hevc_br_u(&r, 1, &pred_mode))
                        return 0;
                    if (!pred_mode) {
                        unsigned delta;
                        if (!hevc_br_ue(&r, &delta))
                            return 0;
                    } else {
                        unsigned coef_num = size_id == 0u ? 16u : 64u;
                        unsigned j;
                        int coef;
                        if (!hevc_br_se(&r, &coef)) /* scaling_list_delta_coef */
                            return 0;
                        for (j = 1; j < coef_num; j++) {
                            if (!hevc_br_se(&r, &coef))
                                return 0;
                        }
                    }
                }
            }
        }
    }
    if (!hevc_br_u(&r, 1, &f->amp_enabled_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->sample_adaptive_offset_enabled_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->pcm_enabled_flag))
        return 0;
    if (f->pcm_enabled_flag) {
        unsigned a, b;
        if (!hevc_br_u(&r, 4, &a) || !hevc_br_u(&r, 4, &b))
            return 0;
        f->pcm_sample_bit_depth_luma_minus1 = a;
        f->pcm_sample_bit_depth_chroma_minus1 = b;
        if (!hevc_br_ue(&r, &f->log2_min_pcm_luma_coding_block_size_minus3))
            return 0;
        if (!hevc_br_ue(&r, &f->log2_diff_max_min_pcm_luma_coding_block_size))
            return 0;
        if (!hevc_br_u(&r, 1, &f->pcm_loop_filter_disabled_flag))
            return 0;
    }
    if (!hevc_br_ue(&r, &f->num_short_term_ref_pic_sets))
        return 0;
    if (f->num_short_term_ref_pic_sets != 0u) {
        /* Refused by the synthesiser; the rest of the SPS cannot be parsed
         * without expanding the set table, so stop here. */
        return 1;
    }
    if (!hevc_br_u(&r, 1, &f->long_term_ref_pics_present_flag))
        return 0;
    if (f->long_term_ref_pics_present_flag) {
        unsigned count, i;
        unsigned bits = f->log2_max_pic_order_cnt_lsb_minus4 + 4u;
        if (!hevc_br_ue(&r, &count))
            return 0;
        f->num_long_term_ref_pics_sps = count;
        for (i = 0; i < count; i++) {
            if (!hevc_br_skip(&r, bits + 1u))
                return 0;
        }
    }
    if (!hevc_br_u(&r, 1, &f->sps_temporal_mvp_enabled_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->strong_intra_smoothing_enabled_flag))
        return 0;
    return 1;
}

int hevc_parse_pps(const uint8_t *nal, unsigned size, HevcParamSetFields *f) {
    HevcBitReader r;
    unsigned v;

    if (!f)
        return 0;
    memset(&r, 0, sizeof(r));
    if (!nal || size < 3u || hevc_nal_type_of(nal) != HEVC_NAL_PPS)
        return 0;
    r.p = nal + 2u;
    r.size = size - 2u;

    if (!hevc_br_ue(&r, &f->pps_id))
        return 0;
    if (!hevc_br_ue(&r, &f->sps_id))
        return 0;
    if (!hevc_br_u(&r, 1, &f->dependent_slice_segments_enabled_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->output_flag_present_flag))
        return 0;
    if (!hevc_br_u(&r, 3, &f->num_extra_slice_header_bits))
        return 0;
    if (!hevc_br_u(&r, 1, &f->sign_data_hiding_enabled_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->cabac_init_present_flag))
        return 0;
    if (!hevc_br_ue(&r, &f->num_ref_idx_l0_default_active_minus1))
        return 0;
    if (!hevc_br_ue(&r, &f->num_ref_idx_l1_default_active_minus1))
        return 0;
    if (!hevc_br_se(&r, &f->init_qp_minus26))
        return 0;
    if (!hevc_br_u(&r, 1, &f->constrained_intra_pred_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->transform_skip_enabled_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->cu_qp_delta_enabled_flag))
        return 0;
    if (f->cu_qp_delta_enabled_flag) {
        if (!hevc_br_ue(&r, &f->diff_cu_qp_delta_depth))
            return 0;
    }
    if (!hevc_br_se(&r, &f->pps_cb_qp_offset))
        return 0;
    if (!hevc_br_se(&r, &f->pps_cr_qp_offset))
        return 0;
    if (!hevc_br_u(&r, 1, &f->pps_slice_chroma_qp_offsets_present_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->weighted_pred_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->weighted_bipred_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->transquant_bypass_enabled_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->tiles_enabled_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &f->entropy_coding_sync_enabled_flag))
        return 0;
    if (f->tiles_enabled_flag) {
        if (!hevc_br_ue(&r, &f->num_tile_columns_minus1))
            return 0;
        if (!hevc_br_ue(&r, &f->num_tile_rows_minus1))
            return 0;
        if (!hevc_br_u(&r, 1, &f->uniform_spacing_flag))
            return 0;
        if (!f->uniform_spacing_flag) {
            unsigned i;
            if (f->num_tile_columns_minus1 > 19u || f->num_tile_rows_minus1 > 21u)
                return 0;
            for (i = 0; i < f->num_tile_columns_minus1; i++) {
                if (!hevc_br_ue(&r, &f->column_width_minus1[i]))
                    return 0;
            }
            for (i = 0; i < f->num_tile_rows_minus1; i++) {
                if (!hevc_br_ue(&r, &f->row_height_minus1[i]))
                    return 0;
            }
        }
        if (!hevc_br_u(&r, 1, &f->loop_filter_across_tiles_enabled_flag))
            return 0;
    }
    if (!hevc_br_u(&r, 1, &f->pps_loop_filter_across_slices_enabled_flag))
        return 0;
    if (!hevc_br_u(&r, 1, &v)) /* deblocking_filter_control_present_flag */
        return 0;
    if (v) {
        if (!hevc_br_u(&r, 1, &f->deblocking_filter_override_enabled_flag))
            return 0;
        if (!hevc_br_u(&r, 1, &f->pps_deblocking_filter_disabled_flag))
            return 0;
        if (!f->pps_deblocking_filter_disabled_flag) {
            if (!hevc_br_se(&r, &f->pps_beta_offset_div2))
                return 0;
            if (!hevc_br_se(&r, &f->pps_tc_offset_div2))
                return 0;
        }
    }
    if (!hevc_br_u(&r, 1, &v)) /* pps_scaling_list_data_present_flag */
        return 0;
    if (!hevc_br_u(&r, 1, &f->lists_modification_present_flag))
        return 0;
    if (!hevc_br_ue(&r, &f->log2_parallel_merge_level_minus2))
        return 0;
    if (!hevc_br_u(&r, 1, &f->slice_segment_header_extension_present_flag))
        return 0;
    return 1;
}

/* Fills `f` from the parameter set NALs of one access unit (sizes include the
 * 2-byte NAL header).  The three NALs must be the first VPS/SPS/PPS seen. */
int hevc_fields_from_parameter_sets(const uint8_t *vps, unsigned vps_size,
                                    const uint8_t *sps, unsigned sps_size,
                                    const uint8_t *pps, unsigned pps_size,
                                    HevcParamSetFields *f) {
    unsigned vps_id = 0;
    unsigned level = 0;

    if (!f || !sps || !pps)
        return 0;
    memset(f, 0, sizeof(*f));
    if (vps && vps_size)
        hevc_parse_vps(vps, vps_size, &vps_id, &level);
    if (!hevc_parse_sps(sps, sps_size, f))
        return 0;
    if (!hevc_parse_pps(pps, pps_size, f))
        return 0;
    f->vps_id = vps_id;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* VIRTIO_WDDM_HEVC_PARAMS_H */
