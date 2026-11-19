/* h264_bits.h - H.264 access-unit normalisation for the DroidVM decoder MFT.
 *
 * The MFT sits between Media Foundation (which hands out one compressed sample
 * per access unit) and the virtio-media driver (VM_IOCTL_SEND_BITSTREAM, which
 * wants one Annex-B access unit with the SPS/PPS in band).  Media Foundation
 * sources can deliver either
 *
 *   - Annex-B byte streams (MFVideoFormat_H264): 00 00 01 / 00 00 00 01
 *     start codes, SPS/PPS usually in band before every IDR, or only once;
 *   - length-prefixed AVC samples (MFVideoFormat_H264_ES): 4-byte big-endian
 *     NAL lengths and an AVCDecoderConfigurationRecord in
 *     MF_MT_MPEG_SEQUENCE_HEADER.
 *
 * This module owns the three things the driver session needs:
 *   1. remember the newest SPS/PPS seen in the stream (or in the sequence
 *      header) so it can be re-injected when an AU arrives without them --
 *      exactly what d3d11_video.c does with its `params` buffer;
 *   2. turn a length-prefixed sample into Annex-B;
 *   3. parse the SPS for the coded and the cropped (display) geometry, because
 *      the output media type has to be created before the first AU is decoded.
 *
 * It is plain C with no Windows dependency on purpose: `tests/h264_bits_test.c`
 * compiles it with the host gcc and runs on the build server, so the bitstream
 * logic is verified without a Windows machine.
 *
 * Limitations (documented in P-BATCH-MFT.md):
 *   - 8-bit 4:2:0 only, matching what the driver's NV12 capture path produces;
 *   - only the newest SPS and the newest PPS are kept, which is all a decoder
 *     needs when the stream does not switch parameter sets inside an AU;
 *   - VUI is not parsed (no HRD / aspect-ratio passthrough).
 */
#ifndef DROIDVM_H264_BITS_H
#define DROIDVM_H264_BITS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The driver refuses an AU larger than the OUTPUT sizeimage it negotiated and
 * the host sizeimage for 1080p is 3,133,440 bytes, so 64 KiB of SPS/PPS cache
 * is far more than any legal parameter set needs. */
#define H264_CONFIG_MAX 1024

typedef struct h264_bits {
    /* Annex-B SPS/PPS, each prefixed with a 4-byte start code. */
    uint8_t  config[H264_CONFIG_MAX];
    uint32_t config_len;
    int      have_sps;
    int      have_pps;
    int      config_dirty;   /* config changed since the last build_au() */

    /* SPS-derived geometry.  `coded_*` is macroblock aligned (1920x1088 for
     * 1080p), `width`/`height` is the cropped display area (1920x1080).  0
     * means "not known yet". */
    uint32_t width;
    uint32_t height;
    uint32_t coded_width;
    uint32_t coded_height;

    uint32_t profile_idc;
    uint32_t level_idc;
    uint32_t chroma_format_idc;
    uint32_t bit_depth_luma;
    uint32_t bit_depth_chroma;
    uint32_t frame_mbs_only;

    uint32_t sps_count;      /* parameter sets accepted (diagnostics) */
    uint32_t pps_count;
} h264_bits;

void h264_bits_init(h264_bits *bits);

/* Scan `data` for NAL units and remember the newest SPS/PPS.  Accepts both
 * Annex-B and length-prefixed input.  Returns 1 if the cached config changed. */
int h264_bits_update(h264_bits *bits, const uint8_t *data, size_t len);

/* Feed a codec-private blob (MF_MT_MPEG_SEQUENCE_HEADER).  Handles both an
 * AVCDecoderConfigurationRecord and a raw Annex-B SPS/PPS pair.  Returns 1 if
 * the cached config changed. */
int h264_bits_set_sequence_header(h264_bits *bits, const uint8_t *data, size_t len);

/* 1 if `data` looks like a 4-byte-length-prefixed AVC sample rather than an
 * Annex-B byte stream. */
int h264_bits_is_avcc(const uint8_t *data, size_t len);

/* Build the Annex-B access unit that is handed to VM_IOCTL_SEND_BITSTREAM.
 * Converts length-prefixed input to start codes and prepends the cached SPS/PPS
 * when the AU carries neither.  Returns the number of bytes written to `out`,
 * or 0 if `out` is too small / the AU is empty.  `out` may not alias `in`. */
size_t h264_bits_build_au(h264_bits *bits, const uint8_t *in, size_t len,
                          uint8_t *out, size_t out_cap);

/* 1 when the access unit contains an IDR slice (NAL type 5). */
int h264_bits_au_is_idr(const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DROIDVM_H264_BITS_H */
