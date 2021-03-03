/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "a2dp-codec-api.h"

#include "bt-codec-msbc.h"
#include <sbc/sbc.h>

typedef struct sbc_info {
    sbc_t sbc;                           /* Codec data */
    size_t codesize, frame_length;       /* SBC Codesize, frame_length. We simply cache those values here */
    uint8_t msbc_seq:2;                  /* mSBC packet sequence number, 2 bits only */

    uint16_t msbc_push_offset;
    uint8_t input_buffer[MSBC_PACKET_SIZE];                        /* Codec transfer buffer */

    pa_sample_spec sample_spec;
} sbc_info_t;

static void *init(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    struct sbc_info *info;
    int ret;

    info = pa_xnew0(struct sbc_info, 1);

    ret = sbc_init_msbc(&info->sbc, 0);
    if (ret != 0) {
        pa_xfree(info);
        pa_log_error("mSBC initialization failed: %d", ret);
        return NULL;
    }

    info->sbc.endian = SBC_LE;

    info->codesize = sbc_get_codesize(&info->sbc);
    info->frame_length = sbc_get_frame_length(&info->sbc);
    pa_log_info("mSBC codesize=%d, frame_length=%d",
                (int)info->codesize,
                (int)info->frame_length);

    info->sample_spec.format = PA_SAMPLE_S16LE;
    info->sample_spec.channels = 1;
    info->sample_spec.rate = 16000;

    pa_assert(pa_frame_aligned(info->codesize, &info->sample_spec));

    *sample_spec = info->sample_spec;

    return info;
}

static void deinit(void *codec_info) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;

    sbc_finish(&sbc_info->sbc);
    pa_xfree(sbc_info);
}

static int reset(void *codec_info) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    int ret;

    /* SBC library release 1.5 has a bug in sbc_reinit_msbc:
     * it forgets to restore priv->msbc flag after clearing priv content.
     * This causes decoder assertion on first call since codesize would be
     * different from expected for mSBC configuration.
     *
     * Do not use sbc_reinit_msbc until it is fixed.
     */

    sbc_finish(&sbc_info->sbc);
    ret = sbc_init_msbc(&sbc_info->sbc, 0);
    if (ret != 0) {
        pa_xfree(sbc_info);
        pa_log_error("mSBC initialization failed: %d", ret);
        return -1;
    }

    sbc_info->sbc.endian = SBC_LE;

    sbc_info->msbc_seq = 0;
    sbc_info->msbc_push_offset = 0;

    return 0;
}

static size_t get_read_block_size(void *codec_info, size_t link_mtu) {
    struct sbc_info *info = (struct sbc_info *) codec_info;
    size_t block_size = info->codesize;

    /* this never happens as sbc_info->codesize is always frame-aligned */
    if (!pa_frame_aligned(block_size, &info->sample_spec)) {
        pa_log_debug("Got invalid block size: %lu, rounding down", block_size);
        block_size = pa_frame_align(block_size, &info->sample_spec);
    }

    return block_size;
}

static size_t get_write_block_size(void *codec_info, size_t link_mtu) {
    struct sbc_info *info = (struct sbc_info *) codec_info;
    return info->codesize;
}

static size_t get_encoded_block_size(void *codec_info, size_t input_size) {
    struct sbc_info *info = (struct sbc_info *) codec_info;
    size_t encoded_size = MSBC_PACKET_SIZE;

    /* input size should be aligned to write block size */
    pa_assert_fp(input_size % info->codesize == 0);

    return encoded_size * (input_size / info->codesize);
}

static size_t reduce_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    return 0;
}

static size_t increase_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    return 0;
}

static size_t encode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    struct msbc_frame *frame;
    uint8_t seq;
    ssize_t encoded;
    ssize_t written;

    pa_assert(input_size == sbc_info->codesize);

    /* must be room to render packet */
    pa_assert(output_size >= MSBC_PACKET_SIZE);

    frame = (struct msbc_frame *)output_buffer;
    seq = sbc_info->msbc_seq++;
    frame->hdr.id0 = MSBC_H2_ID0;
    frame->hdr.id1.s.id1 = MSBC_H2_ID1;
    if (seq & 0x02)
        frame->hdr.id1.s.sn1 = 3;
    else
        frame->hdr.id1.s.sn1 = 0;
    if (seq & 0x01)
        frame->hdr.id1.s.sn0 = 3;
    else
        frame->hdr.id1.s.sn0 = 0;

    encoded = sbc_encode(&sbc_info->sbc,
                         input_buffer, input_size,
                         frame->payload, MSBC_FRAME_SIZE,
                         &written);

    frame->padding = 0x00;

    if (PA_UNLIKELY(encoded <= 0)) {
        pa_log_error("SBC encoding error (%li) for input size %lu, SBC codesize %lu",
                    (long) encoded, input_size, sbc_get_codesize(&sbc_info->sbc));

        if (encoded < 0) {
            *processed = 0;
            return -1;
        } else {
            *processed = input_size;
            return 0;
        }
    }

    pa_assert_fp((size_t) encoded == sbc_info->codesize);
    pa_assert_fp((size_t) written == sbc_info->frame_length);

    *processed = encoded;

    return MSBC_PACKET_SIZE;
}

