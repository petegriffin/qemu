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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "v4l2_backend.h"
#include "virtio_video_helpers.h"

#include "standard-headers/linux/virtio_video.h"

/* function prototypes */

bool video_is_mplane(enum v4l2_buf_type type);
bool video_is_splane(enum v4l2_buf_type type);
bool video_is_meta(struct v4l2_device *dev);
bool video_is_capture(struct v4l2_device *dev);
bool video_is_output(struct v4l2_device *dev);
static const struct v4l2_format_info *v4l2_format_by_fourcc(unsigned int fourcc);
static const struct v4l2_format_info *v4l2_format_by_name(const char *name);
static const char *v4l2_format_name(unsigned int fourcc);
static const char *v4l2_buf_type_name(enum v4l2_buf_type type);
static const char *v4l2_field_name(enum v4l2_field field);
static int v4l2_open(struct v4l2_device *dev, const gchar *devname);
static int video_enum_frame_intervals(struct v4l2_device *dev, __u32 pixelformat,
                                      unsigned int width, unsigned int height, GList **p_vid_fmt_frm_rate_l);

static int video_enum_frame_sizes(struct v4l2_device *dev, __u32 pixelformat, GList **p_vid_fmt_frm_l);
static int cap_get_buf_type(unsigned int capabilities);
static int video_querycap(struct v4l2_device *dev, unsigned int *capabilities);
void video_device_type(struct v4l2_device *dev, enum v4l2_buf_type type, struct v4l2_fmtdesc *fmt_desc);
int v4l2_video_get_format(struct v4l2_device *dev, enum v4l2_buf_type type, struct v4l2_format *fmt);

int v4l2_video_set_format(struct v4l2_device *dev, enum v4l2_buf_type type,
                                 unsigned int w, unsigned int h, unsigned int format, unsigned int stride,
                                 unsigned int buffer_size, enum v4l2_field field,
                          unsigned int flags);

void video_free_frame_intervals( GList *frm_intervals_l);
void video_free_frame_sizes( GList *frm_sz_l);

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

