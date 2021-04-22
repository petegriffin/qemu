// SPDX-License-Identifier: GPL-2.0+
/*
 * virtio-video helpers
 *
 * Copyright Linaro 2021
 * Copyright 2019 OpenSynergy GmbH.
 *
 * Authors:
 *     Peter Griffin <peter.griffin@linaro.org>
 * 
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "standard-headers/linux/virtio_video.h"
#include <linux/videodev2.h>
#include "v4l2_backend.h"
#include "virtio_video_helpers.h"

/* The following conversion helpers and tables taken from Linux
   frontend driver from opensynergy */

uint32_t virtio_video_level_to_v4l2(uint32_t level);
uint32_t virtio_video_v4l2_level_to_virtio(uint32_t v4l2_level);
uint32_t virtio_video_profile_to_v4l2(uint32_t profile);
uint32_t virtio_video_v4l2_profile_to_virtio(uint32_t v4l2_profile);
uint32_t virtio_video_format_to_v4l2(uint32_t format);
uint32_t virtio_video_v4l2_format_to_virtio(uint32_t v4l2_format);
uint32_t virtio_video_control_to_v4l2(uint32_t control);
uint32_t virtio_video_v4l2_control_to_virtio(uint32_t v4l2_control);
__le64 virtio_fmtdesc_generate_mask(GList **p_list);

struct virtio_video_convert_table {
	uint32_t virtio_value;
	uint32_t v4l2_value;
};

#define ARRAY_SIZE(a)	(sizeof(a)/sizeof((a)[0]))

static struct virtio_video_convert_table level_table[] = {
	{ VIRTIO_VIDEO_LEVEL_H264_1_0, V4L2_MPEG_VIDEO_H264_LEVEL_1_0 },
	{ VIRTIO_VIDEO_LEVEL_H264_1_1, V4L2_MPEG_VIDEO_H264_LEVEL_1_1 },
	{ VIRTIO_VIDEO_LEVEL_H264_1_2, V4L2_MPEG_VIDEO_H264_LEVEL_1_2 },
	{ VIRTIO_VIDEO_LEVEL_H264_1_3, V4L2_MPEG_VIDEO_H264_LEVEL_1_3 },
	{ VIRTIO_VIDEO_LEVEL_H264_2_0, V4L2_MPEG_VIDEO_H264_LEVEL_2_0 },
	{ VIRTIO_VIDEO_LEVEL_H264_2_1, V4L2_MPEG_VIDEO_H264_LEVEL_2_1 },
	{ VIRTIO_VIDEO_LEVEL_H264_2_2, V4L2_MPEG_VIDEO_H264_LEVEL_2_2 },
	{ VIRTIO_VIDEO_LEVEL_H264_3_0, V4L2_MPEG_VIDEO_H264_LEVEL_3_0 },
	{ VIRTIO_VIDEO_LEVEL_H264_3_1, V4L2_MPEG_VIDEO_H264_LEVEL_3_1 },
	{ VIRTIO_VIDEO_LEVEL_H264_3_2, V4L2_MPEG_VIDEO_H264_LEVEL_3_2 },
	{ VIRTIO_VIDEO_LEVEL_H264_4_0, V4L2_MPEG_VIDEO_H264_LEVEL_4_0 },
	{ VIRTIO_VIDEO_LEVEL_H264_4_1, V4L2_MPEG_VIDEO_H264_LEVEL_4_1 },
	{ VIRTIO_VIDEO_LEVEL_H264_4_2, V4L2_MPEG_VIDEO_H264_LEVEL_4_2 },
	{ VIRTIO_VIDEO_LEVEL_H264_5_0, V4L2_MPEG_VIDEO_H264_LEVEL_5_0 },
	{ VIRTIO_VIDEO_LEVEL_H264_5_1, V4L2_MPEG_VIDEO_H264_LEVEL_5_1 },
	{ 0 },
};

uint32_t virtio_video_level_to_v4l2(uint32_t level)
{
	size_t idx;

	for (idx = 0; idx < ARRAY_SIZE(level_table); idx++) {
		if (level_table[idx].virtio_value == level)
			return level_table[idx].v4l2_value;
	}

	return 0;
}

uint32_t virtio_video_v4l2_level_to_virtio(uint32_t v4l2_level)
{
	size_t idx;

	for (idx = 0; idx < ARRAY_SIZE(level_table); idx++) {
		if (level_table[idx].v4l2_value == v4l2_level)
			return level_table[idx].virtio_value;
	}

	return 0;
}

