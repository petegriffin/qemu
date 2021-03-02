/*
 * virtio-video video v4l2 backend 
 *
 * The purpose of this backend is to interface with
 * v4l2 stateful encoder and decoder devices in the kernel.
 *
 * v4l2 stateless devices are NOT supported currently.
 * 
 * Some v4l2 helper functions taken from yatva
 *
 * Copyright (c) 2021 Linaro Ltd
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <linux/videodev2.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <stdbool.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "v4l2_backend.h"

#define ARRAY_SIZE(a)	(sizeof(a)/sizeof((a)[0]))

static struct v4l2_format_info {
	const char *name;
	unsigned int fourcc;
	unsigned char n_planes;
} pixel_formats[] = {
	{ "RGB332", V4L2_PIX_FMT_RGB332, 1 },
	{ "RGB444", V4L2_PIX_FMT_RGB444, 1 },
	{ "ARGB444", V4L2_PIX_FMT_ARGB444, 1 },
	{ "XRGB444", V4L2_PIX_FMT_XRGB444, 1 },
	{ "RGB555", V4L2_PIX_FMT_RGB555, 1 },
	{ "ARGB555", V4L2_PIX_FMT_ARGB555, 1 },
	{ "XRGB555", V4L2_PIX_FMT_XRGB555, 1 },
	{ "RGB565", V4L2_PIX_FMT_RGB565, 1 },
	{ "RGB555X", V4L2_PIX_FMT_RGB555X, 1 },
	{ "RGB565X", V4L2_PIX_FMT_RGB565X, 1 },
	{ "BGR666", V4L2_PIX_FMT_BGR666, 1 },
	{ "BGR24", V4L2_PIX_FMT_BGR24, 1 },
	{ "RGB24", V4L2_PIX_FMT_RGB24, 1 },
	{ "BGR32", V4L2_PIX_FMT_BGR32, 1 },
	{ "ABGR32", V4L2_PIX_FMT_ABGR32, 1 },
	{ "XBGR32", V4L2_PIX_FMT_XBGR32, 1 },
	{ "RGB32", V4L2_PIX_FMT_RGB32, 1 },
	{ "ARGB32", V4L2_PIX_FMT_ARGB32, 1 },
	{ "XRGB32", V4L2_PIX_FMT_XRGB32, 1 },
	{ "HSV24", V4L2_PIX_FMT_HSV24, 1 },
	{ "HSV32", V4L2_PIX_FMT_HSV32, 1 },
	{ "Y8", V4L2_PIX_FMT_GREY, 1 },
	{ "Y10", V4L2_PIX_FMT_Y10, 1 },
	{ "Y12", V4L2_PIX_FMT_Y12, 1 },
	{ "Y16", V4L2_PIX_FMT_Y16, 1 },
	{ "UYVY", V4L2_PIX_FMT_UYVY, 1 },
	{ "VYUY", V4L2_PIX_FMT_VYUY, 1 },
	{ "YUYV", V4L2_PIX_FMT_YUYV, 1 },
	{ "YVYU", V4L2_PIX_FMT_YVYU, 1 },
	{ "NV12", V4L2_PIX_FMT_NV12, 1 },
	{ "NV12M", V4L2_PIX_FMT_NV12M, 2 },
	{ "NV21", V4L2_PIX_FMT_NV21, 1 },
	{ "NV21M", V4L2_PIX_FMT_NV21M, 2 },
	{ "NV16", V4L2_PIX_FMT_NV16, 1 },
	{ "NV16M", V4L2_PIX_FMT_NV16M, 2 },
	{ "NV61", V4L2_PIX_FMT_NV61, 1 },
	{ "NV61M", V4L2_PIX_FMT_NV61M, 2 },
	{ "NV24", V4L2_PIX_FMT_NV24, 1 },
	{ "NV42", V4L2_PIX_FMT_NV42, 1 },
	{ "YUV420M", V4L2_PIX_FMT_YUV420M, 3 },
	{ "YUV422M", V4L2_PIX_FMT_YUV422M, 3 },
	{ "YUV444M", V4L2_PIX_FMT_YUV444M, 3 },
	{ "YVU420M", V4L2_PIX_FMT_YVU420M, 3 },
	{ "YVU422M", V4L2_PIX_FMT_YVU422M, 3 },
	{ "YVU444M", V4L2_PIX_FMT_YVU444M, 3 },
	{ "SBGGR8", V4L2_PIX_FMT_SBGGR8, 1 },
	{ "SGBRG8", V4L2_PIX_FMT_SGBRG8, 1 },
	{ "SGRBG8", V4L2_PIX_FMT_SGRBG8, 1 },
	{ "SRGGB8", V4L2_PIX_FMT_SRGGB8, 1 },
	{ "SBGGR10_DPCM8", V4L2_PIX_FMT_SBGGR10DPCM8, 1 },
	{ "SGBRG10_DPCM8", V4L2_PIX_FMT_SGBRG10DPCM8, 1 },
	{ "SGRBG10_DPCM8", V4L2_PIX_FMT_SGRBG10DPCM8, 1 },
	{ "SRGGB10_DPCM8", V4L2_PIX_FMT_SRGGB10DPCM8, 1 },
	{ "SBGGR10", V4L2_PIX_FMT_SBGGR10, 1 },
	{ "SGBRG10", V4L2_PIX_FMT_SGBRG10, 1 },
	{ "SGRBG10", V4L2_PIX_FMT_SGRBG10, 1 },
	{ "SRGGB10", V4L2_PIX_FMT_SRGGB10, 1 },
	{ "SBGGR10P", V4L2_PIX_FMT_SBGGR10P, 1 },
	{ "SGBRG10P", V4L2_PIX_FMT_SGBRG10P, 1 },
	{ "SGRBG10P", V4L2_PIX_FMT_SGRBG10P, 1 },
	{ "SRGGB10P", V4L2_PIX_FMT_SRGGB10P, 1 },
	{ "SBGGR12", V4L2_PIX_FMT_SBGGR12, 1 },
	{ "SGBRG12", V4L2_PIX_FMT_SGBRG12, 1 },
	{ "SGRBG12", V4L2_PIX_FMT_SGRBG12, 1 },
	{ "SRGGB12", V4L2_PIX_FMT_SRGGB12, 1 },
	{ "IPU3_SBGGR10", V4L2_PIX_FMT_IPU3_SBGGR10, 1 },
	{ "IPU3_SGBRG10", V4L2_PIX_FMT_IPU3_SGBRG10, 1 },
	{ "IPU3_SGRBG10", V4L2_PIX_FMT_IPU3_SGRBG10, 1 },
	{ "IPU3_SRGGB10", V4L2_PIX_FMT_IPU3_SRGGB10, 1 },
	{ "DV", V4L2_PIX_FMT_DV, 1 },
	{ "MJPEG", V4L2_PIX_FMT_MJPEG, 1 },
	{ "MPEG", V4L2_PIX_FMT_MPEG, 1 },
	{ "FWHT", V4L2_PIX_FMT_FWHT, 1 },
};

static const struct v4l2_format_info *v4l2_format_by_fourcc(unsigned int fourcc)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
        if (pixel_formats[i].fourcc == fourcc)
            return &pixel_formats[i];
    }

    return NULL;
}

static const struct v4l2_format_info *v4l2_format_by_name(const char *name)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
        if (strcasecmp(pixel_formats[i].name, name) == 0)
            return &pixel_formats[i];
    }

    return NULL;
}

static const char *v4l2_format_name(unsigned int fourcc)
{
    const struct v4l2_format_info *info;
    static char name[5];
    unsigned int i;

    info = v4l2_format_by_fourcc(fourcc);
    if (info)
        return info->name;

    for (i = 0; i < 4; ++i) {
        name[i] = fourcc & 0xff;
        fourcc >>= 8;
    }

    name[4] = '\0';
    return name;
}

static struct {
   enum v4l2_buf_type type;
   bool supported;
   const char *name;
   const char *string;
} buf_types[] = {
	{ V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1, "Video capture mplanes", "capture-mplane", },
	{ V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 1, "Video output", "output-mplane", },
	{ V4L2_BUF_TYPE_VIDEO_CAPTURE, 1, "Video capture", "capture", },
	{ V4L2_BUF_TYPE_VIDEO_OUTPUT, 1, "Video output mplanes", "output", },
	{ V4L2_BUF_TYPE_VIDEO_OVERLAY, 0, "Video overlay", "overlay" },
	{ V4L2_BUF_TYPE_META_CAPTURE, 0, "Meta-data capture", "meta-capture", },
	{ V4L2_BUF_TYPE_META_OUTPUT, 0, "Meta-data output", "meta-output", },
};

static int v4l2_buf_type_from_string(const char *str)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(buf_types); i++) {
        if (!buf_types[i].supported)
            continue;
        
        if (strcmp(buf_types[i].string, str))
            continue;

        return buf_types[i].type;
    }

    return -1;
}

static const char *v4l2_buf_type_name(enum v4l2_buf_type type)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(buf_types); ++i) {
        if (buf_types[i].type == type)
            return buf_types[i].name;
    }

    if (type & V4L2_BUF_TYPE_PRIVATE)
        return "Private";
    else
        return "Unknown";
}

static int v4l2_open(struct v4l2_device *dev, const gchar *devname)
{

    dev->fd = open(devname, O_RDWR);
    if (dev->fd < 0) {
        g_printerr("Error opening device %s: %s (%d).\n", devname,
                   strerror(errno), errno);
        return dev->fd;
    }

    g_print("Device %s opened.\n", devname);
    dev->opened = 1;

    return 0;
}

static void video_enum_frame_intervals(struct v4l2_device *dev, __u32 pixelformat,
	unsigned int width, unsigned int height)
{
    struct v4l2_frmivalenum ival;
    unsigned int i;
    int ret;

    for (i = 0; ; ++i) {
        memset(&ival, 0, sizeof ival);
        ival.index = i;
        ival.pixel_format = pixelformat;
        ival.width = width;
        ival.height = height;
        ret = ioctl(dev->fd, VIDIOC_ENUM_FRAMEINTERVALS, &ival);
        if (ret < 0) {
            g_print("%s: ret=%d i=%d\n", __func__, ret, i);
            break;
        }

        /* driver sanity checks */
        if (i != ival.index)
            g_printerr("Warning: driver returned wrong ival index "
                       "%u.\n", ival.index);
        if (pixelformat != ival.pixel_format)
            g_printerr("Warning: driver returned wrong ival pixel "
                       "format %08x.\n", ival.pixel_format);
        if (width != ival.width)
            g_printerr("Warning: driver returned wrong ival width "
                       "%u.\n", ival.width);
        if (height != ival.height)
            g_printerr("Warning: driver returned wrong ival height "
                       "%u.\n", ival.height);

        if (i != 0)
            g_print(", ");

        switch (ival.type) {
        case V4L2_FRMIVAL_TYPE_DISCRETE:
            g_debug("%u/%u",
                    ival.discrete.numerator,
                    ival.discrete.denominator);
            break;

        case V4L2_FRMIVAL_TYPE_CONTINUOUS:
            g_debug("%u/%u - %u/%u",
                    ival.stepwise.min.numerator,
                    ival.stepwise.min.denominator,
                    ival.stepwise.max.numerator,
                    ival.stepwise.max.denominator);
            return;

        case V4L2_FRMIVAL_TYPE_STEPWISE:
            g_debug("%u/%u - %u/%u (by %u/%u)",
                    ival.stepwise.min.numerator,
                    ival.stepwise.min.denominator,
                    ival.stepwise.max.numerator,
                    ival.stepwise.max.denominator,
                    ival.stepwise.step.numerator,
                    ival.stepwise.step.denominator);
            return;

        default:
            break;
        }
    }
}

