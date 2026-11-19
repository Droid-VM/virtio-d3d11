/* h264_bits.c - see h264_bits.h.  Plain C, no Windows dependency. */
#include "h264_bits.h"

#include <string.h>

#define NAL_SLICE      1
#define NAL_IDR        5
#define NAL_SPS        7
#define NAL_PPS        8
#define NAL_AUD        9
#define NAL_FILLER     12

/* ------------------------------------------------------------------------- */
/* NAL iteration                                                              */
/* ------------------------------------------------------------------------- */

static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end,
                                      size_t *sc_len)
{
    const uint8_t *q = p;

    while (q + 3 <= end) {
        if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q + 4 <= end && q[3] == 1) {
            *sc_len = 4;
            return q;
        }
        if (q[0] == 0 && q[1] == 0 && q[2] == 1) {
            *sc_len = 3;
            return q;
        }
        q++;
    }
    return NULL;
}

/* Returns the next Annex-B NAL payload at or after *pos. */
static int annexb_next(const uint8_t *data, size_t len, size_t *pos,
                       const uint8_t **nal, size_t *nal_len)
{
    const uint8_t *end = data + len;
    const uint8_t *sc;
    const uint8_t *next;
    const uint8_t *nend;
    size_t sc_len = 0;
    size_t next_sc = 0;

    if (*pos >= len)
        return 0;
    sc = find_start_code(data + *pos, end, &sc_len);
    if (sc == NULL)
        return 0;
    next = find_start_code(sc + sc_len, end, &next_sc);
    nend = next ? next : end;
    /* Trailing zero bytes belong to the next start code (or are
     * cabac_zero_word padding); the RBSP trailing bits guarantee that the
     * real last byte of a NAL is non-zero. */
    while (nend > sc + sc_len && nend[-1] == 0)
        nend--;
    *nal = sc + sc_len;
    *nal_len = (size_t)(nend - (sc + sc_len));
    *pos = (size_t)((next ? next : end) - data);
    return *nal_len != 0;
}

/* Returns the next 4-byte-length-prefixed NAL at or after *pos. */
static int avcc_next(const uint8_t *data, size_t len, size_t *pos,
                     const uint8_t **nal, size_t *nal_len)
{
    uint32_t n;

    if (*pos + 4 > len)
        return 0;
    n = ((uint32_t)data[*pos] << 24) | ((uint32_t)data[*pos + 1] << 16) |
        ((uint32_t)data[*pos + 2] << 8) | (uint32_t)data[*pos + 3];
    if (n == 0 || (size_t)n > len - *pos - 4)
        return 0;
    *nal = data + *pos + 4;
    *nal_len = (size_t)n;
    *pos += 4 + (size_t)n;
    return 1;
}