bool video_is_mplane(enum v4l2_buf_type type)
{
        return type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
               type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

bool video_is_splane(enum v4l2_buf_type type)
{
        return type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
               type == V4L2_BUF_TYPE_VIDEO_OUTPUT;
}
bool video_is_meta(struct v4l2_device *dev)
{
        return dev->type == V4L2_BUF_TYPE_META_CAPTURE ||
               dev->type == V4L2_BUF_TYPE_META_OUTPUT;
}

bool video_is_capture(struct v4l2_device *dev)
{
        return dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
               dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
               dev->type == V4L2_BUF_TYPE_META_CAPTURE;
}

bool video_is_output(struct v4l2_device *dev)
{
        return dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
               dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT ||
               dev->type == V4L2_BUF_TYPE_META_OUTPUT;
}

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

static const struct {
        const char *name;
        enum v4l2_field field;
} fields[] = {
        { "any", V4L2_FIELD_ANY },
        { "none", V4L2_FIELD_NONE },
        { "top", V4L2_FIELD_TOP },
        { "bottom", V4L2_FIELD_BOTTOM },
        { "interlaced", V4L2_FIELD_INTERLACED },
        { "seq-tb", V4L2_FIELD_SEQ_TB },
        { "seq-bt", V4L2_FIELD_SEQ_BT },
        { "alternate", V4L2_FIELD_ALTERNATE },
        { "interlaced-tb", V4L2_FIELD_INTERLACED_TB },
        { "interlaced-bt", V4L2_FIELD_INTERLACED_BT },
};

static const char *v4l2_field_name(enum v4l2_field field)
{
        unsigned int i;

        for (i = 0; i < ARRAY_SIZE(fields); ++i) {
                if (fields[i].field == field)
                        return fields[i].name;
        }

        return "unknown";
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

static int video_enum_frame_intervals(struct v4l2_device *dev, __u32 pixelformat,
                                       unsigned int width, unsigned int height, GList **p_vid_fmt_frm_rate_l)
{
    struct v4l2_frmivalenum ival;
    GList *vid_fmt_frm_rate_l;
    struct video_format_frame_rates *vid_fmt_frm_rate;
    unsigned int i;
    int ret = 0;

    for (i = 0; ; ++i) {
        memset(&ival, 0, sizeof ival);
        ival.index = i;
        ival.pixel_format = pixelformat;
        ival.width = width;
        ival.height = height;
        ret = ioctl(dev->fd, VIDIOC_ENUM_FRAMEINTERVALS, &ival);
        if (ret < 0) {
            if (errno == EINVAL) /* EINVAL means no more frame intervals */
                ret = 0;
            else
                g_printerr("%s: VIDIOC_ENUM_FRAMEINTERVALS failed %s\n", __func__, g_strerror(errno));
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

        /* allocate video_format_frame */
        vid_fmt_frm_rate = g_new0(struct video_format_frame_rates, 1);
        /* keep a copy of v4l2 frmsizeenum struct */
        memcpy (&vid_fmt_frm_rate->v4l_ival, &ival, sizeof (struct v4l2_frmivalenum));

        switch (ival.type) {
        case V4L2_FRMIVAL_TYPE_DISCRETE:
            g_debug("%u/%u",
                    ival.discrete.numerator,
                    ival.discrete.denominator);

            vid_fmt_frm_rate->frame_rates.min = ival.discrete.denominator;
            
            break;

        case V4L2_FRMIVAL_TYPE_CONTINUOUS:
            g_debug("%u/%u - %u/%u",
                    ival.stepwise.min.numerator,
                    ival.stepwise.min.denominator,
                    ival.stepwise.max.numerator,
                    ival.stepwise.max.denominator);

            vid_fmt_frm_rate->frame_rates.min = ival.stepwise.min.denominator;
            vid_fmt_frm_rate->frame_rates.max = ival.stepwise.max.denominator;
            vid_fmt_frm_rate->frame_rates.step = 1;
                
            goto out;

        case V4L2_FRMIVAL_TYPE_STEPWISE:
            g_debug("%u/%u - %u/%u (by %u/%u)",
                    ival.stepwise.min.numerator,
                    ival.stepwise.min.denominator,
                    ival.stepwise.max.numerator,
                    ival.stepwise.max.denominator,
                    ival.stepwise.step.numerator,
                    ival.stepwise.step.denominator);
            
            vid_fmt_frm_rate->frame_rates.min = ival.stepwise.min.denominator;
            vid_fmt_frm_rate->frame_rates.max = ival.stepwise.max.denominator;
            vid_fmt_frm_rate->frame_rates.step = ival.stepwise.step.denominator;
            
            goto out;

        default:
            break;
        }
    }

out:
    if (ret == 0) {
        g_print("\n%s: Enumerated %d frame intervals \n", __func__, g_list_length(vid_fmt_frm_rate_l));
        g_return_val_if_fail (i == g_list_length(vid_fmt_frm_rate_l), -EINVAL);
        *p_vid_fmt_frm_rate_l = vid_fmt_frm_rate_l;
    }

    return ret;
}

static int video_enum_frame_sizes(struct v4l2_device *dev, __u32 pixelformat, GList **p_vid_fmt_frm_l)
{
    struct v4l2_frmsizeenum frame;
    struct video_format_frame *vid_frame;
    GList *vid_fmt_frm_l = NULL;
    unsigned int i;
    int ret;

    if (!dev)
        return -EINVAL;

    for (i = 0; ; ++i) {
        memset(&frame, 0, sizeof frame);
        frame.index = i;
        frame.pixel_format = pixelformat;
        ret = ioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, &frame);
        if (ret < 0) {
            if (errno == EINVAL) /* EINVAL means no more frame sizes */
                ret = 0;
            else
                g_printerr("%s: VIDIOC_ENUM_FRAMESIZES failed %s\n", __func__, g_strerror(errno));
            break;
        }

        if (i != frame.index)
            g_printerr("Warning: driver returned wrong frame index "
                       "%u.\n", frame.index);
        if (pixelformat != frame.pixel_format)
            g_printerr("Warning: driver returned wrong frame pixel "
                       "format %08x.\n", frame.pixel_format);

        /* allocate video_format_frame */
        vid_frame = g_new0(struct video_format_frame, 1);
        /* keep a copy of v4l2 frmsizeenum struct */
        memcpy (&vid_frame->v4l_framesize, &frame, sizeof (struct v4l2_frmsizeenum));
        vid_fmt_frm_l = g_list_append(vid_fmt_frm_l, vid_frame);

        switch (frame.type) {
        case V4L2_FRMSIZE_TYPE_DISCRETE:
            g_debug("\tFrame size (D): %ux%u (", frame.discrete.width,
                    frame.discrete.height);

            vid_frame->frame.width.min = htole32(frame.discrete.width);
            vid_frame->frame.width.max = htole32(frame.discrete.width);
            vid_frame->frame.height.min = htole32(frame.discrete.height);
            vid_frame->frame.height.max = htole32(frame.discrete.height);           
            
            if (video_enum_frame_intervals(dev, frame.pixel_format,
                                       frame.discrete.width, frame.discrete.height,
                                           &vid_frame->frm_rate_l) < 0)
                g_printerr("%s: video_enum_frame_intervals failed!", __func__);
            g_debug(")");
            break;

        case V4L2_FRMSIZE_TYPE_CONTINUOUS:
            g_debug("\tFrame size (C): %ux%u - %ux%u (",
                    frame.stepwise.min_width,
                    frame.stepwise.min_height,
                    frame.stepwise.max_width,
                    frame.stepwise.max_height);

            vid_frame->frame.width.min = htole32(frame.stepwise.min_width);
            vid_frame->frame.width.max = htole32(frame.stepwise.max_width);
            vid_frame->frame.width.step = htole32(frame.stepwise.step_width);
            vid_frame->frame.height.min = htole32(frame.stepwise.min_height);
            vid_frame->frame.height.max = htole32(frame.stepwise.max_height);
            vid_frame->frame.height.step = htole32(frame.stepwise.step_height);

            if (video_enum_frame_intervals(dev, frame.pixel_format,
                                       frame.stepwise.max_width,
                                       frame.stepwise.max_height,
                                           &vid_frame->frm_rate_l) < 0 )
                g_printerr("%s: video_enum_frame_intervals failed!", __func__);

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

            vid_frame->frame.width.min = htole32(frame.stepwise.min_width);
            vid_frame->frame.width.max = htole32(frame.stepwise.max_width);
            vid_frame->frame.width.step = htole32(frame.stepwise.step_width);
            vid_frame->frame.height.min = htole32(frame.stepwise.min_height);
            vid_frame->frame.height.max = htole32(frame.stepwise.max_height);
            vid_frame->frame.height.step = htole32(frame.stepwise.step_height);

            if (video_enum_frame_intervals(dev, frame.pixel_format,
                                       frame.stepwise.max_width,
                                       frame.stepwise.max_height,
                                       &vid_frame->frm_rate_l) < 0 )
                g_printerr("%s: video_enum_frame_intervals failed!", __func__);

            g_debug(")");
            break;

        default:
            break;
        }
    }
    if (ret == 0) {
        g_print("\n%s: Enumerated %d frame sizes and %d frame intervals\n",
                __func__, g_list_length(vid_fmt_frm_l), g_list_length(vid_frame->frm_rate_l));

        vid_frame->frame.num_rates = htole32(g_list_length(vid_frame->frm_rate_l));
        
        g_return_val_if_fail (i == g_list_length(vid_fmt_frm_l), -EINVAL);
        *p_vid_fmt_frm_l = vid_fmt_frm_l;
    }

    return ret;
}

static int cap_get_buf_type(unsigned int capabilities)
{
        if (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
                return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        } else if (capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE) {
                return V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        } else if (capabilities & V4L2_CAP_VIDEO_CAPTURE) {
                return  V4L2_BUF_TYPE_VIDEO_CAPTURE;
        } else if (capabilities & V4L2_CAP_VIDEO_OUTPUT) {
                return V4L2_BUF_TYPE_VIDEO_OUTPUT;
        } else if (capabilities & V4L2_CAP_META_CAPTURE) {
                return V4L2_BUF_TYPE_META_CAPTURE;
        } else if (capabilities & V4L2_CAP_META_OUTPUT) {
                return V4L2_BUF_TYPE_META_OUTPUT;
        } else {
                g_print("Device supports neither capture nor output. (caps 0x%x)\n", capabilities);
                //return -EINVAL;
        }

        return 0;
}

static int video_querycap(struct v4l2_device *dev, unsigned int *capabilities)
{
        struct v4l2_capability cap;
        unsigned int caps;
        bool has_video;
        bool has_meta;
        bool has_capture;
        bool has_output;
        bool has_mplane;
        int ret;

        memset(&cap, 0, sizeof cap);
        ret = ioctl(dev->fd, VIDIOC_QUERYCAP, &cap);
        if (ret < 0)
                return 0;

        caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
             ? cap.device_caps : cap.capabilities;

        has_video = caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE |
                            V4L2_CAP_VIDEO_CAPTURE |
                            V4L2_CAP_VIDEO_OUTPUT_MPLANE |
                            V4L2_CAP_VIDEO_OUTPUT);
        has_meta = caps & (V4L2_CAP_META_CAPTURE |
                           V4L2_CAP_META_OUTPUT);
        has_capture = caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE |
                              V4L2_CAP_VIDEO_CAPTURE |
                              V4L2_CAP_META_CAPTURE);
        has_output = caps & (V4L2_CAP_VIDEO_OUTPUT_MPLANE |
                             V4L2_CAP_VIDEO_OUTPUT |
                             V4L2_CAP_META_OUTPUT);
        has_mplane = caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE |
                             V4L2_CAP_VIDEO_OUTPUT_MPLANE);

        g_print("Device `%s' on `%s' (driver '%s') supports%s%s%s%s %s mplanes.\n",
                cap.card, cap.bus_info, cap.driver,
                has_video ? " video," : "",
                has_meta ? " meta-data," : "",
                has_capture ? " capture," : "",
                has_output ? " output," : "",
                has_mplane ? "with" : "without");

        *capabilities = caps;

        dev->type = cap_get_buf_type(caps);

        return 0;
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