static void video_enum_frame_sizes(struct v4l2_device *dev, __u32 pixelformat)
{
    struct v4l2_frmsizeenum frame;
    unsigned int i;
    int ret;

    for (i = 0; ; ++i) {
        memset(&frame, 0, sizeof frame);
        frame.index = i;
        frame.pixel_format = pixelformat;
        ret = ioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, &frame);
        if (ret < 0)
            break;

        if (i != frame.index)
            g_printerr("Warning: driver returned wrong frame index "
                       "%u.\n", frame.index);
        if (pixelformat != frame.pixel_format)
            g_printerr("Warning: driver returned wrong frame pixel "
                       "format %08x.\n", frame.pixel_format);

        switch (frame.type) {
        case V4L2_FRMSIZE_TYPE_DISCRETE:
            g_debug("\tFrame size (D): %ux%u (", frame.discrete.width,
                    frame.discrete.height);
            video_enum_frame_intervals(dev, frame.pixel_format,
                                       frame.discrete.width, frame.discrete.height);
            g_debug(")");
            break;

        case V4L2_FRMSIZE_TYPE_CONTINUOUS:
            g_debug("\tFrame size (C): %ux%u - %ux%u (",
                    frame.stepwise.min_width,
                    frame.stepwise.min_height,
                    frame.stepwise.max_width,
                    frame.stepwise.max_height);
            video_enum_frame_intervals(dev, frame.pixel_format,
                                       frame.stepwise.max_width,
                                       frame.stepwise.max_height);
            g_debug(")");
            break;

        case V4L2_FRMSIZE_TYPE_STEPWISE:
            g_debug("\tFrame size (S): %ux%u - %ux%u (by %ux%u) (",
                    frame.stepwise.min_width,
                    frame.stepwise.min_height,
                    frame.stepwise.max_width,
                    frame.stepwise.max_height,
                    frame.stepwise.step_width,
                    frame.stepwise.step_height);
            video_enum_frame_intervals(dev, frame.pixel_format,
                                       frame.stepwise.max_width,
                                       frame.stepwise.max_height);
            g_debug(")");
            break;

        default:
            break;
        }
    }
}