static struct virtio_video_convert_table profile_table[] = {
	{ VIRTIO_VIDEO_PROFILE_H264_BASELINE,
		V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE },
	{ VIRTIO_VIDEO_PROFILE_H264_MAIN, V4L2_MPEG_VIDEO_H264_PROFILE_MAIN },
	{ VIRTIO_VIDEO_PROFILE_H264_EXTENDED,
		V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED },
	{ VIRTIO_VIDEO_PROFILE_H264_HIGH, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH },
	{ VIRTIO_VIDEO_PROFILE_H264_HIGH10PROFILE,
		V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10 },
	{ VIRTIO_VIDEO_PROFILE_H264_HIGH422PROFILE,
		V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422},
	{ VIRTIO_VIDEO_PROFILE_H264_HIGH444PREDICTIVEPROFILE,
		V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE },
	{ VIRTIO_VIDEO_PROFILE_H264_SCALABLEBASELINE,
		V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_BASELINE },
	{ VIRTIO_VIDEO_PROFILE_H264_SCALABLEHIGH,
		V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_HIGH },
	{ VIRTIO_VIDEO_PROFILE_H264_STEREOHIGH,
		V4L2_MPEG_VIDEO_H264_PROFILE_STEREO_HIGH },
	{ VIRTIO_VIDEO_PROFILE_H264_MULTIVIEWHIGH,
		V4L2_MPEG_VIDEO_H264_PROFILE_MULTIVIEW_HIGH },
	{ 0 },
};

uint32_t virtio_video_profile_to_v4l2(uint32_t profile)
{
	size_t idx;

	for (idx = 0; idx < ARRAY_SIZE(profile_table); idx++) {
		if (profile_table[idx].virtio_value == profile)
			return profile_table[idx].v4l2_value;
	}

	return 0;
}

uint32_t virtio_video_v4l2_profile_to_virtio(uint32_t v4l2_profile)
{
	size_t idx;

	for (idx = 0; idx < ARRAY_SIZE(profile_table); idx++) {
		if (profile_table[idx].v4l2_value == v4l2_profile)
			return profile_table[idx].virtio_value;
	}

	return 0;
}


static struct virtio_video_convert_table format_table[] = {
	{ VIRTIO_VIDEO_FORMAT_ARGB8888, V4L2_PIX_FMT_ARGB32 },
	{ VIRTIO_VIDEO_FORMAT_BGRA8888, V4L2_PIX_FMT_ABGR32 },
	{ VIRTIO_VIDEO_FORMAT_NV12, V4L2_PIX_FMT_NV12 },
	{ VIRTIO_VIDEO_FORMAT_YUV420, V4L2_PIX_FMT_YUV420 },
	{ VIRTIO_VIDEO_FORMAT_YVU420, V4L2_PIX_FMT_YVU420 },
	{ VIRTIO_VIDEO_FORMAT_MPEG2, V4L2_PIX_FMT_MPEG2 },
	{ VIRTIO_VIDEO_FORMAT_MPEG4, V4L2_PIX_FMT_MPEG4 },
	{ VIRTIO_VIDEO_FORMAT_H264, V4L2_PIX_FMT_H264 },
	{ VIRTIO_VIDEO_FORMAT_HEVC, V4L2_PIX_FMT_HEVC },
	{ VIRTIO_VIDEO_FORMAT_VP8, V4L2_PIX_FMT_VP8 },
	{ VIRTIO_VIDEO_FORMAT_VP9, V4L2_PIX_FMT_VP9 },
	{ 0 },
};

uint32_t virtio_video_format_to_v4l2(uint32_t format)
{
	size_t idx;

	for (idx = 0; idx < ARRAY_SIZE(format_table); idx++) {
		if (format_table[idx].virtio_value == format)
			return format_table[idx].v4l2_value;
	}

	return 0;
}

uint32_t virtio_video_v4l2_format_to_virtio(uint32_t v4l2_format)
{
	size_t idx;

	for (idx = 0; idx < ARRAY_SIZE(format_table); idx++) {
		if (format_table[idx].v4l2_value == v4l2_format)
			return format_table[idx].virtio_value;
	}

	return 0;
}

static struct virtio_video_convert_table control_table[] = {
	{ VIRTIO_VIDEO_CONTROL_BITRATE, V4L2_CID_MPEG_VIDEO_BITRATE },
	{ VIRTIO_VIDEO_CONTROL_PROFILE, V4L2_CID_MPEG_VIDEO_H264_PROFILE },
	{ VIRTIO_VIDEO_CONTROL_LEVEL, V4L2_CID_MPEG_VIDEO_H264_LEVEL },
	{ VIRTIO_VIDEO_CONTROL_FORCE_KEYFRAME,
			V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME },
	{ 0 },
};

uint32_t virtio_video_control_to_v4l2(uint32_t control)
{
	size_t idx;

	for (idx = 0; idx < ARRAY_SIZE(control_table); idx++) {
		if (control_table[idx].virtio_value == control)
			return control_table[idx].v4l2_value;
	}

	return 0;
}

