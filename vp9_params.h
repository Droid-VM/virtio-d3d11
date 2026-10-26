/* VP9 uncompressed-header and superframe parsing for the D3D11 UMD (P-BATCH VP9).
 *
 * Why this file exists
 * --------------------
 * The UMD advertises DXVA_ModeVP9_VLD_Profile0 (D3D11_1DDI VP9 VLD profile 0)
 * next to H.264, and the decode path it already has is a *pass-through* decoder:
 * the host virtio-media decoder is a stateful Android MediaCodec decoder (the
 * tablet's c2.qti.vp9.decoder), so the bytes it needs for a VP9 frame are the
 * VP9 frame bytes themselves -- VP9 has no out-of-band parameter sets that the
 * client could have taken away, unlike H.264 (see h264_params.h: there the SPS
 * and PPS only exist inside DXVA_PicParams_H264 and have to be rebuilt).
 *
 * What is therefore still needed here, and what this header does:
 *
 *   1. Rebuild the access unit from the SLICE_CONTROL records.  A D3D11 VP9
 *      client fills a SLICE_CONTROL buffer with DXVA_Slice_VPx_Short records
 *      (dxva.h:1527 / :685) that point into the BITSTREAM buffer.  The host
 *      decoder is handed one contiguous access unit, so the records have to be
 *      resolved into (offset, size) pairs instead of assuming that the BITSTREAM
 *      buffer is one frame.
 *   2. Split a VP9 *superframe*.  VP9 allows 1..8 frames back to back in one
 *      buffer, delimited by the superframe index in the last byte (VP9 7.2.7 /
 *      libvpx vp9_parse_superframe_index).  The UMD's DecoderEndFrame contract
 *      is one decoded picture per output view, so the members are sent one by
 *      one and the last one is the picture that is written into the texture;
 *      passing the whole superframe to the host as one access unit would queue
 *      extra pictures that a later GetFrame would then deliver to the wrong
 *      DecoderEndFrame.
 *   3. Validate the frame before it is sent: frame marker, profile, sync code
 *      for a key frame, and the coded size of a key frame.  This is what turns
 *      "wrong codec / corrupt AU" into one clear log line instead of a one
 *      second GetFrame timeout.  It also recognises a sequence start (key frame,
 *      or an intra-only inter frame) for the flush rule in DecoderEndFrame.
 *   4. VP9FIX (2026-10-25): bound every length before it is used.  The
 *      superframe index is validated against the access unit with 64-bit
 *      arithmetic, a one-frame "index" is not treated as a split, and every
 *      resulting (offset, size) pair is re-checked by vp9_access_unit_valid()
 *      before the EndFrame loop dereferences it.  A split that does not fit its
 *      access unit is refused by the caller (E_INVALIDARG) instead of being
 *      sent to the host truncated or with a size taken from garbage.
 *
 * Honest limits, deliberately not papered over:
 *   - Only profile 0 (8-bit 4:2:0) is parsed in full, which is exactly the
 *     profile advertised (4:4:4 profile 1, 10/12-bit profile 2/3 are not
 *     advertised and are rejected here with the profile number in the log).
 *   - For an INTER frame only the flags up to intra_only / reset_frame_context
 *     are parsed.  Its coded size comes from `frame_size_with_refs` (VP9 7.2.6),
 *     i.e. from the reference frame slots rather than from bits in this frame,
 *     and DXVA_PicParams_VP9 carries width/height for every frame anyway; the
 *     key frame that starts every sequence is parsed completely and is what the
 *     geometry check and the log use.
 *   - show_existing_frame (a frame with no compressed data at all) is reported
 *     but has no geometry; it is passed through so the host can refresh its
 *     output from a reference slot.  A stream that only uses show_existing_frame
 *     would make the host re-emit a picture, not decode a new one.
 *   - The compressed header (loop filter, quantisation, segmentation, tiles,
 *     `header_size_in_bytes`) is not parsed: the host parses it, the UMD has no
 *     use for it, and a mis-parse there would only add a risk of dropping a
 *     frame that the host would have accepted.
 *
 * Everything here is plain C99 (stdint + string only) so the same code compiles
 * into the UMD (PE32/PE32+ for Windows) and into the Linux unit test that feeds
 * it real libvpx output (tests/vp9_params_test.c).
 */
