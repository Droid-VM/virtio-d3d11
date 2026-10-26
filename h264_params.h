/* H.264 Annex-B SPS/PPS synthesis from DXVA_PicParams_H264 (P2-2i).
 *
 * Why this file exists
 * --------------------
 * Chromium's D3D11 H.264 decoder only puts slice NALs into the BITSTREAM
 * decoder buffer (each slice prefixed with a 3-byte start code,
 * media/gpu/windows/d3d11_h264_accelerator.cc,
 * AppendBitstreamAndSliceDataWithStartCode<DXVA_Slice_H264_Short>). The SPS and
 * PPS are *not* in that buffer: they travel in the PICTURE_PARAMETERS buffer as
 * a DXVA_PicParams_H264, which is a parsed/derived structure, not an SPS.
 *
 * The host virtio-media decoder is a V4L2 stateful decoder: it is handed exactly
 * the bytes of the BITSTREAM buffer (P2-2h driver log: "bitstream: 26690
 * bytes"), so without in-band parameter sets it cannot decode anything
 * (DQBUF bytesused=0 -> eventq timeout -> STATUS_IO_TIMEOUT).
 *
 * This header rebuilds the minimal H.264 SPS (NAL 7) and PPS (NAL 8) that are
 * consistent with the slices Chromium sends, from the values Chromium puts in
 * DXVA_PicParams_H264:
 *
 *   SPS <- profile_idc, level_idc, seq_parameter_set_id,
 *          wFrameWidthInMbsMinus1/wFrameHeightInMbsMinus1,
 *          chroma_format_idc, bit_depth_{luma,chroma}_minus8,
 *          log2_max_frame_num_minus4, pic_order_cnt_type,
 *          log2_max_pic_order_cnt_lsb_minus4,
 *          delta_pic_order_always_zero_flag, num_ref_frames,
 *          frame_mbs_only_flag, MbaffFrameFlag, direct_8x8_inference_flag,
 *          constrained_intra_pred_flag
 *   PPS <- seq_parameter_set_id, entropy_coding_mode_flag,
 *          pic_order_present_flag, num_ref_idx_l{0,1}_active_minus1,
 *          weighted_pred_flag, weighted_bipred_idc, pic_init_qp_minus26,
 *          pic_init_qs_minus26, chroma_qp_index_offset,
 *          second_chroma_qp_index_offset, deblocking_filter_control_present_flag,
 *          constrained_intra_pred_flag, redundant_pic_cnt_present_flag,
 *          transform_8x8_mode_flag
 *
 * The PPS's pic_parameter_set_id is taken from the first slice NAL of the
 * access unit (the slice header carries it; DXVA_PicParams_H264 does not), so a
 * stream that uses a PPS id other than 0 still gets a matching PPS.
 *
 * Limitations, deliberately not papered over:
 *   - FMO/ASO (num_slice_groups_minus1 != 0) is not synthesised; the decoder
 *     profile we report is DXVA_ModeH264_VLD_NoFGT, so a conforming client
 *     never asks for it (h264_synth_sps_pps refuses, caller falls back).
 *   - pic_order_cnt_type 1 has no offsets in DXVA_PicParams_H264. They are
 *     written as 0 / cycle length 0. That does not change slice parsing, only
 *     the derived POC values; acceptable while the one-shot driver decodes one
 *     access unit per host session.
 *   - VUI is not emitted (vui_parameters_present_flag = 0): no aspect ratio /
 *     timing / colour description. The host decoder does not need them to
 *     decode; it gets the timing from the virtio-media frames.
 *   - Scaling matrices are not emitted (seq_scaling_matrix_present_flag = 0,
 *     pic_scaling_matrix_present_flag = 0). x264/ffmpeg default streams do not
 *     use them; a stream that does would need the DXVA_Qmatrix_H264 buffer,
 *     which this UMD logs but does not yet forward.
 *
 * Everything here is plain C99 (stdint only) so the same code can be compiled
 * into the UMD (ARM64 PE) and into a Linux test harness that compares the
 * synthesised parameter sets against the reference stream.
 *
 * Unit conventions: the fields are stored in the units the SPS/PPS syntax
 * wants, i.e. "minus1"/"minus4" values are already added back (frame_width_mbs
 * is the real MB count, log2_max_frame_num_minus4 is the real ..._minus4).
 */