int v4l2_video_get_format(struct v4l2_device *dev, enum v4l2_buf_type type, struct v4l2_format *fmt)
{
    unsigned int i;
    int ret;

    if (!fmt)
        return -EINVAL;

    if (!dev)
        return -EINVAL;

    memset(fmt, 0, sizeof (struct v4l2_format));
    fmt->type = type;

    ret = ioctl(dev->fd, VIDIOC_G_FMT, fmt);
    if (ret < 0) {
        g_printerr("Unable to get format: %s (%d).\n", strerror(errno),
               errno);
        return ret;
    }

    if (video_is_mplane(dev->type)) {
        dev->width = fmt->fmt.pix_mp.width;
        dev->height = fmt->fmt.pix_mp.height;
        dev->num_planes = fmt->fmt.pix_mp.num_planes;

        g_print("Video format: %s (%08x) %ux%u field %s, %u planes: \n",
               v4l2_format_name(fmt->fmt.pix_mp.pixelformat), fmt->fmt.pix_mp.pixelformat,
               fmt->fmt.pix_mp.width, fmt->fmt.pix_mp.height,
               v4l2_field_name(fmt->fmt.pix_mp.field),
               fmt->fmt.pix_mp.num_planes);

        for (i = 0; i < fmt->fmt.pix_mp.num_planes; i++) {
            dev->plane_fmt[i].bytesperline =
                fmt->fmt.pix_mp.plane_fmt[i].bytesperline;
            dev->plane_fmt[i].sizeimage =
                fmt->fmt.pix_mp.plane_fmt[i].bytesperline ?
                fmt->fmt.pix_mp.plane_fmt[i].sizeimage : 0;

            g_print(" * Stride %u, buffer size %u\n",
                   fmt->fmt.pix_mp.plane_fmt[i].bytesperline,
                   fmt->fmt.pix_mp.plane_fmt[i].sizeimage);
        }
    } else if (video_is_meta(dev)) {
        dev->width = 0;
        dev->height = 0;
        dev->num_planes = 1;

        g_print("Meta-data format: %s (%08x) buffer size %u\n",
               v4l2_format_name(fmt->fmt.meta.dataformat), fmt->fmt.meta.dataformat,
               fmt->fmt.meta.buffersize);
    } else {
        dev->width = fmt->fmt.pix.width;
        dev->height = fmt->fmt.pix.height;
        dev->num_planes = 1;

        dev->plane_fmt[0].bytesperline = fmt->fmt.pix.bytesperline;
        dev->plane_fmt[0].sizeimage = fmt->fmt.pix.bytesperline ? fmt->fmt.pix.sizeimage : 0;

        g_print("Video format: %s (%08x) %ux%u (stride %u) field %s buffer size %u\n",
               v4l2_format_name(fmt->fmt.pix.pixelformat), fmt->fmt.pix.pixelformat,
               fmt->fmt.pix.width, fmt->fmt.pix.height, fmt->fmt.pix.bytesperline,
               v4l2_field_name(fmt->fmt.pix_mp.field),
               fmt->fmt.pix.sizeimage);
    }

    return 0;
}