int h264_bits_is_avcc(const uint8_t *data, size_t len)
{
    uint32_t l;
    size_t off;

    if (data == NULL || len < 6)
        return 0;
    /* Any start code means Annex-B, including a 3-byte one at the very front. */
    if (data[0] == 0 && data[1] == 0 && data[2] == 1)
        return 0;
    if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
        return 0;

    l = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    if (l == 0 || (size_t)l > len - 4)
        return 0;
    off = 4 + (size_t)l;
    if (off == len)
        return 1;
    if (off + 4 <= len) {
        uint32_t l2 = ((uint32_t)data[off] << 24) |
                      ((uint32_t)data[off + 1] << 16) |
                      ((uint32_t)data[off + 2] << 8) |
                      (uint32_t)data[off + 3];
        if (l2 != 0 && (size_t)l2 <= len - off - 4)
            return 1;
    }
    /* A single length-prefixed NAL whose length does not exactly fill the
     * sample is still worth accepting: the driver only needs the bytes. */
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Bit reader (SPS parsing)                                                   */
/* ------------------------------------------------------------------------- */

typedef struct bitreader {
    const uint8_t *p;
    size_t         len_bits;
    size_t         bit;
    int            err;
} bitreader;

static uint32_t br_u(bitreader *br, unsigned n)
{
    uint32_t v = 0;
    unsigned i;

    if (br->err)
        return 0;
    if (n > 32 || br->bit + n > br->len_bits) {
        br->err = 1;
        return 0;
    }
    for (i = 0; i < n; i++) {
        size_t b = br->bit + i;
        v = (v << 1) | ((uint32_t)(br->p[b >> 3] >> (7 - (b & 7))) & 1u);
    }
    br->bit += n;
    return v;
}

static uint32_t br_ue(bitreader *br)
{
    unsigned zeros = 0;

    while (!br->err) {
        uint32_t b = br_u(br, 1);
        if (br->err)
            return 0;
        if (b != 0)
            break;
        if (++zeros > 31) {
            br->err = 1;
            return 0;
        }
    }
    if (br->err)
        return 0;
    return ((uint32_t)1 << zeros) - 1u + br_u(br, zeros);
}

static int32_t br_se(bitreader *br)
{
    uint32_t v = br_ue(br);

    if (br->err)
        return 0;
    if (v & 1u)
        return (int32_t)((v + 1u) >> 1);
    return -(int32_t)(v >> 1);
}

static void skip_scaling_list(bitreader *br, int size)
{
    int last = 8;
    int next = 8;
    int j;

    for (j = 0; j < size && !br->err; j++) {
        if (next != 0) {
            int32_t delta = br_se(br);
            next = (int)((last + delta + 256) % 256);
        }
        if (next != 0)
            last = next;
    }
}

/* ------------------------------------------------------------------------- */
/* SPS parsing                                                                */
/* ------------------------------------------------------------------------- */

static int parse_sps(h264_bits *bits, const uint8_t *nal, size_t len)
{
    bitreader br;
    uint32_t profile;
    uint32_t level;
    uint32_t chroma = 1;
    uint32_t bd_luma = 8;
    uint32_t bd_chroma = 8;
    uint32_t frame_mbs_only = 1;
    uint32_t w_mbs;
    uint32_t h_map;
    uint32_t crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    uint32_t poc_type;
    uint32_t sub_w;
    uint32_t sub_h;
    uint32_t coded_w;
    uint32_t coded_h;
    uint32_t disp_w;
    uint32_t disp_h;

    if (nal == NULL || len < 5)
        return 0;

    /* `nal` points at the NAL unit header (forbidden_zero_bit + nal_ref_idc +
     * nal_unit_type); the RBSP that the syntax below describes starts after
     * it.  AVCDecoderConfigurationRecord stores the SPS with that same header
     * byte, so both callers are consistent. */
    br.p = nal + 1;
    br.len_bits = (len - 1) * 8;
    br.bit = 0;
    br.err = 0;

    profile = br_u(&br, 8);
    (void)br_u(&br, 8);                 /* constraint_set flags + reserved */
    level = br_u(&br, 8);
    (void)br_ue(&br);                   /* seq_parameter_set_id */

    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83:
    case 86: case 118: case 128: case 138: case 139: case 134: case 135:
        chroma = br_ue(&br);
        if (chroma == 3)
            (void)br_u(&br, 1);         /* separate_colour_plane_flag */
        bd_luma = 8u + br_ue(&br);
        bd_chroma = 8u + br_ue(&br);
        (void)br_u(&br, 1);             /* qpprime_y_zero_transform_bypass */
        if (br_u(&br, 1)) {             /* seq_scaling_matrix_present */
            int lists = (chroma != 3) ? 8 : 12;
            int i;
            for (i = 0; i < lists && !br.err; i++) {
                if (br_u(&br, 1))
                    skip_scaling_list(&br, i < 6 ? 16 : 64);
            }
        }
        break;
    default:
        break;
    }

    (void)br_ue(&br);                   /* log2_max_frame_num_minus4 */
    poc_type = br_ue(&br);
    if (poc_type == 0) {
        (void)br_ue(&br);               /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (poc_type == 1) {
        uint32_t n;
        uint32_t i;
        (void)br_u(&br, 1);             /* delta_pic_order_always_zero */
        (void)br_se(&br);               /* offset_for_non_ref_pic */
        (void)br_se(&br);               /* offset_for_top_to_bottom_field */
        n = br_ue(&br);
        if (n > 255) {
            br.err = 1;
            return 0;
        }
        for (i = 0; i < n && !br.err; i++)
            (void)br_se(&br);
    }

    (void)br_ue(&br);                   /* max_num_ref_frames */
    (void)br_u(&br, 1);                 /* gaps_in_frame_num_value_allowed */
    w_mbs = br_ue(&br) + 1u;
    h_map = br_ue(&br) + 1u;
    frame_mbs_only = br_u(&br, 1);
    if (!frame_mbs_only)
        (void)br_u(&br, 1);             /* mb_adaptive_frame_field */
    (void)br_u(&br, 1);                 /* direct_8x8_inference */
    if (br_u(&br, 1)) {                 /* frame_cropping_flag */
        crop_l = br_ue(&br);
        crop_r = br_ue(&br);
        crop_t = br_ue(&br);
        crop_b = br_ue(&br);
    }
    if (br.err)
        return 0;
    if (w_mbs == 0 || w_mbs > 256 || h_map == 0 || h_map > 256)
        return 0;
    if (chroma > 3 || bd_luma != 8 || bd_chroma != 8)
        return 0;

    sub_w = (chroma == 1 || chroma == 2) ? 2u : 1u;
    sub_h = (chroma == 1) ? 2u : 1u;
    coded_w = w_mbs * 16u;
    coded_h = (2u - frame_mbs_only) * h_map * 16u;
    if (crop_l + crop_r > 0xFFFFu || crop_t + crop_b > 0xFFFFu)
        return 0;
    disp_w = coded_w - (crop_l + crop_r) * sub_w;
    disp_h = coded_h - (crop_t + crop_b) * sub_h * (2u - frame_mbs_only);
    if (disp_w == 0 || disp_h == 0 || disp_w > coded_w || disp_h > coded_h)
        return 0;
    /* The driver requires an even geometry and NV12 has 2x2 chroma blocks. */
    disp_w = (disp_w + 1u) & ~1u;
    disp_h = (disp_h + 1u) & ~1u;
    if (disp_w > coded_w)
        disp_w = coded_w;
    if (disp_h > coded_h)
        disp_h = coded_h;
    if (disp_w < 16 || disp_h < 16 || disp_w > 4096 || disp_h > 4096)
        return 0;

    bits->width = disp_w;
    bits->height = disp_h;
    bits->coded_width = coded_w;
    bits->coded_height = coded_h;
    bits->profile_idc = profile;
    bits->level_idc = level;
    bits->chroma_format_idc = chroma;
    bits->bit_depth_luma = bd_luma;
    bits->bit_depth_chroma = bd_chroma;
    bits->frame_mbs_only = frame_mbs_only;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Parameter-set cache                                                        */
/* ------------------------------------------------------------------------- */

static size_t put_annexb_nal(uint8_t *dst, size_t cap, const uint8_t *nal,
                             size_t len)
{
    if (nal == NULL || len == 0 || cap < len + 4)
        return 0;
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = 0;
    dst[3] = 1;
    memcpy(dst + 4, nal, len);
    return len + 4;
}

/* Split the cached config back into SPS and PPS payloads. */
static void split_config(const h264_bits *bits, const uint8_t **sps,
                         size_t *sps_len, const uint8_t **pps, size_t *pps_len)
{
    size_t pos = 0;
    const uint8_t *nal;
    size_t nal_len;

    *sps = NULL;
    *sps_len = 0;
    *pps = NULL;
    *pps_len = 0;
    while (annexb_next(bits->config, bits->config_len, &pos, &nal, &nal_len)) {
        if ((nal[0] & 0x1F) == NAL_SPS && *sps == NULL) {
            *sps = nal;
            *sps_len = nal_len;
        } else if ((nal[0] & 0x1F) == NAL_PPS && *pps == NULL) {
            *pps = nal;
            *pps_len = nal_len;
        }
    }
}

static int rebuild_config(h264_bits *bits, const uint8_t *sps, size_t sps_len,
                          const uint8_t *pps, size_t pps_len)
{
    uint8_t buf[H264_CONFIG_MAX];
    size_t len = 0;
    size_t n;

    if (sps && sps_len) {
        n = put_annexb_nal(buf + len, sizeof(buf) - len, sps, sps_len);
        if (n == 0)
            return 0;
        len += n;
    }
    if (pps && pps_len) {
        n = put_annexb_nal(buf + len, sizeof(buf) - len, pps, pps_len);
        if (n == 0)
            return 0;
        len += n;
    }
    if (len == 0)
        return 0;
    if (len == bits->config_len && memcmp(buf, bits->config, len) == 0)
        return 0;
    memcpy(bits->config, buf, len);
    bits->config_len = (uint32_t)len;
    bits->have_sps = sps && sps_len;
    bits->have_pps = pps && pps_len;
    bits->config_dirty = 1;
    return 1;
}

void h264_bits_init(h264_bits *bits)
{
    if (bits)
        memset(bits, 0, sizeof(*bits));
}

int h264_bits_update(h264_bits *bits, const uint8_t *data, size_t len)
{
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    size_t pos = 0;
    const uint8_t *nal = NULL;
    size_t nal_len = 0;
    int avcc;
    int found_sps = 0;
    int found_pps = 0;

    if (bits == NULL || data == NULL || len == 0)
        return 0;

    /* The cached payloads live inside bits->config, which rebuild_config()
     * does not touch until both scans are finished, so these pointers stay
     * valid for the whole call. */
    split_config(bits, &sps, &sps_len, &pps, &pps_len);

    avcc = h264_bits_is_avcc(data, len);
    while (avcc ? avcc_next(data, len, &pos, &nal, &nal_len)
                : annexb_next(data, len, &pos, &nal, &nal_len)) {
        if (nal_len == 0)
            continue;
        if ((nal[0] & 0x1F) == NAL_SPS) {
            sps = nal;
            sps_len = nal_len;
            found_sps = 1;
        } else if ((nal[0] & 0x1F) == NAL_PPS) {
            pps = nal;
            pps_len = nal_len;
            found_pps = 1;
        }
    }

    if (!found_sps && !found_pps)
        return 0;

    if (found_sps) {
        /* A parameter set we cannot parse is still worth forwarding, but it
         * must not clobber a geometry we already reported. */
        (void)parse_sps(bits, sps, sps_len);
        bits->sps_count++;
    }
    if (found_pps)
        bits->pps_count++;

    return rebuild_config(bits, sps, sps_len, pps, pps_len);
}

int h264_bits_set_sequence_header(h264_bits *bits, const uint8_t *data,
                                  size_t len)
{
    const uint8_t *sps = NULL, *pps = NULL;
    const uint8_t *old_sps = NULL, *old_pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    size_t old_sps_len = 0, old_pps_len = 0;
    size_t pos;

    if (bits == NULL || data == NULL || len < 4)
        return 0;

    split_config(bits, &old_sps, &old_sps_len, &old_pps, &old_pps_len);
    sps = old_sps;
    sps_len = old_sps_len;
    pps = old_pps;
    pps_len = old_pps_len;

    /* AVCDecoderConfigurationRecord (ISO/IEC 14496-15 5.2.4.1). */
    if (data[0] == 1 && len >= 7 && (data[5] & 0xE0) == 0xE0) {
        size_t off = 6;
        unsigned num_sps = data[5] & 0x1F;
        unsigned i;

        for (i = 0; i < num_sps; i++) {
            uint32_t n;
            if (off + 2 > len)
                return 0;
            n = ((uint32_t)data[off] << 8) | data[off + 1];
            off += 2;
            if (n == 0 || off + n > len)
                return 0;
            sps = data + off;
            sps_len = n;
            off += n;
        }
        if (off >= len)
            return 0;
        {
            unsigned num_pps = data[off++];
            for (i = 0; i < num_pps; i++) {
                uint32_t n;
                if (off + 2 > len)
                    return 0;
                n = ((uint32_t)data[off] << 8) | data[off + 1];
                off += 2;
                if (n == 0 || off + n > len)
                    return 0;
                pps = data + off;
                pps_len = n;
                off += n;
            }
        }
        if (sps == old_sps && pps == old_pps)
            return 0;
        if (sps != old_sps) {
            (void)parse_sps(bits, sps, sps_len);
            bits->sps_count++;
        }
        if (pps != old_pps)
            bits->pps_count++;
        return rebuild_config(bits, sps, sps_len, pps, pps_len);
    }

    /* A bare Annex-B (or length-prefixed) SPS/PPS blob. */
    pos = 0;
    {
        const uint8_t *nal = NULL;
        size_t nal_len = 0;
        int changed = 0;
        int once = 0;
        int avcc = h264_bits_is_avcc(data, len);

        while (avcc ? avcc_next(data, len, &pos, &nal, &nal_len)
                    : annexb_next(data, len, &pos, &nal, &nal_len)) {
            if (nal_len == 0)
                continue;
            if ((nal[0] & 0x1F) == NAL_SPS) {
                sps = nal;
                sps_len = nal_len;
                once = 1;
                if (parse_sps(bits, sps, sps_len))
                    bits->sps_count++;
            } else if ((nal[0] & 0x1F) == NAL_PPS) {
                pps = nal;
                pps_len = nal_len;
                once = 1;
                bits->pps_count++;
            }
        }
        if (!once)
            return 0;
        changed = rebuild_config(bits, sps, sps_len, pps, pps_len);
        return changed;
    }
}

/* ------------------------------------------------------------------------- */
/* Access-unit assembly                                                       */
/* ------------------------------------------------------------------------- */

size_t h264_bits_build_au(h264_bits *bits, const uint8_t *in, size_t len,
                          uint8_t *out, size_t out_cap)
{
    size_t total = 0;
    size_t pos = 0;
    const uint8_t *nal = NULL;
    size_t nal_len = 0;
    int avcc;
    int has_sps = 0;
    int has_pps = 0;

    if (bits == NULL || in == NULL || len == 0 || out == NULL)
        return 0;

    avcc = h264_bits_is_avcc(in, len);

    /* Pass 1: inspect the AU for in-band parameter sets. */
    while (avcc ? avcc_next(in, len, &pos, &nal, &nal_len)
                : annexb_next(in, len, &pos, &nal, &nal_len)) {
        if (nal_len == 0)
            continue;
        if ((nal[0] & 0x1F) == NAL_SPS)
            has_sps = 1;
        else if ((nal[0] & 0x1F) == NAL_PPS)
            has_pps = 1;
    }
    pos = 0;

    /* The AU lacks its SPS/PPS: inject the cached pair, exactly like
     * d3d11_video.c does before vm_send(). */
    if ((!has_sps || !has_pps) && bits->config_len != 0) {
        if (bits->config_len > out_cap)
            return 0;
        memcpy(out, bits->config, bits->config_len);
        total = bits->config_len;
    }

    if (!avcc) {
        if (len > out_cap - total)
            return 0;
        memcpy(out + total, in, len);
        total += len;
        return total;
    }

    /* Length-prefixed -> Annex-B.  A 4-byte start code replaces each 4-byte
     * length field, so the converted AU never grows. */
    while (avcc_next(in, len, &pos, &nal, &nal_len)) {
        if (nal_len == 0)
            continue;
        if (total + 4 + nal_len > out_cap)
            return 0;
        out[total++] = 0;
        out[total++] = 0;
        out[total++] = 0;
        out[total++] = 1;
        memcpy(out + total, nal, nal_len);
        total += nal_len;
    }
    return total;
}

int h264_bits_au_is_idr(const uint8_t *in, size_t len)
{
    size_t pos = 0;
    const uint8_t *nal = NULL;
    size_t nal_len = 0;
    int avcc;

    if (in == NULL || len == 0)
        return 0;
    avcc = h264_bits_is_avcc(in, len);
    while (avcc ? avcc_next(in, len, &pos, &nal, &nal_len)
                : annexb_next(in, len, &pos, &nal, &nal_len)) {
        if (nal_len != 0 && (nal[0] & 0x1F) == NAL_IDR)
            return 1;
    }
    return 0;
}