#ifndef VIRTIO_WDDM_H264_PARAMS_H
#define VIRTIO_WDDM_H264_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Biggest SPS+PPS this writer can emit (Annex-B, both 4-byte start codes).
 * Real ones are < 40 bytes each; this is only a sanity bound for callers. */
#define H264_PARAMS_MAX_BYTES 512u

typedef struct {
    unsigned frame_width_mbs;  /* wFrameWidthInMbsMinus1 + 1 */
    unsigned frame_height_mbs; /* wFrameHeightInMbsMinus1 + 1 */

    unsigned seq_parameter_set_id;
    unsigned profile_idc;
    unsigned level_idc;

    unsigned chroma_format_idc;
    unsigned bit_depth_luma_minus8;
    unsigned bit_depth_chroma_minus8;

    unsigned log2_max_frame_num_minus4;
    unsigned pic_order_cnt_type;
    unsigned log2_max_pic_order_cnt_lsb_minus4;
    unsigned delta_pic_order_always_zero_flag;

    unsigned num_ref_frames;
    unsigned frame_mbs_only_flag;
    unsigned mb_adaptive_frame_field_flag;
    unsigned direct_8x8_inference_flag;

    /* PPS side */
    unsigned num_slice_groups_minus1;
    unsigned num_ref_idx_l0_active_minus1;
    unsigned num_ref_idx_l1_active_minus1;
    unsigned weighted_pred_flag;
    unsigned weighted_bipred_idc;
    int pic_init_qp_minus26;
    int pic_init_qs_minus26;
    int chroma_qp_index_offset;
    int second_chroma_qp_index_offset;
    unsigned deblocking_filter_control_present_flag;
    unsigned constrained_intra_pred_flag;
    unsigned redundant_pic_cnt_present_flag;
    unsigned entropy_coding_mode_flag;
    unsigned pic_order_present_flag;
    unsigned transform_8x8_mode_flag;

    /* Display size used for frame_cropping; 0 means "no cropping info", which
     * encodes the SPS dimensions as they are (coded size). */
    unsigned visible_width;
    unsigned visible_height;
} H264ParamSetFields;

/* Writes "00 00 00 01 67 <SPS> 00 00 00 01 68 <PPS>" into out. Returns the
 * number of bytes written (0 on refusal / buffer too small). *sps_bytes and
 * *pps_bytes receive the per-NAL sizes including their 4-byte start codes. */
unsigned h264_synth_sps_pps(const H264ParamSetFields *fields, unsigned pps_id,
                            uint8_t *out, unsigned out_cap, unsigned *sps_bytes,
                            unsigned *pps_bytes);

/* Returns the pic_parameter_set_id of the first VCL slice NAL (type 1 or 5) in
 * an Annex-B access unit. *ok is set to 1 when a slice was found and parsed. */
unsigned h264_first_slice_pps_id(const uint8_t *au, unsigned size, int *ok);

/* ------------------------------------------------------------------------- */
/* Implementation (static: this header is included by exactly one TU)         */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *out;
    unsigned cap;
    unsigned size;
    uint32_t acc;
    unsigned nbits;
    int overflow;
} H264BitWriter;