int v4l2_video_set_format(struct v4l2_device *dev, enum v4l2_buf_type type,
                                 unsigned int w, unsigned int h, unsigned int format, unsigned int stride,
                                 unsigned int buffer_size, enum v4l2_field field,
                                 unsigned int flags)

{
    struct v4l2_format fmt;
    int ret;
    unsigned int i;

    memset(&fmt, 0, sizeof fmt);
    fmt.type = type;

    if (video_is_mplane(dev->type)) {
        const struct v4l2_format_info *info = v4l2_format_by_fourcc(format);

        fmt.fmt.pix_mp.width = w;
        fmt.fmt.pix_mp.height = h;
        fmt.fmt.pix_mp.pixelformat = format;
        fmt.fmt.pix_mp.field = field;
        fmt.fmt.pix_mp.num_planes = info->n_planes;
        fmt.fmt.pix_mp.flags = flags;

        for (i = 0; i < fmt.fmt.pix_mp.num_planes; i++) {
            fmt.fmt.pix_mp.plane_fmt[i].bytesperline = stride;
            fmt.fmt.pix_mp.plane_fmt[i].sizeimage = buffer_size;
        }
    } else if (video_is_meta(dev)) {
        fmt.fmt.meta.dataformat = format;
        fmt.fmt.meta.buffersize = buffer_size;
    } else {
        fmt.fmt.pix.width = w;
        fmt.fmt.pix.height = h;
        fmt.fmt.pix.pixelformat = format;
        fmt.fmt.pix.field = field;
        fmt.fmt.pix.bytesperline = stride;
        fmt.fmt.pix.sizeimage = buffer_size;
        fmt.fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
        fmt.fmt.pix.flags = flags;
    }
    
    ret = ioctl(dev->fd, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
        g_printerr("Unable to set format: %s (%d).\n", strerror(errno),
               errno);
        return ret;
    }
}