#ifndef VIRTIO_WDDM_VP9_PARAMS_H
#define VIRTIO_WDDM_VP9_PARAMS_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* VP9 7.2.7: at most 8 frames in a superframe. */
#define VP9_MAX_SUPERFRAME_FRAMES 8u

/* A profile-0 VP9 uncompressed header is a handful of bytes: marker+profile+3
 * flags+24-bit sync code+two 16-bit sizes+2 flags+8 refresh flags = 10 bytes for
 * a key frame, 3 bytes for an inter frame.  64 is a sanity bound only. */
#define VP9_MAX_HEADER_BYTES 64u

/* VP9FIX (2026-10-25): hard bound for one frame of a split access unit.
 * 64 MiB is far above any 8-bit 4:2:0 VP9 frame (a 4K intra frame is ~12 MB),
 * so a larger value can only come from a corrupt/degenerate superframe index;
 * the caller refuses such an access unit instead of handing a bogus size to
 * vm_send()/memcpy().  The whole access unit has its own cap in
 * d3d11_video.c (VIRTIO_WDDM_MAX_ACCESS_UNIT_BYTES). */
#define VP9_MAX_FRAME_BYTES (64u * 1024u * 1024u)

/* frame_type values (VP9 6.2.2). */
#define VP9_FRAME_TYPE_KEY 0u
#define VP9_FRAME_TYPE_INTER 1u

typedef struct {
    unsigned profile;
    unsigned show_existing_frame;
    unsigned frame_to_show_map_idx; /* valid when show_existing_frame != 0 */

    unsigned frame_type; /* VP9_FRAME_TYPE_KEY / _INTER */
    unsigned show_frame;
    unsigned error_resilient_mode;
    unsigned intra_only;        /* only meaningful for an INTER frame */
    unsigned reset_frame_context;
    unsigned refresh_frame_flags;

    unsigned color_space;   /* 3 bits: 0 = CS_UNKNOWN, 1 = BT.601, 2 = BT.709,
                             * 5 = BT.2020, 7 = CS_RGB (VP9 6.2.1) */
    unsigned color_range;   /* 0 = studio/limited, 1 = full */
    unsigned subsampling_x; /* profile 0: 1 */
    unsigned subsampling_y; /* profile 0: 1 */

    /* Coded size from this frame's header.  Only a key frame (or the explicit
     * size of an intra-only inter frame) carries it; geometry_valid says so. */
    unsigned frame_width;
    unsigned frame_height;
    unsigned render_width;
    unsigned render_height;
    unsigned geometry_valid;

    unsigned header_size_bytes; /* whole bytes of *this* header that were read */

    unsigned valid;        /* flags above are trustworthy */
    const char *error;     /* static string when valid == 0, else NULL */
} Vp9FrameHeader;

/* The access unit split into VP9 frames.  count == 1 and superframe == 0 for the
 * normal single-frame case. */
typedef struct {
    unsigned count;
    unsigned superframe;
    unsigned offset[VP9_MAX_SUPERFRAME_FRAMES];
    unsigned size[VP9_MAX_SUPERFRAME_FRAMES];
} Vp9AccessUnit;

/* ------------------------------------------------------------------------- */
/* Implementation (static: this header is included by exactly one TU)         */
/* ------------------------------------------------------------------------- */

/* MSB-first bit reader over one frame.  VP9's uncompressed header is a plain
 * bit string, so this is all the bitstream plumbing the header needs. */
typedef struct {
    const uint8_t *data;
    unsigned size; /* bytes */
    unsigned bit;  /* next bit to read */
    int fail;      /* set when a read ran past the end */
} Vp9BitReader;

static unsigned vp9_bit_read(Vp9BitReader *r, unsigned bits) {
    unsigned value = 0;
    unsigned i;
    for (i = 0; i < bits; i++) {
        unsigned byte = r->bit >> 3;
        if (byte >= r->size) {
            r->fail = 1;
            return 0;
        }
        value = (value << 1) | (unsigned)((r->data[byte] >> (7u - (r->bit & 7u))) & 1u);
        r->bit++;
    }
    return value;
}