void video_device_type(struct v4l2_device *dev, enum v4l2_buf_type type, struct v4l2_fmtdesc *fmt_desc)
{
    if (fmt_desc->flags & V4L2_FMT_FLAG_COMPRESSED) {

        switch (fmt_desc->pixelformat) {
        case V4L2_PIX_FMT_H263:
        case V4L2_PIX_FMT_H264:
        case V4L2_PIX_FMT_H264_NO_SC:
        case V4L2_PIX_FMT_H264_MVC:
        case V4L2_PIX_FMT_MPEG1:
        case V4L2_PIX_FMT_MPEG2:
        case V4L2_PIX_FMT_MPEG4:
        case V4L2_PIX_FMT_XVID:
        case V4L2_PIX_FMT_VC1_ANNEX_G:
        case V4L2_PIX_FMT_VC1_ANNEX_L:
        case V4L2_PIX_FMT_VP8:
        case V4L2_PIX_FMT_VP9:
        case V4L2_PIX_FMT_HEVC:
        case V4L2_PIX_FMT_FWHT:
            if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
                dev->dev_type |= STATEFUL_DECODER;
            if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
                dev->dev_type |= STATEFUL_ENCODER;
            break;
        case V4L2_PIX_FMT_MPEG2_SLICE:
        case V4L2_PIX_FMT_FWHT_STATELESS:
            if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
                dev->dev_type |= STATELESS_DECODER;
            if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
                dev->dev_type |= STATELESS_ENCODER;
            break;

        default:
            break;
        }
    }
}