static void h264_bw_put(H264BitWriter *w, uint32_t value, unsigned bits) {
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

/* ue(v): leadingZeroBits = floor(log2(codeNum)), then codeNum in that many + 1
 * bits. value is clamped to 2^31-1, far above anything H.264 allows here. */
static void h264_bw_ue(H264BitWriter *w, unsigned value) {
    uint32_t code = (uint32_t)value + 1u;
    unsigned n = 0;
    uint32_t scan = code;

    while (scan > 1u) {
        scan >>= 1;
        n++;
    }
    h264_bw_put(w, 0, n); /* n zero bits */
    h264_bw_put(w, code, n + 1u);
}

/* se(v): codeNum = 2|v|-1 for v>0, 2|v| otherwise, then ue(codeNum). */
static void h264_bw_se(H264BitWriter *w, int value) {
    unsigned code = value > 0 ? (unsigned)(2 * value - 1) : (unsigned)(-2 * value);
    h264_bw_ue(w, code);
}

static void h264_bw_trailing(H264BitWriter *w) {
    h264_bw_put(w, 1u, 1u); /* rbsp_stop_one_bit */
    while (w->nbits != 0)
        h264_bw_put(w, 0u, 1u);
}

/* profile_idc values whose SPS/PPS carry the chroma/bit-depth/scaling-matrix
 * extension syntax (H.264 7.3.2.1.1 / 7.3.2.2). */
static int h264_profile_has_extension(unsigned profile_idc) {
    switch (profile_idc) {
    case 100: /* High */
    case 110: /* High 10 */
    case 122: /* High 4:2:2 */
    case 244: /* High 4:4:4 Predictive */
    case 44:  /* CAVLC 4:4:4 */
    case 83:  /* Scalable Baseline */
    case 86:  /* Scalable High */
    case 118: /* Multiview High */
    case 128: /* Stereo High */
    case 138: /* Multiview Depth High */
    case 139:
    case 134:
    case 135:
        return 1;
    default:
        return 0;
    }
}

/* Copy an RBSP into an EBSP: after two consecutive zero bytes, any byte <= 3 is
 * preceded by 0x03 (H.264 7.4.1). Returns bytes written, 0 on overflow. */
static unsigned h264_escape_rbsp(const uint8_t *rbsp, unsigned size, uint8_t *out,
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

/* Annex-B NAL: 4-byte start code + 1-byte NAL header + escaped RBSP. */
static unsigned h264_emit_nal(unsigned nal_type, const uint8_t *rbsp,
                              unsigned rbsp_size, uint8_t *out, unsigned cap) {
    unsigned escaped;

    if (cap < 4u + 1u)
        return 0;
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 1;
    out[4] = (uint8_t)((3u << 5) | (nal_type & 0x1fu)); /* nal_ref_idc=3 */
    escaped = h264_escape_rbsp(rbsp, rbsp_size, out + 5, cap - 5u);
    if (escaped == 0)
        return 0;
    return 4u + 1u + escaped;
}

/* ------------------------------------------------------------------------- */
/* SPS                                                                       */
/* ------------------------------------------------------------------------- */

static unsigned h264_write_sps(const H264ParamSetFields *f, uint8_t *out,
                               unsigned cap) {
    uint8_t rbsp[128];
    H264BitWriter w;
    unsigned coded_width;
    unsigned coded_height;
    unsigned crop_flag = 0;
    unsigned crop_left = 0;
    unsigned crop_right = 0;
    unsigned crop_top = 0;
    unsigned crop_bottom = 0;
    unsigned pic_height_in_map_units;
    unsigned sub_width_c;
    unsigned sub_height_c;
    unsigned crop_unit_x;
    unsigned crop_unit_y;

    if (f->frame_width_mbs == 0 || f->frame_height_mbs == 0)
        return 0;
    if (f->frame_mbs_only_flag == 0 && (f->frame_height_mbs & 1u))
        return 0; /* FrameHeightInMbs must be even for field-coded streams */

    coded_width = f->frame_width_mbs * 16u;
    coded_height = f->frame_height_mbs * 16u;

    /* CropUnitX/Y from H.264 7.4.2.1.1. SubWidthC/SubHeightC are 2/2 for
     * 4:2:0, 2/1 for 4:2:2, 1/1 for 4:4:4 and monochrome. */
    sub_width_c = (f->chroma_format_idc == 1u || f->chroma_format_idc == 2u) ? 2u : 1u;
    sub_height_c = (f->chroma_format_idc == 1u) ? 2u : 1u;
    crop_unit_x = sub_width_c;
    crop_unit_y = sub_height_c * (2u - f->frame_mbs_only_flag);

    if (f->visible_width && f->visible_height && f->visible_width <= coded_width &&
        f->visible_height <= coded_height) {
        unsigned dx = coded_width - f->visible_width;
        unsigned dy = coded_height - f->visible_height;
        if ((dx % crop_unit_x) == 0u && (dy % crop_unit_y) == 0u) {
            crop_right = dx / crop_unit_x;
            crop_bottom = dy / crop_unit_y;
            crop_flag = (crop_right || crop_bottom) ? 1u : 0u;
        }
    }

    w.out = rbsp;
    w.cap = (unsigned)sizeof(rbsp);
    w.size = 0;
    w.acc = 0;
    w.nbits = 0;
    w.overflow = 0;

    h264_bw_put(&w, f->profile_idc & 0xffu, 8);
    h264_bw_put(&w, 0, 6); /* constraint_set0..5_flag: 0 is legal for all */
    h264_bw_put(&w, 0, 2); /* reserved_zero_2bits */
    h264_bw_put(&w, f->level_idc & 0xffu, 8);
    h264_bw_ue(&w, f->seq_parameter_set_id);

    if (h264_profile_has_extension(f->profile_idc)) {
        h264_bw_ue(&w, f->chroma_format_idc);
        if (f->chroma_format_idc == 3u)
            h264_bw_put(&w, 0, 1); /* separate_colour_plane_flag */
        h264_bw_ue(&w, f->bit_depth_luma_minus8);
        h264_bw_ue(&w, f->bit_depth_chroma_minus8);
        h264_bw_put(&w, 0, 1); /* qpprime_y_zero_transform_bypass_flag */
        h264_bw_put(&w, 0, 1); /* seq_scaling_matrix_present_flag */
    }

    h264_bw_ue(&w, f->log2_max_frame_num_minus4);
    h264_bw_ue(&w, f->pic_order_cnt_type);
    if (f->pic_order_cnt_type == 0u) {
        h264_bw_ue(&w, f->log2_max_pic_order_cnt_lsb_minus4);
    } else if (f->pic_order_cnt_type == 1u) {
        /* Not in DXVA_PicParams_H264 (see the header comment): zero offsets and
         * an empty cycle. Slice parsing is unaffected. */
        h264_bw_put(&w, f->delta_pic_order_always_zero_flag, 1);
        h264_bw_se(&w, 0); /* offset_for_non_ref_pic */
        h264_bw_se(&w, 0); /* offset_for_top_to_bottom_field */
        h264_bw_ue(&w, 0); /* num_ref_frames_in_pic_order_cnt_cycle */
    }

    h264_bw_ue(&w, f->num_ref_frames);
    h264_bw_put(&w, 0, 1); /* gaps_in_frame_num_value_allowed_flag */

    pic_height_in_map_units = f->frame_height_mbs / (2u - f->frame_mbs_only_flag);
    h264_bw_ue(&w, f->frame_width_mbs - 1u);
    h264_bw_ue(&w, pic_height_in_map_units - 1u);

    h264_bw_put(&w, f->frame_mbs_only_flag, 1);
    if (!f->frame_mbs_only_flag)
        h264_bw_put(&w, f->mb_adaptive_frame_field_flag, 1);
    h264_bw_put(&w, f->direct_8x8_inference_flag, 1);

    h264_bw_put(&w, crop_flag, 1);
    if (crop_flag) {
        h264_bw_ue(&w, crop_left);
        h264_bw_ue(&w, crop_right);
        h264_bw_ue(&w, crop_top);
        h264_bw_ue(&w, crop_bottom);
    }
    h264_bw_put(&w, 0, 1); /* vui_parameters_present_flag */

    h264_bw_trailing(&w);
    if (w.overflow || w.nbits != 0)
        return 0;
    return h264_emit_nal(7u, rbsp, w.size, out, cap);
}

/* ------------------------------------------------------------------------- */
/* PPS                                                                       */
/* ------------------------------------------------------------------------- */

static unsigned h264_write_pps(const H264ParamSetFields *f, unsigned pps_id,
                               uint8_t *out, unsigned cap) {
    uint8_t rbsp[128];
    H264BitWriter w;
    int with_extension;

    if (pps_id > 255u)
        return 0;

    w.out = rbsp;
    w.cap = (unsigned)sizeof(rbsp);
    w.size = 0;
    w.acc = 0;
    w.nbits = 0;
    w.overflow = 0;

    h264_bw_ue(&w, pps_id);
    h264_bw_ue(&w, f->seq_parameter_set_id);
    h264_bw_put(&w, f->entropy_coding_mode_flag, 1);
    h264_bw_put(&w, f->pic_order_present_flag, 1);
    h264_bw_ue(&w, 0); /* num_slice_groups_minus1: FMO is refused above */
    h264_bw_ue(&w, f->num_ref_idx_l0_active_minus1);
    h264_bw_ue(&w, f->num_ref_idx_l1_active_minus1);
    h264_bw_put(&w, f->weighted_pred_flag, 1);
    h264_bw_put(&w, f->weighted_bipred_idc, 2);
    h264_bw_se(&w, f->pic_init_qp_minus26);
    h264_bw_se(&w, f->pic_init_qs_minus26);
    h264_bw_se(&w, f->chroma_qp_index_offset);
    h264_bw_put(&w, f->deblocking_filter_control_present_flag, 1);
    h264_bw_put(&w, f->constrained_intra_pred_flag, 1);
    h264_bw_put(&w, f->redundant_pic_cnt_present_flag, 1);

    /* The PPS extension is present when more_rbsp_data() is true. A high
     * profile decoder expects it; a stream that enables 8x8 transform anywhere
     * needs transform_8x8_mode_flag = 1. Emit it in both cases. */
    with_extension = h264_profile_has_extension(f->profile_idc) ||
                     f->transform_8x8_mode_flag;
    if (with_extension) {
        h264_bw_put(&w, f->transform_8x8_mode_flag, 1);
        h264_bw_put(&w, 0, 1); /* pic_scaling_matrix_present_flag */
        h264_bw_se(&w, f->second_chroma_qp_index_offset);
    }

    h264_bw_trailing(&w);
    if (w.overflow || w.nbits != 0)
        return 0;
    return h264_emit_nal(8u, rbsp, w.size, out, cap);
}

unsigned h264_synth_sps_pps(const H264ParamSetFields *fields, unsigned pps_id,
                            uint8_t *out, unsigned out_cap, unsigned *sps_bytes,
                            unsigned *pps_bytes) {
    unsigned sps;
    unsigned pps;
    unsigned total = 0;

    if (sps_bytes)
        *sps_bytes = 0;
    if (pps_bytes)
        *pps_bytes = 0;
    if (!fields || !out || out_cap < 16u)
        return 0;

    /* Only 4:2:0 8-bit NV12 is what this UMD advertises; 4:4:4/monochrome would
     * need formats the host decoder does not produce here. */
    if (fields->chroma_format_idc != 1u)
        return 0;
    if (fields->bit_depth_luma_minus8 != 0u || fields->bit_depth_chroma_minus8 != 0u)
        return 0;
    if (fields->num_slice_groups_minus1 != 0u)
        return 0;
    if (fields->pic_order_cnt_type > 2u)
        return 0;

    sps = h264_write_sps(fields, out, out_cap);
    if (sps == 0)
        return 0;
    pps = h264_write_pps(fields, pps_id, out + sps, out_cap - sps);
    if (pps == 0)
        return 0;
    total = sps + pps;

    if (sps_bytes)
        *sps_bytes = sps;
    if (pps_bytes)
        *pps_bytes = pps;
    return total;
}

/* ------------------------------------------------------------------------- */
/* Slice header: pic_parameter_set_id                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    unsigned size;
    unsigned pos;
    unsigned cur;
    int bits;
    unsigned zeros;
} H264BitReader;

/* Next RBSP byte of the NAL payload, removing emulation_prevention_three_byte
 * (0x03 after two zero bytes). Returns -1 at the end. */
static int h264_br_byte(H264BitReader *r) {
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

static int h264_br_bit(H264BitReader *r) {
    int b;
    if (r->bits == 0) {
        b = h264_br_byte(r);
        if (b < 0)
            return -1;
        r->cur = (unsigned)b;
        r->bits = 8;
    }
    r->bits--;
    return (int)((r->cur >> r->bits) & 1u);
}

static int h264_br_ue(H264BitReader *r, unsigned *value) {
    unsigned zeros = 0;
    unsigned v = 0;
    unsigned i;
    int b;

    for (;;) {
        b = h264_br_bit(r);
        if (b < 0)
            return 0;
        if (b != 0)
            break;
        if (++zeros > 31u)
            return 0;
    }
    for (i = 0; i < zeros; i++) {
        b = h264_br_bit(r);
        if (b < 0)
            return 0;
        v = (v << 1) | (unsigned)b;
    }
    if (zeros == 0)
        *value = 0;
    else
        *value = ((1u << zeros) - 1u) + v;
    return 1;
}

/* Finds the next Annex-B start code at or after *pos (pos is updated past it);
 * returns the offset of the start code or -1. *prefix receives 3 or 4. */
static int h264_next_start_code(const uint8_t *au, unsigned size, unsigned *pos,
                                unsigned *prefix) {
    unsigned i = *pos;
    while (i + 3u <= size) {
        if (au[i] == 0 && au[i + 1] == 0) {
            if (au[i + 2] == 1u) {
                *prefix = 3;
                *pos = i + 3u;
                return (int)i;
            }
            if (i + 4u <= size && au[i + 2] == 0u && au[i + 3] == 1u) {
                *prefix = 4;
                *pos = i + 4u;
                return (int)i;
            }
        }
        i++;
    }
    *pos = size;
    return -1;
}

unsigned h264_first_slice_pps_id(const uint8_t *au, unsigned size, int *ok) {
    unsigned pos = 0;
    unsigned prefix = 0;

    if (ok)
        *ok = 0;
    if (!au || size < 5u)
        return 0;

    for (;;) {
        int start = h264_next_start_code(au, size, &pos, &prefix);
        unsigned nal;
        unsigned type;
        unsigned next_pos;
        unsigned next_prefix;
        unsigned end;
        H264BitReader r;
        unsigned first_mb;
        unsigned slice_type;
        unsigned pps_id;

        if (start < 0)
            break;
        nal = (unsigned)start + prefix;
        if (nal >= size)
            break;
        type = au[nal] & 0x1fu;
        next_pos = nal + 1u;
        if (h264_next_start_code(au, size, &next_pos, &next_prefix) >= 0)
            end = next_pos - next_prefix;
        else
            end = size;
        pos = next_pos;

        if (type != 1u && type != 5u)
            continue;
        if (end <= nal + 1u)
            continue;

        r.p = au + nal + 1u;
        r.size = end - (nal + 1u);
        r.pos = 0;
        r.cur = 0;
        r.bits = 0;
        r.zeros = 0;
        if (!h264_br_ue(&r, &first_mb) || !h264_br_ue(&r, &slice_type) ||
            !h264_br_ue(&r, &pps_id))
            continue;
        (void)first_mb;
        (void)slice_type;
        if (pps_id > 255u)
            continue;
        if (ok)
            *ok = 1;
        return pps_id;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* VIRTIO_WDDM_H264_PARAMS_H */
