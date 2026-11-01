/*
 * av1_dpb.h -- the STATEFUL AV1 access-unit builder (DPB + one-frame lookahead).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef VIRTIO_WDDM_AV1_DPB_H
#define VIRTIO_WDDM_AV1_DPB_H

/*
 * PORT OF THE STATEFUL REFERENCE DECODER'S AV1 BITSTREAM BUILDER
 * =============================================================
 *
 * Upstream: Droid-VM/libva-v4l2 branch wip/vpu, src/stateful/av1_bitstream.{h,cc}
 * (1613 lines), HEAD e92baea.  That builder exists because a VA-API AV1 client
 * hands a stateful decoder the ALREADY-PARSED sequence/frame headers plus the raw
 * tile payloads, while a V4L2 stateful decoder (c2.qti.av1.decoder) wants the
 * original temporal-unit OBU stream -- AND because two fields the client cannot
 * supply (refresh_frame_flags, the global-motion base) are DERIVABLE from the
 * NEXT picture's reference map.  Hence:
 *
 *   Av1Dpb / Av1Dpb::Slot          <-> Av1Dpb (av1_params.h: texture/order_hint/gm)
 *   derive_refresh_frame_flags()   <-> av1_derive_refresh_flags()
 *   choose_refresh_flags()         <-> av1_choose_refresh_flags()
 *   update_dpb()                   <-> av1_dpb_update()
 *   submit_picture(tag)            <-> av1_builder_submit()
 *   flush()                        <-> av1_builder_flush()
 *   finish()                       <-> av1_builder_finish()
 *   assemble_au()                  <-> av1_ab_assemble()
 *   pending_ / provisional_        <-> pending / provisional held picture
 *   catch_up_                      <-> the hidden re-send of a provisional frame
 *   build_frame_header()'s ref remap <-> av1_frame_prepare()
 *
 * WHAT IS DIFFERENT FROM THE REFERENCE (each deliberate; see the difference
 * list in 04-测试记录/45-AV1移植DPB.md):
 *
 *  1. ONE PICTURE PER HOST INPUT.  The reference splices its catch-up frame into
 *     the NEXT temporal unit (a bare OBU inside that unit) because a V4L2
 *     decoder may carry several coded frames per input as long as one of them is
 *     shown.  Our host refuses that: round 26 measured "[v4lav1D_438]
 *     Unsupported input buffer" for every unit describing two pictures, and the
 *     picture the client asked for was lost with it.  So a catch-up is its own
 *     temporal unit sent as its own input, flagged "produces no picture" (the
 *     VP9 hidden-frame rule from round 25).
 *
 *  2. THE TAG IS OPAQUE.  The reference tags a deferred frame with the caller's
 *     per-frame `tag` (its surface already bound to that sequence); we carry the
 *     same tag as an unsigned -- the DDI stores the output-view snapshot under
 *     it -- and hand it back on every ReadyFrame.
 *
 *  3. flush() DOES NOT KEEP provisional_.  The reference keeps it so that a
 *     later submit_picture() can still catch it up (a client that syncs a frame
 *     before decoding the next).  Our flush() runs at EOS / seek / close / error
 *     and every one of those is followed by a host session reset, so the DPB is
 *     gone and a catch-up would be meaningless: the frame is emitted with mask 0
 *     and dropped.
 *
 *  4. allow_defer IS A CALLER DECISION.  The reference can always defer.  Our
 *     DDI cannot while the pipeline is off: EndFrame must hand the client a
 *     decoded picture before it returns.  The serial path therefore emits the
 *     picture NOW with refresh_frame_flags = 0 (the only mask that cannot evict a
 *     live picture) and keeps it as `provisional` -- exactly the mechanism the
 *     reference uses in flush() -- and the next submit derives its true mask and
 *     re-sends it hidden.
 *
 *  5. The sequence header is deduped on its BYTES (reference: last_emitted_
 *     sequence_header_), but a key frame does NOT force a re-emit; only a
 *     session reset (av1_builder_reset) does.  Our old path re-emitted it on
 *     every key frame.
 */