/* Parse the uncompressed header of one VP9 frame.  Returns 1 when the frame is a
 * VP9 profile-0 frame whose header was read far enough to trust frame_type /
 * intra_only / (for a key frame) the coded size; 0 otherwise, with hdr->error
 * naming the reason and hdr->profile / hdr->valid still describing what was
 * read.  `size` may be the size of the whole frame: only the header is read. */
static int vp9_parse_uncompressed_header(const uint8_t *data, unsigned size,
                                         Vp9FrameHeader *hdr) {
    Vp9BitReader r;
    unsigned marker;
    unsigned profile_low_bit;
    unsigned profile_high_bit;
    unsigned profile;

    if (!hdr)
        return 0;
    memset(hdr, 0, sizeof(*hdr));
    if (!data || size == 0) {
        hdr->error = "empty frame";
        return 0;
    }

    r.data = data;
    r.size = size;
    r.bit = 0;
    r.fail = 0;

    /* VP9 6.2: frame_marker f(2) == 0b10, then profile_low_bit, profile_high_bit
     * (profile = high << 1 | low).  A non-VP9 payload (H.264 Annex-B starts with
     * 00 00 00 01, AV1 with a 1-bit marker) fails the very first test, which is
     * the cheapest possible "wrong codec / stale buffer" check. */
    marker = vp9_bit_read(&r, 2);
    if (r.fail || marker != 2u) {
        hdr->error = "no VP9 frame marker (top two bits are not 0b10)";
        return 0;
    }
    profile_low_bit = vp9_bit_read(&r, 1);
    profile_high_bit = vp9_bit_read(&r, 1);
    profile = (profile_high_bit << 1) | profile_low_bit;
    hdr->profile = profile;
    if (r.fail) {
        hdr->error = "truncated profile bits";
        return 0;
    }
    if (profile == 3u) {
        /* reserved_zero f(1) must be 0 in profile 3 (VP9 6.2.1). */
        if (vp9_bit_read(&r, 1) != 0u) {
            hdr->error = "profile 3 with reserved_zero = 1";
            return 0;
        }
    }
    if (profile != 0u) {
        hdr->error = "VP9 profile is not 0 (only 8-bit 4:2:0 is advertised)";
        return 0;
    }

    hdr->show_existing_frame = vp9_bit_read(&r, 1);
    if (r.fail) {
        hdr->error = "truncated show_existing_frame";
        return 0;
    }
    if (hdr->show_existing_frame != 0u) {
        /* VP9 6.2.2: nothing follows but the slot to show.  The host re-emits
         * the picture that is already in that reference slot; there is no coded
         * size here, so geometry_valid stays 0. */
        hdr->frame_to_show_map_idx = vp9_bit_read(&r, 3);
        hdr->valid = !r.fail;
        hdr->header_size_bytes = (r.bit + 7u) / 8u;
        if (r.fail)
            hdr->error = "truncated show_existing_frame index";
        return hdr->valid;
    }

    hdr->frame_type = vp9_bit_read(&r, 1);
    hdr->show_frame = vp9_bit_read(&r, 1);
    hdr->error_resilient_mode = vp9_bit_read(&r, 1);
    if (r.fail) {
        hdr->error = "truncated frame type / show_frame / error_resilient_mode";
        return 0;
    }

    if (hdr->frame_type == VP9_FRAME_TYPE_KEY) {
        unsigned sync_code;

        /* VP9 6.2.3: frame_sync_code f(24) == 0x498342. */
        sync_code = vp9_bit_read(&r, 24);
        if (r.fail) {
            hdr->error = "truncated key-frame sync code";
            return 0;
        }
        if (sync_code != 0x498342u) {
            hdr->error = "bad key-frame sync code (not 0x498342)";
            return 0;
        }

        /* VP9 6.2.1: color_config() follows the sync code for a key frame.
         * Empirically (and contrary to a common reading of the spec) it is
         * present for profile 0 as well: libvpx 1.14 emits color_space f(3) and
         * color_range f(1) here, verified by encoding the same content with
         * -colorspace bt709/bt470bg/bt2020 (the four bits became 0100/0011/1010),
         * so the parser must read them or every field after them is shifted by
         * four bits.  The subsampling_x/y/reserved_zero bits exist only for
         * profiles 1 and 3, which this decoder does not advertise. */
        hdr->color_space = vp9_bit_read(&r, 3);
        hdr->color_range = vp9_bit_read(&r, 1);
        hdr->subsampling_x = 1;
        hdr->subsampling_y = 1;
        if (r.fail) {
            hdr->error = "truncated key-frame color_config";
            return 0;
        }

        hdr->frame_width = vp9_bit_read(&r, 16) + 1u;
        hdr->frame_height = vp9_bit_read(&r, 16) + 1u;
        if (r.fail) {
            hdr->error = "truncated key-frame frame_size";
            return 0;
        }
        if (vp9_bit_read(&r, 1) != 0u) { /* render_and_frame_size_different */
            hdr->render_width = vp9_bit_read(&r, 16) + 1u;
            hdr->render_height = vp9_bit_read(&r, 16) + 1u;
            if (r.fail) {
                hdr->error = "truncated key-frame render_size";
                hdr->geometry_valid = 1; /* the coded size is still known */
                return 0;
            }
        } else {
            hdr->render_width = hdr->frame_width;
            hdr->render_height = hdr->frame_height;
        }
        hdr->geometry_valid = 1;

        /* VP9 6.2.3: refresh_frame_flags f(8) is present only when the frame is
         * not error resilient; an error-resilient key frame refreshes all 8. */
        hdr->refresh_frame_flags =
            hdr->error_resilient_mode ? 0xFFu : vp9_bit_read(&r, 8);
        hdr->valid = !r.fail;
        hdr->header_size_bytes = (r.bit + 7u) / 8u;
        if (r.fail) {
            hdr->error = "truncated key-frame refresh_frame_flags";
            return 0;
        }
        return 1;
    }

    /* INTER frame.  intra_only is coded only when the frame is not shown;
     * reset_frame_context only when it is not error resilient (VP9 6.2.4).  The
     * coded size is inherited through frame_size_with_refs and is deliberately
     * not parsed here (see the file comment). */
    hdr->intra_only = hdr->show_frame ? 0u : vp9_bit_read(&r, 1);
    hdr->reset_frame_context =
        hdr->error_resilient_mode ? 0u : vp9_bit_read(&r, 2);
    hdr->refresh_frame_flags = hdr->intra_only ? 0xFFu : 0u;
    hdr->valid = !r.fail;
    hdr->header_size_bytes = (r.bit + 7u) / 8u;
    if (r.fail) {
        hdr->error = "truncated inter-frame flags";
        return 0;
    }
    return 1;
}

