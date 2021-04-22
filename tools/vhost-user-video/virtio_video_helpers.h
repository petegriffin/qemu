// SPDX-License-Identifier: GPL-2.0+
/*
 * virtio-video helpers
 *
 * Copyright Linaro 2021
 *
 * Authors:
 *     Peter Griffin <peter.griffin@linaro.org>
 * 
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef VIRTIO_VIDEO_HELPERS_H
#define VIRTIO_VIDEO_HELPERS_H

#include <stdint.h>
#include "v4l2_backend.h"

typedef struct replybuf {
    uint8_t *buf_base;
    uint8_t *buf_pos;
    size_t replysize;
} replybuf;

void inc_rbuf_pos ( struct replybuf *rbuf, size_t incsize );
void debug_capability_reply(replybuf *buf);

#endif