#include "av1_params.h"

/* The reference's Av1AccessUnitBuilder::ReadyFrame.  `au` points into the
 * builder's staging buffer and stays valid until the next builder call. */
typedef struct {
    unsigned tag;                 /* caller's per-picture tag (output view)    */
    const uint8_t *au;            /* the assembled temporal unit               */
    unsigned au_size;
    unsigned refresh_frame_flags; /* the mask this AU was coded with           */
    unsigned order_hint;
    unsigned shows_picture;       /* 0 = hidden catch-up: no CAPTURE expected  */
    unsigned width, height;       /* coded geometry the host needs (S_FMT)     */
} Av1ReadyFrame;

/* One picture held across a boundary (the reference's PendingFrame). */
typedef struct {
    Av1SeqParams seq;
    Av1FrameParams frame; /* as submitted; refs are remapped at build time     */
    Av1Tile tiles[AV1_MAX_TILES];
    unsigned ntiles;
    uint8_t *data; /* OWN copy of the client's tile payload region          */
    unsigned data_size, data_cap;
    unsigned width, height;
    unsigned tag;
    int valid;
} Av1HeldPicture;

typedef struct {
    /* ---- the accumulated picture (reference: picture_/tile_params_/tile_data_) */
    Av1FrameParams frame;
    Av1SeqParams seq;
    Av1Tile tiles[AV1_MAX_TILES];
    unsigned ntiles;
    uint8_t *cur; /* own copy of this picture's bitstream (tile offsets stay) */
    unsigned cur_size, cur_cap;
    unsigned width, height;
    int has_picture;

    /* ---- decoder state mirrored across frames */
    Av1Dpb dpb;
    int have_sequence;
    uint8_t last_seq[1024]; /* the sequence_header OBU last emitted             */
    unsigned last_seq_size;

    /* ---- the two boundaries; at most one of them is set (see the header) */
    Av1HeldPicture pending;     /* held for the one-frame lookahead            */
    Av1HeldPicture provisional; /* already emitted with mask 0; its exact mask
                                 * is known once a successor arrives          */

    /* ---- staging for the AUs returned by submit/flush */
    uint8_t *stage;
    unsigned stage_cap, stage_used;

    /* ---- statistics (logged by the DDI) */
    unsigned n_submit, n_key, n_deferred, n_provisional, n_catchup, n_flushed,
        n_failed;
} Av1Builder;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

static inline void av1_held_release(Av1HeldPicture *p) {
    p->valid = 0;
    p->data_size = 0;
    p->tag = 0;
}

static inline void av1_builder_free(Av1Builder *b) {
    if (b->cur)
        HeapFree(GetProcessHeap(), 0, b->cur);
    if (b->pending.data)
        HeapFree(GetProcessHeap(), 0, b->pending.data);
    if (b->provisional.data)
        HeapFree(GetProcessHeap(), 0, b->provisional.data);
    if (b->stage)
        HeapFree(GetProcessHeap(), 0, b->stage);
    memset(b, 0, sizeof(*b));
}

/* Forget the DECODER (new session / seek / close): the host DPB is gone, the
 * held pictures are not ours to keep, and the sequence header must be re-emitted
 * to whatever session comes next. */
static inline void av1_builder_reset(Av1Builder *b) {
    av1_dpb_reset(&b->dpb);
    b->have_sequence = 0;
    b->last_seq_size = 0;
    av1_held_release(&b->pending);
    av1_held_release(&b->provisional);
    b->has_picture = 0;
    b->cur_size = 0;
    b->stage_used = 0;
}

/* ------------------------------------------------------------------------- */
/* staging: every AU a single submit()/flush() returns must live in ONE buffer  */
/* that is never reallocated underneath the pointers already handed out, so the  */
/* caller reserves the total up front and emits into it.                        */
/* ------------------------------------------------------------------------- */

