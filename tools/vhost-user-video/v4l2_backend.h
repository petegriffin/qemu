/*
 * Virtio vhost-user VIDEO Device
 *
 * Copyright Linaro 2021
 *
 * Authors:
 *     Peter Griffin <peter.griffin@linaro.org>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef V4L2_BACKEND_H
#define V4L2_BACKEND_H

struct v4l2_device {
    int fd;
    int opened;
    unsigned int dev_type;

    GArray *output_fmtdesc;
    unsigned int num_output_fmtdesc;

    GArray *capture_fmtdesc;
    unsigned int num_capture_fmtdesc;
};

#define STATEFUL_ENCODER (1 << 0)
#define STATEFUL_DECODER (1 << 1)
#define STATELESS_ENCODER (1 << 2)
#define STATELESS_DECODER (1 << 3)

struct v4l2_device* v4l2_backend_init(const gchar *devname);

#endif