int video_enum_formats(struct v4l2_device *dev, enum v4l2_buf_type type, GList **p_fmt_list, bool only_enum_fmt)
{
    struct v4l2_fmtdesc fmt;
    struct video_format *vid_fmt;
    GList *fmt_list=NULL;
    unsigned int index;
    int ret = 0;

    if (!dev)
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

        /* allocate video_format struct */
        vid_fmt = g_new0(struct video_format, 1);

        /* keep a copy of v4l2 struct */
        memcpy (&vid_fmt->fmt, &fmt, sizeof (struct v4l2_fmtdesc));

        /* add it to linked list */
        fmt_list = g_list_append(fmt_list, vid_fmt);

        if (!only_enum_fmt) {
            /* pass video_format to enum_frame_sizes */
            ret = video_enum_frame_sizes(dev, fmt.pixelformat, &vid_fmt->vid_fmt_frm_l);
            if (ret < 0) {
                g_printerr("video_enum_frame_sizes failed\n");
            }

            /* convert to virtio format*/
            v4l2_to_virtio_fmtdesc(dev, vid_fmt, type);
        }

        /* determine the type of device */
        video_device_type(dev, type, &fmt);
    }

    if (ret == 0) {
        g_print("%s: Enumerated %d formats on type(%s)\n", __func__, index, v4l2_buf_type_name(type));
        g_print("%s: Enumerated %d frame sizes\n", __func__, g_list_length(vid_fmt->vid_fmt_frm_l));
        g_return_val_if_fail (index == g_list_length(fmt_list), -EINVAL);
        
        *p_fmt_list = fmt_list;
    }

    return ret;
}