static inline int av1_builder_reserve(Av1Builder *b, unsigned need) {
    unsigned cap;
    uint8_t *p;

    if (need <= b->stage_cap)
        return 1;
    cap = b->stage_cap ? b->stage_cap : 16384u;
    while (cap < need)
        cap *= 2u;
    p = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, cap);
    if (!p)
        return 0;
    if (b->stage)
        HeapFree(GetProcessHeap(), 0, b->stage);
    b->stage = p;
    b->stage_cap = cap;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* AU assembly (reference: Av1AccessUnitBuilder::assemble_au)                  */
/* ------------------------------------------------------------------------- */

/* temporal_delimiter [+ sequence_header when changed] + frame_header + tile_group
 * for one held picture with an explicit refresh mask.
 *
 * `shown` is the show_frame the AU is coded with: 1 for every picture the host
 * must emit, 0 for a hidden DPB catch-up (which must NOT produce a second
 * CAPTURE buffer).  A shown key frame implies refresh_frame_flags = 0xFF, so the
 * effective mask goes back through *effective_refresh for update_dpb().
 *
 * Returns the AU size, or 0 when the picture cannot be re-synthesised. */
static inline unsigned av1_ab_assemble(Av1Builder *b, const Av1HeldPicture *p,
                                       unsigned refresh, int shown, uint8_t *out,
                                       unsigned cap,
                                       unsigned *effective_refresh) {
    Av1FrameParams f = p->frame;
    uint8_t seq_obu[1024];
    unsigned at = 0, n;

    if (!out || cap < 64u)
        return 0;

    f.show_frame = shown ? 1u : 0u;
    /* 5.9.2: showable_frame is only CODED when show_frame == 0. */
    f.showable_frame = shown ? 0u : 1u;
    /* Resolve the client's reference slots against OUR DPB before anything reads
     * them (reference: build_frame_header()'s ref_frame_idx remap). */
    av1_frame_prepare(&b->dpb, &f);
    if (f.frame_type == AV1_FRAME_KEY && shown)
        refresh = 0xFFu;
    if (effective_refresh)
        *effective_refresh = refresh;

    /* 1. temporal_delimiter OBU. */
    n = av1_temporal_delimiter_obu(out, cap);
    if (n == 0)
        return 0;
    at += n;

    /* 2. sequence_header OBU, deduped on its bytes.  A session reset clears
     *    have_sequence, so the first AU after a seek re-emits it. */
    n = av1_sequence_header_obu(&p->seq, seq_obu, (unsigned)sizeof(seq_obu));
    if (n == 0)
        return 0;
    if (!b->have_sequence || n != b->last_seq_size ||
        memcmp(seq_obu, b->last_seq, n) != 0) {
        if (cap - at < n)
            return 0;
        memcpy(out + at, seq_obu, n);
        at += n;
        memcpy(b->last_seq, seq_obu, n);
        b->last_seq_size = n;
        b->have_sequence = 1;
    }

    /* 3. frame header + tile group.  One tile: a single frame_obu (type 6), which
     *    is what the encoder emits and what the source stream carries (round 26).
     *    Several tiles: a frame_header OBU carrying tile_size_bytes of THIS
     *    frame's records, then the tile_group OBU. */
    if (p->ntiles <= 1) {
        Av1Tile one;
        const Av1Tile *tiles = p->tiles;

        if (p->ntiles == 0) {
            one.offset = 0;
            one.size = p->data_size;
            one.row = 0;
            one.column = 0;
            tiles = &one;
        }
        n = av1_frame_obu(&p->seq, &f, &b->dpb, tiles, 1, p->data, p->data_size,
                          refresh, out + at, cap - at);
        if (n == 0)
            return 0;
        at += n;
    } else {
        n = av1_frame_header_obu(&p->seq, &f, &b->dpb, p->tiles, p->ntiles,
                                 refresh, out + at, cap - at);
        if (n == 0)
            return 0;
        at += n;
        n = av1_tile_group_obu(&p->seq, &f, p->tiles, p->ntiles, p->data,
                               p->data_size, out + at, cap - at);
        if (n == 0)
            return 0;
        at += n;
    }
    return at;
}

