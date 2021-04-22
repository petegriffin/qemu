/*
 * Virtio vhost-user VIDEO Device
 *
 * Copyright Linaro 2021
 *
 * Authors: *     Peter Griffin <peter.griffin@linaro.org>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef V4L2_BACKEND_H
#define V4L2_BACKEND_H

#include "standard-headers/linux/virtio_video.h"
#include <linux/videodev2.h>
#include "virtio_video_helpers.h"

struct v4l2_device {
    int fd;
    int opened;
    unsigned int dev_type;
    enum v4l2_buf_type type;
    unsigned int width;
    unsigned int height;
    unsigned char num_planes;
    struct v4l2_plane_pix_format plane_fmt[VIDEO_MAX_PLANES];
};

#define ARRAY_SIZE(a)	(sizeof(a)/sizeof((a)[0]))

#define MAX_CAPS_LEN 4096
#define MAX_FMT_DESCS 64

struct video_format_frame_rates {
    struct virtio_video_format_range frame_rates;
    struct v4l2_frmivalenum v4l_ival;
};

struct video_format_frame {
    struct virtio_video_format_frame frame;
    struct v4l2_frmsizeenum v4l_framesize;
    GList *frm_rate_l;
};

struct video_format {
    struct v4l2_fmtdesc fmt;
    struct virtio_video_format_desc desc;
    GList *vid_fmt_frm_l; /* list of struct video_format_frame* */
};

#define STATEFUL_ENCODER (1 << 0)
#define STATEFUL_DECODER (1 << 1)
#define STATELESS_ENCODER (1 << 2)
#define STATELESS_DECODER (1 << 3)

/* Function protoypes */

struct v4l2_device* v4l2_backend_init(const gchar *devname);
void v4l2_backend_free(const struct v4l2_device *dev);

int video_enum_formats(struct v4l2_device *dev, enum v4l2_buf_type type, GList **p_fmt_list, bool only_enum_fmt);
void video_free_formats( GList **fmt_l);
void create_query_cap_resp( struct virtio_video_query_capability *qcmd, GList **fmt_l, replybuf *rbuf);

#endif