void video_free_frame_intervals( GList *frm_intervals_l)
{
    GList *l;
    struct video_format_frame_rates *vid_fmt_frm_rate;
    for (l = frm_intervals_l; l != NULL; l = l->next)
    {
        vid_fmt_frm_rate = l->data;
        //if (vid_fmt_frm_rate->frm_rate_l)
        g_free(vid_fmt_frm_rate);
    }
}

void video_free_frame_sizes( GList *frm_sz_l)
{
    GList *l;
    struct video_format_frame *vid_frame;
    for (l = frm_sz_l; l != NULL; l = l->next)
    {
        vid_frame = l->data;
        if (vid_frame->frm_rate_l)
            video_free_frame_intervals(vid_frame->frm_rate_l);

        g_free(vid_frame);
    }
}

void video_free_formats( GList **fmt_l)
{
    GList *l;
    struct video_format *vid_fmt;
    
    for (l = *fmt_l; l != NULL; l = l->next)
    {
        vid_fmt = l->data;
        if (vid_fmt->vid_fmt_frm_l)
            video_free_frame_sizes(vid_fmt->vid_fmt_frm_l);

        g_free(vid_fmt);
    }
}

void create_query_cap_resp( struct virtio_video_query_capability *qcmd, GList **fmt_l, replybuf *rbuf)
{
    GList *l;
    GList *fmt_frm_l;
    GList *frm_rate_l;

    struct video_format *vid_fmt;
    struct video_format_frame *vid_fmt_frm;
    struct video_format_frame_rates *vid_fmt_frm_rate;

    struct virtio_video_query_capability_resp *cap_resp;
    struct virtio_video_format_desc *fmt_dsc;
    struct virtio_video_format_frame *fmt_frame;
    struct virtio_video_format_range *frame_rate;

    l = *fmt_l;
    g_debug("%s: rbuf->base=0x%p\n", __func__, rbuf->buf_base);
    assert (MAX_CAPS_LEN > sizeof(struct virtio_video_query_capability_resp));
    cap_resp = (struct virtio_video_query_capability_resp *) rbuf->buf_pos;

    cap_resp->hdr.type = le32toh(qcmd->hdr.type);
    cap_resp->hdr.stream_id = le32toh(qcmd->hdr.stream_id);
    cap_resp->num_descs = htole32(g_list_length(*fmt_l));
    g_debug("%s: QueryCapability num_descs = %d", __func__, le32toh(cap_resp->num_descs));

    assert(le32toh(cap_resp->num_descs) < MAX_FMT_DESCS);

    inc_rbuf_pos(rbuf, sizeof (struct virtio_video_query_capability_resp));

    for (; l != NULL; l = l->next)
    {
        vid_fmt = l->data;
        memcpy (rbuf->buf_pos, &vid_fmt->desc, sizeof (struct virtio_video_format_desc));
        
        //buf_pos += sizeof (struct virtio_video_format_desc);
        inc_rbuf_pos(rbuf, sizeof (struct virtio_video_format_desc));
        fmt_dsc = (struct virtio_video_format_desc*) rbuf->buf_pos;

        /* does video_format have a list of format_frame? */
        if (!vid_fmt->vid_fmt_frm_l) {
            fmt_dsc->num_frames = htole32(0);
            g_debug("%s: QueryCapability num_frames = %d", __func__, le32toh(fmt_dsc->num_frames));
        } else {
            fmt_frm_l = vid_fmt->vid_fmt_frm_l;
            fmt_dsc->num_frames = htole32(g_list_length(fmt_frm_l));
            g_debug("%s: QueryCapability num_frames = %d", __func__, le32toh(fmt_dsc->num_frames));

            /* iterate format_frame list */
            for (; fmt_frm_l != NULL; fmt_frm_l = fmt_frm_l->next)
            {
                vid_fmt_frm = fmt_frm_l->data;
                memcpy (rbuf->buf_pos, &vid_fmt_frm->frame, sizeof (struct virtio_video_format_frame));
                //buf_pos += sizeof (struct virtio_video_format_frame);
                inc_rbuf_pos(rbuf, sizeof (struct virtio_video_format_frame));
                fmt_frame = (struct virtio_video_format_frame *) rbuf->buf_pos;

                /* does format_frame have a list of frame_rates? */
                if (!vid_fmt_frm->frm_rate_l) {
                    fmt_frame->num_rates = htole32(0);
                    g_debug("%s: QueryCapability num_rates = %d", __func__, le32toh(fmt_frame->num_rates));
                } else {
                    frm_rate_l = vid_fmt_frm->frm_rate_l;
                    fmt_frame->num_rates = htole32(g_list_length(frm_rate_l));
                    g_debug("%s: QueryCapability num_rates = %d", __func__, le32toh(fmt_frame->num_rates));

                    /* iterate frame_rate list */
                    for (; frm_rate_l != NULL; frm_rate_l = frm_rate_l->next)
                    {
                        vid_fmt_frm_rate = frm_rate_l->data;
                        memcpy (rbuf->buf_pos, &vid_fmt_frm_rate->frame_rates, sizeof (struct virtio_video_format_range));
                        inc_rbuf_pos(rbuf, sizeof (struct virtio_video_format_range));
                    }
                }
            }
        }
    }

    g_debug("%s: QueryCapability reply size %zu bytes", __func__, rbuf->replysize);
    debug_capability_reply(rbuf);
}