/* Build one ReadyFrame for `p` and mirror the decoder's decode_frame_wrapup.
 * The staging space must already be reserved (see av1_builder_reserve). */
static inline unsigned av1_ab_emit(Av1Builder *b, const Av1HeldPicture *p,
                                   unsigned refresh, int shown, Av1ReadyFrame *rf) {
    uint8_t *out;
    unsigned eff = refresh, size, room;

    if (b->stage_used > b->stage_cap)
        return 0;
    room = b->stage_cap - b->stage_used;
    if (room < p->data_size + 8192u)
        return 0; /* the caller's reservation was too small: refuse, never
                   * reallocate underneath the pointers already returned */
    out = b->stage + b->stage_used;
    size = av1_ab_assemble(b, p, refresh, shown, out, room, &eff);
    if (size == 0)
        return 0;
    b->stage_used += size;

    memset(rf, 0, sizeof(*rf));
    rf->tag = p->tag;
    rf->au = out;
    rf->au_size = size;
    rf->refresh_frame_flags = eff;
    rf->order_hint = p->frame.order_hint;
    rf->shows_picture = shown ? 1u : 0u;
    rf->width = p->width;
    rf->height = p->height;

    /* decode_frame_wrapup: the DPB this frame leaves behind.  `shown` decides the
     * key-frame reset, so code it into the copy the DPB stores. */
    {
        Av1FrameParams f = p->frame;

        f.show_frame = shown ? 1u : 0u;
        av1_dpb_update(&b->dpb, &f, eff);
    }
    return size;
}

/* Copy the accumulator into a held picture (the reference's
 * `pf.pic = picture_; pf.tiles = tile_params_; pf.data = tile_data_;`). */
static inline int av1_held_copy(Av1HeldPicture *p, const Av1Builder *b,
                                unsigned tag) {
    if (p->data_cap < b->cur_size) {
        uint8_t *n = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, b->cur_size);

        if (!n)
            return 0;
        if (p->data)
            HeapFree(GetProcessHeap(), 0, p->data);
        p->data = n;
        p->data_cap = b->cur_size;
    }
    memcpy(p->data, b->cur, b->cur_size);
    p->data_size = b->cur_size;
    p->seq = b->seq;
    p->frame = b->frame;
    memcpy(p->tiles, b->tiles, sizeof(b->tiles[0]) * b->ntiles);
    p->ntiles = b->ntiles;
    p->width = b->width;
    p->height = b->height;
    p->tag = tag;
    p->valid = 1;
    return 1;
}

/* A ReadyFrame view of the accumulator itself (used for the immediate paths). */
static inline void av1_held_view(Av1HeldPicture *p, const Av1Builder *b,
                                 unsigned tag) {
    p->seq = b->seq;
    p->frame = b->frame;
    memcpy(p->tiles, b->tiles, sizeof(b->tiles[0]) * b->ntiles);
    p->ntiles = b->ntiles;
    p->data = b->cur;
    p->data_size = b->cur_size;
    p->data_cap = 0;
    p->width = b->width;
    p->height = b->height;
    p->tag = tag;
    p->valid = 1;
}

/* ------------------------------------------------------------------------- */
/* the accumulator (reference: set_picture_parameters / add_tile_parameters /  */
/* add_tile_data, fused because DXVA hands us the BITSTREAM buffer and the tile */
/* records together)                                                          */
/* ------------------------------------------------------------------------- */

/* Returns 1 when the picture was accepted.  The tile payloads are COPIED: a
 * deferred picture is assembled while the client's BITSTREAM buffer already
 * carries the NEXT access unit. */