uint32_t virtio_video_v4l2_control_to_virtio(uint32_t v4l2_control)
{
	size_t idx;

	for (idx = 0; idx < ARRAY_SIZE(control_table); idx++) {
		if (control_table[idx].v4l2_value == v4l2_control)
			return control_table[idx].virtio_value;
	}

	return 0;
}

/* new helper functions (not from Linux frontend driver) */

__le64 virtio_fmtdesc_generate_mask(GList **p_list)
{
    uint64_t mask=0;
    unsigned int bit=0;
    GList *l;

    for (l=*p_list; l != NULL; l = l->next)
    {
        mask |= (1 << bit);
        bit++;
    }
    g_debug("%s: mask=0x%lx\n", __func__, mask);

    return mask;
}

void v4l2_to_virtio_fmtdesc(struct v4l2_device *dev, struct video_format *vid_fmt, enum v4l2_buf_type type)
{
    struct v4l2_fmtdesc *v4l2_fmtdsc = &vid_fmt->fmt;
    struct virtio_video_format_desc *virtio_fmtdesc = &vid_fmt->desc;
    enum v4l2_buf_type other_type;
    struct v4l2_format cur_fmt;
    int ret;

    if (!vid_fmt)
        return;

    g_debug("%s: \n", __func__);
    
    virtio_fmtdesc->format = htole32(virtio_video_v4l2_format_to_virtio(v4l2_fmtdsc->pixelformat));

    /* To generate the mask we need to check the FORMAT is already set.
       before we enumerate the other queue to generate the mask */

    /* get the current set format */
    ret = v4l2_video_get_format(dev, type, &cur_fmt);
    if (ret < 0) {
        g_printerr("%s: v4l2_video_get_format() failed", __func__);
    }

    if (video_is_mplane(cur_fmt.type)) {
            g_print("%s: Format is mplane\n", __func__);
            if (cur_fmt.fmt.pix_mp.pixelformat != vid_fmt->fmt.pixelformat) {

                /* set to correct pixel format */
                cur_fmt.fmt.pix_mp.pixelformat = vid_fmt->fmt.pixelformat;
                ret = v4l2_video_set_format(dev, cur_fmt.type, cur_fmt.fmt.pix_mp.width,
                                            cur_fmt.fmt.pix_mp.height, cur_fmt.fmt.pix_mp.pixelformat, 0, 0,
                                            cur_fmt.fmt.pix_mp.field, cur_fmt.fmt.pix_mp.flags );
                if (ret < 0) {
                    g_printerr("%s: v4l2_video_set_format failed\n", __func__);
                }

            } else {
                g_print("%s: formats are the same\n", __func__);
            }
        } else if (video_is_splane(cur_fmt.type)) {
            g_print("%s: Format is splane\n", __func__);
            if (cur_fmt.fmt.pix_mp.pixelformat != vid_fmt->fmt.pixelformat) {
                cur_fmt.fmt.pix.pixelformat = vid_fmt->fmt.pixelformat;
                /* set to correct pixel format */
                ret = v4l2_video_set_format(dev, cur_fmt.type, cur_fmt.fmt.pix.width,
                                            cur_fmt.fmt.pix.height, cur_fmt.fmt.pix.pixelformat, 0, 0,
                                            cur_fmt.fmt.pix.field, cur_fmt.fmt.pix.flags );
                if (ret < 0) {
                    g_printerr("%s: v4l2_video_set_format failed\n", __func__);
                }
            } else {
                g_print("%s: formats are the same\n", __func__);
            }
    }

    /* enumerate formats on the other queue now the format is set */
    GList *vid_fmts_l = NULL;
    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
        ret = video_enum_formats(dev, V4L2_BUF_TYPE_VIDEO_CAPTURE, &vid_fmts_l, true);
    } else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        ret = video_enum_formats(dev, V4L2_BUF_TYPE_VIDEO_OUTPUT, &vid_fmts_l, true);
    }

    /* generate the capability mask. bitset represents the supported combinations
       of input and output formats.*/
    virtio_fmtdesc->mask = htole64(virtio_fmtdesc_generate_mask(&vid_fmts_l));
    g_debug("%s: virtio_fmtdesc->mask=%llx", __func__, virtio_fmtdesc->mask);

    if (!v4l2_fmtdsc->flags & V4L2_FMT_FLAG_COMPRESSED) {
       g_debug("%s: Not an encoded format \n", __func__);

       /* TODO FIXME need to properly set plane_align and planes_layout fields */
       virtio_fmtdesc->planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE;
    }

    g_debug("%s: fmtdesc->num_frames = %d\n", __func__, g_list_length(vid_fmt->vid_fmt_frm_l));
    virtio_fmtdesc->num_frames = htole32(g_list_length(vid_fmt->vid_fmt_frm_l));

    video_free_formats(&vid_fmts_l);
}