static int video_enum_formats(struct v4l2_device *dev, enum v4l2_buf_type type, unsigned int *num_fmts, GArray *fmtdesc)
{
    struct v4l2_fmtdesc fmt;
    unsigned int index;
    int ret = 0;

    if (!dev)
        return -EINVAL;

    if (!num_fmts)
        return -EINVAL;

    if (!fmtdesc)
        return -EINVAL;

    for (index = 0; ; ++index) {
        memset(&fmt, 0, sizeof fmt);
        fmt.index = index;
        fmt.type = type;
        ret = ioctl(dev->fd, VIDIOC_ENUM_FMT, &fmt);

        if (ret < 0) {
            if (errno == EINVAL)
               ret = 0;
            else
                g_printerr("%s: VIDIOC_ENUM_FMT failed %s\n", __func__, g_strerror(errno));

        break;
        }

        /* do some driver sanity checks */
        if (index != fmt.index)
            g_warning("v4l2 driver modified index %u.\n", fmt.index);
        if (type != fmt.type)
            g_warning("v4l2 driver modified type %u.\n", fmt.type);

        g_debug("\tFormat %u: %s (%08x)", index,
               v4l2_format_name(fmt.pixelformat), fmt.pixelformat);
        g_debug("\tType: %s (%u)", v4l2_buf_type_name(fmt.type),
               fmt.type);
        g_debug("\tName: %.32s", fmt.description);
        video_enum_frame_sizes(dev, fmt.pixelformat);

        video_device_type(dev, type, &fmt);

        g_array_append_val(fmtdesc, fmt);
    }

    if (ret == 0) {
        g_print("%s: Enumerated %d formats on type(%s)\n", __func__, index, v4l2_buf_type_name(type));
        *num_fmts = index;
    }

    return ret;
}