static inline int av1_builder_begin_picture(Av1Builder *b,
                                            const Av1SeqParams *seq,
                                            const Av1FrameParams *frame,
                                            const Av1Tile *tiles, unsigned ntiles,
                                            const uint8_t *bitstream,
                                            unsigned bitstream_size,
                                            unsigned width, unsigned height) {
    if (!seq || !frame || !tiles || ntiles == 0 || ntiles > AV1_MAX_TILES ||
        !bitstream || bitstream_size == 0)
        return 0;
    if (b->cur_cap < bitstream_size) {
        uint8_t *p = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, bitstream_size);

        if (!p)
            return 0;
        if (b->cur)
            HeapFree(GetProcessHeap(), 0, b->cur);
        b->cur = p;
        b->cur_cap = bitstream_size;
    }
    memcpy(b->cur, bitstream, bitstream_size);
    b->cur_size = bitstream_size;
    b->seq = *seq;
    b->frame = *frame;
    memcpy(b->tiles, tiles, sizeof(tiles[0]) * ntiles);
    b->ntiles = ntiles;
    b->width = width;
    b->height = height;
    b->has_picture = 1;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* submit_picture / flush / finish                                            */
/* ------------------------------------------------------------------------- */

/* How much staging this submit() can need: one AU per picture it may emit. */
static inline unsigned av1_builder_need(const Av1Builder *b) {
    unsigned need = 0;

    if (b->has_picture)
        need += b->cur_size + 8192u;
    if (b->pending.valid)
        need += b->pending.data_size + 8192u;
    if (b->provisional.valid)
        need += b->provisional.data_size + 8192u;
    return need;
}

/* Accept the accumulated picture and resolve the one-frame lookahead.
 *
 * Returns the number of ReadyFrames written to `out` -- 0..3, in DECODE ORDER:
 *   - a hidden DPB catch-up for a picture an earlier submit() had to emit
 *     provisionally (produces no picture),
 *   - the frame held by the previous submit(), now exact because THIS picture's
 *     reference map reveals the slots it refreshed, and
 *   - this picture itself when it needs no lookahead (a shown key frame).
 *
 * 0 is a legitimate, expected outcome ("this picture is held").  That is the
 * whole point of the port: one AU in must NOT mean one picture out.
 *
 * allow_defer == 0 is the DDI's serial path: the client waits for a picture, so
 * the frame is emitted NOW with refresh_frame_flags = 0 and kept as
 * `provisional`; the next submit derives its true mask and re-sends it hidden. */
static inline unsigned av1_builder_submit(Av1Builder *b, unsigned tag,
                                          int allow_defer, Av1ReadyFrame *out,
                                          unsigned max_out) {
    unsigned n = 0;

    if (!out || max_out == 0)
        return 0;
    b->n_submit++;
    b->stage_used = 0;
    if (!av1_builder_reserve(b, av1_builder_need(b)))
        return 0;

    /* 0. A picture emitted provisionally: this picture's map says which slots the
     *    encoder put it in.  Re-send it HIDDEN with that exact mask so the host
     *    lands it where the encoder did (identical bytes -> identical picture and
     *    CDFs, but no second CAPTURE buffer). */
    if (b->provisional.valid) {
        if (b->has_picture && n < max_out) {
            unsigned refresh = av1_derive_refresh_flags(
                b->provisional.frame.texture & 0xFFu, b->frame.ref_texture);
            int key_resets = (b->frame.frame_type == AV1_FRAME_KEY);

            if (refresh != 0 && !key_resets) {
                if (av1_ab_emit(b, &b->provisional, refresh, /*shown=*/0,
                                &out[n])
                    != 0) {
                    out[n].shows_picture = 0;
                    out[n].tag = 0; /* no picture to route */
                    n++;
                    b->n_catchup++;
                } else {
                    b->n_failed++;
                }
            }
        }
        av1_held_release(&b->provisional);
    }

    /* 1. The frame held for the lookahead: THIS picture's reference map is the
     *    DPB after that frame updated it, so it pins the mask down exactly. */
    if (b->pending.valid) {
        if (n < max_out) {
            unsigned refresh =
                b->has_picture
                    ? av1_derive_refresh_flags(b->pending.frame.texture & 0xFFu,
                                               b->frame.ref_texture)
                    : av1_choose_refresh_flags(&b->dpb, &b->pending.frame);

            if (av1_ab_emit(b, &b->pending, refresh, /*shown=*/1, &out[n]) != 0)
                n++;
            else
                b->n_failed++;
        }
        av1_held_release(&b->pending);
    }

    if (!b->has_picture)
        return n;

    /* 2. This picture, when it needs no successor. */
    if (b->frame.frame_type == AV1_FRAME_KEY) {
        Av1HeldPicture now;
        unsigned refresh = b->frame.show_frame
                               ? 0xFFu
                               : av1_choose_refresh_flags(&b->dpb, &b->frame);

        b->n_key++;
        if (n < max_out) {
            av1_held_view(&now, b, tag);
            if (av1_ab_emit(b, &now, refresh, /*shown=*/1, &out[n]) != 0)
                n++;
            else
                b->n_failed++;
        }
        b->has_picture = 0;
        b->cur_size = 0;
        return n;
    }

    /* 3. Inter / intra-only.  Defer (async path) or emit provisionally (serial
     *    path: the client is waiting for this picture right now). */
    if (allow_defer && n < max_out) {
        if (av1_held_copy(&b->pending, b, tag)) {
            b->has_picture = 0;
            b->cur_size = 0;
            b->n_deferred++;
            return n;
        }
        /* fall through: without a copy the picture must still go out */
    }

    if (n < max_out) {
        Av1HeldPicture now;

        av1_held_view(&now, b, tag);
        /* mask 0: the only choice that cannot evict a live picture. */
        if (av1_ab_emit(b, &now, 0, /*shown=*/1, &out[n]) != 0) {
            n++;
            b->n_provisional++;
            if (!av1_held_copy(&b->provisional, b, tag))
                av1_held_release(&b->provisional);
        } else {
            b->n_failed++;
        }
    }
    b->has_picture = 0;
    b->cur_size = 0;
    return n;
}