/* TRUE when the frame starts (or restarts) the sequence, i.e. it is coded
 * without reference to an earlier picture: INTRA_ONLY or KEY_FRAME.  Used for
 * the same purpose as decoder_au_has_idr() on the H.264 path. */
static int vp9_frame_is_intra(const Vp9FrameHeader *hdr) {
    if (!hdr || !hdr->valid || hdr->show_existing_frame)
        return 0;
    return hdr->frame_type == VP9_FRAME_TYPE_KEY || hdr->intra_only != 0u;
}

/* Split one access unit into its VP9 frames using the superframe index
 * (VP9 7.2.7, libvpx vp9_parse_superframe_index).
 *
 * The index is the last `2 + mag * (frames - 1)` bytes of the buffer:
 *   [0]              marker byte, 0b110 in the top three bits
 *   [1 .. ]          (frames-1) little-endian `mag`-byte frame sizes
 *   [index_size - 1] the same marker byte again
 * marker & 7 = frames - 1, (marker >> 3) & 3 = mag - 1.
 *
 * When the trailing byte is not a superframe marker, or the index does not
 * self-validate, the whole buffer is reported as one frame: that is the normal
 * case for a single VP9 frame, and a false negative here only means "one send
 * with everything in it", which the host rejects or decodes -- it never splits a
 * frame on garbage. */