void inc_rbuf_pos ( struct replybuf *rbuf, size_t incsize )
{
    /* can't excede MAX_CAPS_LEN in our reply buf*/
    if ((rbuf->buf_pos + incsize) > (rbuf->buf_base + MAX_CAPS_LEN)) {
        g_debug("%s: 0x%p > 0x%p", __func__, (rbuf->buf_pos + incsize) > (rbuf->buf_base + MAX_CAPS_LEN));
        assert ((rbuf->buf_pos + incsize) > (rbuf->buf_base + MAX_CAPS_LEN));
    }
    rbuf->buf_pos += incsize;
    rbuf->replysize = rbuf->buf_pos - rbuf->buf_base;
}

/* debug functions */
void debug_capability_reply(replybuf *buf)
{
    struct virtio_video_query_capability_resp *cap_resp;
    struct virtio_video_format_desc *fmt_dsc;
    struct virtio_video_format_frame *fmt_frame;
    struct virtio_video_format_range *frame_rate;
    int num_descs;
    int num_frames;
    int num_rates;

    replybuf tmpbuf;

    tmpbuf.buf_base = buf->buf_base;
    tmpbuf.buf_pos = tmpbuf.buf_base;
    tmpbuf.replysize = 0;  

    /* function assumes generation of the replybuf has finished and buf_pos can be re-used */
    g_debug("\n********************************************");
    g_debug("\nQueryCapability base=%p size=0x%x", tmpbuf.buf_base, tmpbuf.replysize);

    cap_resp = (struct virtio_video_query_capability_resp *) tmpbuf.buf_pos;
    g_debug("CapResp:");
    g_debug("hdr.type 0x%x", le32toh(cap_resp->hdr.type));
    g_debug("hdr.stream_id 0x%x", le32toh(cap_resp->hdr.stream_id));
    g_debug("num_descs = %d", le32toh(cap_resp->num_descs));
    
    num_descs = le32toh(cap_resp->num_descs);
    inc_rbuf_pos(&tmpbuf, sizeof (struct virtio_video_query_capability_resp));

    for (int i = 0; i < num_descs; i++)
    {

        fmt_dsc = (struct virtio_video_format_desc *) tmpbuf.buf_pos;

        g_debug("FmtDesc(%d)", i);
        g_debug("FmtDesc(%d) mask=0x%llx", i, le64toh(fmt_dsc->mask));
        g_debug("FmtDesc(%d) format=0x%lx", i, le32toh(fmt_dsc->format));
        g_debug("FmtDesc(%d) planes_layout 0x%x", i, le32toh(fmt_dsc->planes_layout));
        g_debug("FmtDesc(%d) plane_align 0x%x", i, le32toh(fmt_dsc->plane_align));
        g_debug("FmtDesc(%d) num_frames %d", i, le32toh(fmt_dsc->num_frames));

        inc_rbuf_pos(&tmpbuf, sizeof (struct virtio_video_format_desc));

        num_frames = le32toh(fmt_dsc->num_frames);

        for (int x = 0; x < num_frames; x++)
        {
            fmt_frame = (struct virtio_video_format_frame *) tmpbuf.buf_pos;

            g_debug("FmtFrame(%d)", x);
            g_debug("FmtFrame(%d) width.min %d", x, le32toh(fmt_frame->width.min));
            g_debug("FmtFrame(%d) width.max %d", x, le32toh(fmt_frame->width.max));
            g_debug("FmtFrame(%d) width.step %d", x, le32toh(fmt_frame->width.step));
            g_debug("FmtFrame(%d) height.min %d", x, le32toh(fmt_frame->height.min));
            g_debug("FmtFrame(%d) height.max %d", x, le32toh(fmt_frame->height.max));
            g_debug("FmtFrame(%d) height.step %d", x, le32toh(fmt_frame->height.step));
            g_debug("FmtFrame(%d) num_rates %d", x, le32toh(fmt_frame->num_rates));

            inc_rbuf_pos(&tmpbuf, sizeof (struct virtio_video_format_frame));
           
            num_rates = le32toh(fmt_frame->num_rates);

            for (int y = 0; y < num_rates; y++) {

                frame_rate = (struct virtio_video_format_range *) tmpbuf.buf_pos;
               
                g_debug("FrameRate(%d)", y);
                g_debug("FrameRate(%d) min %d", x, le32toh(frame_rate->min));
                g_debug("FraneRate(%d) max %d", x, le32toh(frame_rate->max));
                g_debug("FrameRate(%d) step %d", x, le32toh(frame_rate->step));

                inc_rbuf_pos(&tmpbuf, sizeof (struct virtio_video_format_range));
            }
        }
    }
}