/* Emit a still-held frame with refresh_frame_flags = 0 (EOS / seek / close /
 * error).  There is no successor any more, so the true mask is unreachable and 0
 * is the only non-destructive answer.  Difference 3 in the header: the
 * reference keeps the picture as provisional_ so a later submit() can catch it
 * up; our flush is always followed by a session reset, so it is dropped. */
static inline unsigned av1_builder_flush(Av1Builder *b, Av1ReadyFrame *out,
                                         unsigned max_out) {
    unsigned n = 0;

    b->stage_used = 0;
    if (b->pending.valid) {
        if (out && max_out && av1_builder_reserve(b, b->pending.data_size + 8192u)) {
            if (av1_ab_emit(b, &b->pending, 0, /*shown=*/1, &out[n]) != 0) {
                n++;
                b->n_flushed++;
            } else {
                b->n_failed++;
            }
        }
        av1_held_release(&b->pending);
    }
    if (b->provisional.valid) {
        /* already emitted once; its true mask is unreachable now */
        av1_held_release(&b->provisional);
        b->n_flushed++;
    }
    b->has_picture = 0;
    b->cur_size = 0;
    return n;
}

/* The immediate single-picture API (the reference's finish()): assemble what the
 * accumulator holds right now.  Exact for a shown key frame, best effort
 * otherwise.  Only the offline self-test drives this. */
static inline unsigned av1_builder_finish(Av1Builder *b, Av1ReadyFrame *out) {
    Av1HeldPicture now;
    unsigned refresh, size;

    if (!b->has_picture || !out)
        return 0;
    b->stage_used = 0;
    if (!av1_builder_reserve(b, b->cur_size + 8192u))
        return 0;
    av1_held_view(&now, b, 0);
    refresh = (b->frame.frame_type == AV1_FRAME_KEY && b->frame.show_frame)
                  ? 0xFFu
                  : av1_choose_refresh_flags(&b->dpb, &b->frame);
    size = av1_ab_emit(b, &now, refresh, /*shown=*/1, out);
    b->has_picture = 0;
    b->cur_size = 0;
    return size;
}

#endif /* VIRTIO_WDDM_AV1_DPB_H */