void v4l2_backend_free(const struct v4l2_device *dev)
{

    if (dev) {

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

    GList *vid_output_fmt_l = NULL;
    GList *vid_capture_fmt_l = NULL;

    /* enumerate coded formats on OUTPUT */
    ret = video_enum_formats( dev, V4L2_BUF_TYPE_VIDEO_OUTPUT, &vid_output_fmt_l, true);

    /* enumerate coded formats on CAPTURE */
    ret = video_enum_formats( dev, V4L2_BUF_TYPE_VIDEO_CAPTURE, &vid_capture_fmt_l, true);

    if (dev->dev_type & STATEFUL_ENCODER)
        g_print("%s: %s is a stateful encoder (0x%x)!\n", __func__, devname, dev->dev_type);

    if (dev->dev_type & STATEFUL_DECODER)
        g_print("%s: %s is a stateful decoder (0x%x)!\n", __func__, devname, dev->dev_type);

    video_free_formats(&vid_output_fmt_l);
    video_free_formats(&vid_capture_fmt_l);

    if (!(dev->dev_type & STATEFUL_ENCODER || dev->dev_type & STATEFUL_DECODER)) {
        g_printerr("v4l2 device not supported! v4l2 backend only supports stateful devices (%d)!\n", dev->dev_type);
        goto err;
    }

    g_print("%s: sucess!\n", __func__);
    return dev;

err:
    v4l2_backend_free(dev);
    return NULL;
}