static void v4l2_backend_free(const struct v4l2_device *dev)
{

    if (dev) {
    
        if (dev->output_fmtdesc)
            g_array_free (dev->output_fmtdesc, TRUE);
        if (dev->capture_fmtdesc)
            g_array_free(dev->capture_fmtdesc, TRUE);

        if (dev->opened)
            close(dev->fd);
    }

    g_free(dev);
}

struct v4l2_device * v4l2_backend_init(const gchar *devname)
{
    struct v4l2_device *dev;
    int ret = 0;

    g_debug("%s: \n", __func__);

    if (!devname)
        return NULL;

    dev = g_malloc0(sizeof(struct v4l2_device));

    /* open the device */
    ret = v4l2_open(dev, devname); 
    if (ret < 0) {
        g_printerr("v4l2_open() failed!\n");
        goto err;
    }

    /* enumerate formats */
    dev->output_fmtdesc = g_array_new(FALSE, FALSE, sizeof(struct v4l2_fmtdesc));
    dev->capture_fmtdesc = g_array_new(FALSE, FALSE, sizeof(struct v4l2_fmtdesc));

    /* enumerate coded formats on OUTPUT */
    ret = video_enum_formats( dev, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                              &dev->num_output_fmtdesc, dev->output_fmtdesc);

    /* enumerate coded formats on CAPTURE */
    ret = video_enum_formats( dev, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                              &dev->num_capture_fmtdesc, dev->capture_fmtdesc);

    if (dev->dev_type & STATEFUL_ENCODER)
        g_print("%s: %s is a stateful encoder (0x%x)!\n", __func__, devname, dev->dev_type);

    if (dev->dev_type & STATEFUL_DECODER)
        g_print("%s: %s is a stateful decoder (0x%x)!\n", __func__, devname, dev->dev_type);

    if (!(dev->dev_type & STATEFUL_ENCODER || dev->dev_type & STATEFUL_DECODER)) {
        g_printerr("v4l2 device not supported! v4l2 backend only supports stateful devices!\n");
        goto err;
    }

    g_print("%s: sucess!\n", __func__);
    return dev;

err:
    v4l2_backend_free(dev);
    return NULL;
}