static void vp9_parse_superframe(const uint8_t *au, unsigned size,
                                 Vp9AccessUnit *out) {
    unsigned frames;
    unsigned magnitude;
    unsigned index_size;
    unsigned index_at;
    unsigned i;
    unsigned consumed;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->count = 1;
    out->size[0] = size;
    if (!au || size < 2u)
        return;

    if ((au[size - 1] & 0xE0u) != 0xC0u)
        return; /* not a superframe */

    frames = (unsigned)(au[size - 1] & 0x07u) + 1u;
    magnitude = (unsigned)((au[size - 1] >> 3) & 0x03u) + 1u;
    /* VP9FIX: frames == 1 means frames_in_superframe_minus_1 == 0, i.e. an
     * index that carries no frame sizes at all (index_size would be 2).  It
     * cannot describe a split, and treating the two marker bytes as an index
     * would drop them from the picture (a payload that happens to end in
     * 0xC0..0xE7 followed by the same byte is not distinguishable from it).
     * The safe reading is "one frame": the host parses the payload anyway. */
    if (frames < 2u)
        return;
    index_size = 2u + magnitude * (frames - 1u);
    if (frames > VP9_MAX_SUPERFRAME_FRAMES || size < index_size ||
        au[size - index_size] != au[size - 1])
        return; /* marker byte without a matching index: treat as one frame */

    index_at = size - index_size;
    consumed = 0;
    for (i = 0; i + 1u < frames; i++) {
        unsigned k;
        unsigned frame_size = 0;
        for (k = 0; k < magnitude; k++)
            frame_size |= (unsigned)au[index_at + 1u + i * magnitude + k] << (8u * k);
        /* VP9FIX: compare in 64 bits.  frame_size comes from the bitstream and
         * can be up to 2^32-1, so `consumed + frame_size` could wrap in 32 bits
         * and let a bogus size through; VP9_MAX_FRAME_BYTES rejects the rest. */
        if (frame_size == 0 || frame_size > VP9_MAX_FRAME_BYTES ||
            (uint64_t)consumed + (uint64_t)frame_size > (uint64_t)index_at)
            return; /* inconsistent index: do not split on it */
        out->offset[i] = consumed;
        out->size[i] = frame_size;
        consumed += frame_size;
    }
    if (consumed >= index_at)
        return;
    out->offset[frames - 1u] = consumed;
    out->size[frames - 1u] = index_at - consumed;
    out->count = frames;
    out->superframe = 1;
}

/* VP9FIX (2026-10-25): final gate between the superframe parser and the
 * EndFrame loop that dereferences `offset[]`/`size[]`.
 *
 * vp9_parse_superframe() already validates as it parses, but the caller must
 * not depend on that: the arrays are read with an index taken from `count`, and
 * `offset + size` becomes a pointer and a length for vm_send().  This checks,
 * with no 32-bit wrap:
 *   - count in [1, 8] (the size of both arrays), and `superframe` consistent
 *     with it (1 frame is never a split),
 *   - every member non-empty and no larger than VP9_MAX_FRAME_BYTES,
 *   - 0 <= offset <= au_size and offset + size <= au_size.
 * Returns 1 when the whole split is usable, 0 otherwise (the caller refuses the
 * frame; a partially valid split must not be sent because the host would decode
 * a truncated picture). */
static int vp9_access_unit_valid(const Vp9AccessUnit *unit, unsigned au_size) {
    unsigned i;

    if (!unit)
        return 0;
    if (unit->count == 0 || unit->count > VP9_MAX_SUPERFRAME_FRAMES)
        return 0;
    if (unit->superframe != 0u) {
        if (unit->count < 2u)
            return 0;
    } else if (unit->count != 1u) {
        return 0;
    }
    for (i = 0; i < unit->count; i++) {
        unsigned offset = unit->offset[i];
        unsigned size = unit->size[i];
        if (size == 0 || size > VP9_MAX_FRAME_BYTES)
            return 0;
        if (offset > au_size || size > au_size - offset)
            return 0;
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* VIRTIO_WDDM_VP9_PARAMS_H */