static inline bool is_all_zero(const uint8_t *ptr, size_t len) {
    size_t i;

    for (i = 0; i < len; ++i)
        if (ptr[i] != 0)
            return false;

    return true;
}

/*
 * We build a msbc frame up in the sbc_info buffer until we have a whole one
 */
static struct msbc_frame *msbc_find_frame(struct sbc_info *si, ssize_t *len,
                                          const uint8_t *buf, int *pseq)
{
    int i;
    uint8_t *p = si->input_buffer;

    /* skip input if it has all zero bytes
     * this could happen with older kernels inserting all-zero blocks
     * inside otherwise valid mSBC stream */
    if (*len > 0 && is_all_zero(buf, *len))
        *len = 0;

    for (i = 0; i < *len; i++) {
        union msbc_h2_id1 id1;

        if (si->msbc_push_offset == 0) {
            if (buf[i] != MSBC_H2_ID0)
                continue;
        } else if (si->msbc_push_offset == 1) {
            id1.b = buf[i];

            if (id1.s.id1 != MSBC_H2_ID1)
                goto error;
            if (id1.s.sn0 != 3 && id1.s.sn0 != 0)
                goto error;
            if (id1.s.sn1 != 3 && id1.s.sn1 != 0)
                goto error;
        } else if (si->msbc_push_offset == 2) {
            if (buf[i] != MSBC_SYNC_BYTE)
                goto error;
        }
        p[si->msbc_push_offset++] = buf[i];

        if (si->msbc_push_offset == MSBC_PACKET_SIZE) {
            id1.b = p[1];
            *pseq = (id1.s.sn0 & 0x1) | (id1.s.sn1 & 0x2);
            si->msbc_push_offset = 0;
            *len = *len - i;
            return (struct msbc_frame *)p;
        }
        continue;

        error:
        si->msbc_push_offset = 0;
    }
    *len = 0;
    return NULL;
}

static size_t decode_buffer(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    ssize_t remaining;
    ssize_t decoded;
    size_t written = 0;
    struct msbc_frame *frame;
    int seq;

    remaining = input_size;
    frame = msbc_find_frame(sbc_info, &remaining, input_buffer, &seq);

    /* only process when we have a full frame */
    if (!frame) {
        *processed = input_size - remaining;
        return 0;
    }

    uint8_t lost_packets = (4 + seq - sbc_info->msbc_seq++) % 4;

    if (lost_packets) {
        pa_log_error("Lost %d input audio packet(s)", lost_packets);
        sbc_info->msbc_seq = seq + 1;
    }

    decoded = sbc_decode(&sbc_info->sbc, frame->payload, MSBC_FRAME_SIZE, output_buffer, output_size, &written);

    /* now we've consumed the sbc_info buffer, start a new one with
     * the partial frame we have */
    if (remaining > 0)
        msbc_find_frame(sbc_info, &remaining, input_buffer + input_size - remaining, &seq);

    pa_assert_fp(remaining == 0);

    if (PA_UNLIKELY(decoded <= 0)) {
        pa_log_error("mSBC decoding error (%li)", (long) decoded);
        *processed = 0;
        return 0;
    }
    pa_assert_fp((size_t)decoded == sbc_info->frame_length);
    pa_assert_fp((size_t)written == sbc_info->codesize);

    *processed = input_size - remaining;
    return written;
}

/* Modified SBC codec for HFP Wideband Speech*/
const pa_a2dp_codec pa_bt_codec_msbc = {
    .name = "mSBC",
    .description = "mSBC",
    .init = init,
    .deinit = deinit,
    .reset = reset,
    .get_read_block_size = get_read_block_size,
    .get_write_block_size = get_write_block_size,
    .get_encoded_block_size = get_encoded_block_size,
    .reduce_encoder_bitrate = reduce_encoder_bitrate,
    .increase_encoder_bitrate = increase_encoder_bitrate,
    .encode_buffer = encode_buffer,
    .decode_buffer = decode_buffer,
};
