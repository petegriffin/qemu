/*
 * VIRTIO Video Emulation via vhost-user
 *
 * Copyright (c) 2021 Linaro Ltd
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define G_LOG_DOMAIN "vhost-user-video"
#define G_LOG_USE_STRUCTURED 1

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <endian.h>
#include <assert.h>

#include "libvhost-user-glib.h"
#include "libvhost-user.h"
#include "standard-headers/linux/virtio_video.h"

#include "qemu/compiler.h"
#include "qemu/iov.h"

#include "v4l2_backend.h"
#include "virtio_video_helpers.h"

size_t video_iov_size(const struct iovec *iov, const unsigned int iov_cnt);

#ifndef container_of
#define container_of(ptr, type, member) ({                      \
        const typeof(((type *) 0)->member) *__mptr = (ptr);     \
        (type *) ((char *) __mptr - offsetof(type, member));})
#endif

static gchar *socket_path;
static gchar *v4l2_path;
static gint socket_fd = -1;
static gboolean print_cap;
static gboolean verbose;
static gboolean debug;

static GOptionEntry options[] =
{
    { "socket-path", 0, 0, G_OPTION_ARG_FILENAME, &socket_path, "Location of vhost-user Unix domain socket, incompatible with --fd", "PATH" },
    { "v4l2-device", 0, 0, G_OPTION_ARG_FILENAME, &v4l2_path, "Location of v4l2 device node", "PATH" },
    { "fd", 0, 0, G_OPTION_ARG_INT, &socket_fd, "Specify the file-descriptor of the backend, incompatible with --socket-path", "FD" },
    { "print-capabilities", 0, 0, G_OPTION_ARG_NONE, &print_cap, "Output to stdout the backend capabilities in JSON format and exit", NULL},
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Be more verbose in output", NULL},
    { "debug", 0, 0, G_OPTION_ARG_NONE, &debug, "Include debug output", NULL},
    { NULL }
};

enum {
    VHOST_USER_VIDEO_MAX_QUEUES = 2,
};


/* taken from util/iov.c */
size_t video_iov_size(const struct iovec *iov, const unsigned int iov_cnt)
{
    size_t len;
    unsigned int i;

    len = 0;
    for (i = 0; i < iov_cnt; i++) {
        len += iov[i].iov_len;
    }
    return len;
}

static size_t video_iov_to_buf(const struct iovec *iov, const unsigned int iov_cnt,
                               size_t offset, void *buf, size_t bytes)
{
    size_t done;
    unsigned int i;
    for (i = 0, done = 0; (offset || done < bytes) && i < iov_cnt; i++) {
        if (offset < iov[i].iov_len) {
            size_t len = MIN(iov[i].iov_len - offset, bytes - done);
            memcpy(buf + done, iov[i].iov_base + offset, len);
            done += len;
            offset = 0;
        } else {
            offset -= iov[i].iov_len;
        }
    }
    assert(offset == 0);
    return done;
}

static size_t video_iov_from_buf(const struct iovec *iov, unsigned int iov_cnt,
                                 size_t offset, const void *buf, size_t bytes)
{
    size_t done;
    unsigned int i;
    for (i = 0, done = 0; (offset || done < bytes) && i < iov_cnt; i++) {
        if (offset < iov[i].iov_len) {
            size_t len = MIN(iov[i].iov_len - offset, bytes - done);
            memcpy(iov[i].iov_base + offset, buf + done, len);
            done += len;
            offset = 0;
        } else {
            offset -= iov[i].iov_len;
        }
    }
    assert(offset == 0);
    return done;
}

/*
 * Structure to track internal state of VIDEO Device
 */

typedef struct VuVideo {
    VugDev dev;
    struct virtio_video_config virtio_config;
    GMainLoop *loop;
    struct v4l2_device *v4l2_dev;
} VuVideo;

static void video_panic(VuDev *dev, const char *msg)
{
    g_critical("%s\n", msg);
    exit(EXIT_FAILURE);
}

static uint64_t video_get_features(VuDev *dev)
{
    g_info("%s: replying", __func__);
    return 0;
}

static void video_set_features(VuDev *dev, uint64_t features)
{
    if (features) {
        g_autoptr(GString) s = g_string_new("Requested un-handled feature");
        g_string_append_printf(s, " 0x%" PRIx64 "", features);
        g_info("%s: %s", __func__, s->str);
    }
}

/*
 * The configuration of the device is static and set when we start the
 * daemon.
 */
static int
video_get_config(VuDev *dev, uint8_t *config, uint32_t len)
{
    VuVideo *v = container_of(dev, VuVideo, dev.parent);
    g_debug("%s: ", __func__);

    g_return_val_if_fail(len <= sizeof(struct virtio_video_config), -1);
    /* crosvm virtio-video uses 1024 */
    v->virtio_config.version = 0;
    v->virtio_config.max_caps_length = MAX_CAPS_LEN;
    v->virtio_config.max_resp_length = MAX_CAPS_LEN;
    
    memcpy(config, &v->virtio_config, len);

    g_info("%s: len=%d", __func__, len);
    g_info("%s: config->max_caps_length = %d", __func__, ((struct virtio_video_config *)config)->max_caps_length);
    g_info("%s: config->max_caps_length = %d", __func__, ((struct virtio_video_config *)config)->max_resp_length);

    g_info("%s: done", __func__);
    return 0;
}

static int
video_set_config(VuDev *dev, const uint8_t *data,
                 uint32_t offset, uint32_t size,
                 uint32_t flags)
{
    g_debug("%s: ", __func__);
    /* ignore */
    return 0;
}

/*
 * Handlers for individual control messages
 */

static void fmt_bytes(GString *s, uint8_t *bytes, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        if (i % 16 == 0) {
            g_string_append_c(s, '\n');
        }
        g_string_append_printf(s, "%x ", bytes[i]);
    }
}

static int
handle_get_params_cmd(struct VuVideo *v, struct virtio_video_get_params *cmd, struct virtio_video_get_params_resp *resp)
{
    int ret;
    struct v4l2_format fmt;
    enum v4l2_buf_type buf_type;

    g_debug("%s: type=0x%x", __func__, le32toh(cmd->hdr.type));
    g_debug("%s: stream_id=0x%x", __func__, le32toh(cmd->hdr.stream_id));
    g_debug("%s: queue_type = 0x%x", __func__, le32toh(cmd->queue_type));

    resp->params.queue_type = cmd->queue_type;

    buf_type = get_v4l2_buf_type( le32toh(cmd->queue_type), v->v4l2_dev->dev_type);

    ret = v4l2_video_get_format(v->v4l2_dev, buf_type, &fmt);
    if (ret < 0) {
        g_printerr("v4l2_video_get_format failed");
    }

    /* convert from v4l2 to virtio */
    v4l2_to_virtio_video_params(v->v4l2_dev, &fmt, resp);
}

static int
handle_query_capability_cmd(struct VuVideo *v, struct virtio_video_query_capability *qcmd, replybuf *rbuf)
{
    GList *fmt_l;
    int ret;

    g_debug("%s: type=0x%x", __func__, le32toh(qcmd->hdr.type));
    g_debug("%s: stream_id=0x%x", __func__, le32toh(qcmd->hdr.stream_id));
    g_debug("%s: queue_type = 0x%x", __func__, le32toh(qcmd->queue_type));

    //cap_resp->hdr.type = le32toh(qcmd->hdr.type);
    //cap_resp->hdr.stream_id = le32toh(qcmd->hdr.stream_id);
    
    if (le32toh(qcmd->queue_type) == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
        if (v->v4l2_dev->dev_type == STATEFUL_DECODER) {

            /* enumerate coded formats on OUTPUT */
            ret = video_enum_formats( v->v4l2_dev, V4L2_BUF_TYPE_VIDEO_OUTPUT, &fmt_l, false);
            if (ret < 0) {
                g_printerr("video_enum_formats failed");
                return ret;
            }
        }
        if (v->v4l2_dev->dev_type == STATEFUL_ENCODER) {
            /* TODO not implemented yet */
            g_critical("%s: TODO STATEFUL_ENCODER not implemented!", __func__);
        }
        /* I think this means 'input queue' to the device e.g. coded data for a decoder or raw data for an encoder */

    } else if ((le32toh(qcmd->queue_type) == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT)) {
        if (v->v4l2_dev->dev_type == STATEFUL_DECODER) {

            /* enumerate coded formats on INPUT */
            ret = video_enum_formats( v->v4l2_dev, V4L2_BUF_TYPE_VIDEO_CAPTURE, &fmt_l, false);
            if (ret < 0) {
                g_printerr("video_enum_formats failed");
                return ret;
            }
        }
        if (v->v4l2_dev->dev_type == STATEFUL_ENCODER) {
            /* TODO not implemented yet */
            g_critical("%s: TODO STATEFUL_ENCODER support not implemented!", __func__);
        }
    }

    create_query_cap_resp(qcmd, &fmt_l, rbuf);

    video_free_formats(&fmt_l);

    return 0;
}

/* for v3 virtio-video spec currently */

static void
video_handle_ctrl(VuDev *dev, int qidx)
{
    VuVirtq *vq = vu_get_queue(dev, qidx);
    VuVideo *video = container_of(dev, VuVideo, dev.parent);
    struct virtio_video_cmd_hdr *cmd_hdr = NULL;
    size_t len;
    replybuf response;

    for (;;) {
        VuVirtqElement *elem;

        elem = vu_queue_pop(dev, vq, sizeof(struct VuVirtqElement));
        if (!elem) {
            break;
        }
        g_debug("%s: got queue (in %d, out %d)", __func__,
                elem->in_num, elem->out_num);

        len = video_iov_size(elem->out_sg, elem->out_num);
        g_debug("%s: len=%ld", __func__, len);
        cmd_hdr = g_realloc(cmd_hdr, len);
        len = video_iov_to_buf(elem->out_sg, elem->out_num,
                         0, cmd_hdr, len);

        if (len != sizeof(cmd_hdr)) {
            g_warning("%s: command hdr size incorrect %zu vs %zu\n",
                      __func__, len, sizeof(cmd_hdr));
        }
        //video_process_cmd(video, cmd_hdr);
        g_debug("%s: cmd_hdr=%p",__func__, cmd_hdr);

        switch (le32toh(cmd_hdr->type)) {
        case VIRTIO_VIDEO_CMD_QUERY_CAPABILITY:
            g_debug("VIRTIO_VIDEO_CMD_QUERY_CAPABILITY");

            response.buf_base = g_malloc(MAX_CAPS_LEN);
            response.buf_pos = response.buf_base;
            handle_query_capability_cmd(video, (struct virtio_video_query_capability *) cmd_hdr, &response);
            
            if (response.replysize > 0) {
                g_debug("VIRTIO_VIDEO_CMD_QUERY_CAPABILITY: Sending response=0x%p size=%zu", response.buf_base, response.replysize);
                len = video_iov_from_buf(elem->in_sg,
                                         elem->in_num, 0, response.buf_base, response.replysize);

                if (len != response.replysize) {
                    g_critical("%s: response size incorrect %zu vs %zu",
                               __func__, len, response.replysize);
                }
                vu_queue_push(dev, vq, elem, len);
                vu_queue_notify(dev, vq);
            }

            g_free(response.buf_base);
            break;

        case VIRTIO_VIDEO_CMD_STREAM_CREATE:
            g_debug("VIRTIO_VIDEO_CMD_QUERY_CAPABILITY");
        break;
        case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
            g_debug("VIRTIO_VIDEO_CMD_STREAM_DESTROY");
            break;
        case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
            g_debug("VIRTIO_VIDEO_CMD_STREAM_DRAIN");
        case VIRTIO_VIDEO_CMD_RESOURCE_CREATE:
            g_debug("VIRTIO_VIDEO_CMD_RESOURCE_CREATE");
            break;
        case VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL:
            g_debug("VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL");
            break;
        case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
            g_debug("VIRTIO_VIDEO_CMD_QUEUE_CLEAR");
            break;
        case VIRTIO_VIDEO_CMD_GET_PARAMS:
            g_debug("VIRTIO_VIDEO_CMD_GET_PARAMS");

            struct virtio_video_get_params_resp *params_reply;
            params_reply = g_new0(struct virtio_video_get_params_resp, 1);
            
            handle_get_params_cmd(video, (struct virtio_video_get_params *) cmd_hdr, params_reply);

            len = video_iov_from_buf(elem->in_sg,
                                     elem->in_num, 0, (void*) params_reply,
                                     sizeof(struct virtio_video_get_params_resp));

            if (len != sizeof(struct virtio_video_get_params_resp)) {
                    g_critical("%s: response size incorrect %zu vs %zu",
                               __func__, len, sizeof(params_reply));
            }
            vu_queue_push(dev, vq, elem, len);
            vu_queue_notify(dev, vq);
            g_free(params_reply);

            break;
        case VIRTIO_VIDEO_CMD_SET_PARAMS:
            g_debug("VIRTIO_VIDEO_CMD_SET_PARAMS");
            break;
        case VIRTIO_VIDEO_CMD_QUERY_CONTROL:
            g_debug("VIRTIO_VIDEO_CMD_QUERY_CONTROL");
            break;
        case VIRTIO_VIDEO_CMD_GET_CONTROL:
            g_debug("VIRTIO_VIDEO_CMD_GET_CONTROL");
            break;
        case VIRTIO_VIDEO_CMD_SET_CONTROL:
            g_debug("VIRTIO_VIDEO_CMD_SET_CONTROL");
            break;
            
        default:
            g_debug("Unhandled VIRTIO VIDEO command!");
            break;
        }
    }
}

static void
video_queue_set_started(VuDev *dev, int qidx, bool started)
{
    VuVirtq *vq = vu_get_queue(dev, qidx);

    g_debug("queue started %d:%d\n", qidx, started);

    switch (qidx) {
    case 0:
        vu_set_queue_handler(dev, vq, started ? video_handle_ctrl : NULL);
        break;
    default:
        break;
    }
}

/*
 * video_process_msg: process messages of vhost-user interface
 *
 * Any that are not handled here are processed by the libvhost library
 * itself.
 */
static int video_process_msg(VuDev *dev, VhostUserMsg *msg, int *do_reply)
{
    VuVideo *r = container_of(dev, VuVideo, dev.parent);

    g_info("%s: msg %d", __func__, msg->request);

    switch (msg->request) {
    case VHOST_USER_NONE:
        g_main_loop_quit(r->loop);
        return 1;
    default:
        return 0;
    }

    return 0;
}

static const VuDevIface vuiface = {
    .set_features = video_set_features,
    .get_features = video_get_features,
    .queue_set_started = video_queue_set_started,
    .process_msg = video_process_msg,
    .get_config = video_get_config,
    .set_config = video_set_config,
};

static void video_destroy(VuVideo *v)
{
    vug_deinit(&v->dev);
    if (socket_path) {
        unlink(socket_path);
    }
}

/* Print vhost-user.json backend program capabilities */
static void print_capabilities(void)
{
    printf("{\n");
    printf("  \"type\": \"misc\"\n");
    printf("}\n");
}

static gboolean hangup(gpointer user_data)
{
    GMainLoop *loop = (GMainLoop *) user_data;
    g_info("%s: caught hangup/quit signal, quitting main loop", __func__);
    g_main_loop_quit(loop);
    return true;
}

int main(int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;
    g_autoptr(GSocket) socket = NULL;
    VuVideo video = {  };

    context = g_option_context_new ("vhost-user emulation of video device");
    g_option_context_add_main_entries (context, options, "vhost-user-video");
    if (!g_option_context_parse (context, &argc, &argv, &error))
    {
        g_printerr ("option parsing failed: %s\n", error->message);
        exit (1);
    }

    if (print_cap) {
        print_capabilities();
        exit(0);
    }

    if (!socket_path && socket_fd < 0) {
        g_printerr("Please specify either --fd or --socket-path\n");
        exit(EXIT_FAILURE);
    }

    if (verbose || debug) {
        g_log_set_handler(NULL, G_LOG_LEVEL_MASK, g_log_default_handler, NULL);
        if (debug) {
            g_setenv("G_MESSAGES_DEBUG", "all", true);
        }
    } else {
        g_log_set_handler(NULL,
                          G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR,
                          g_log_default_handler, NULL);
    }

    /*
    * Open the v4l2 device and enumerate supported formats.
    * Use this to determine whether it is a stateful encoder/decoder.
    */ 
    if (!v4l2_path || !g_file_test(v4l2_path, G_FILE_TEST_EXISTS)) {
        g_printerr("Please specify a valid --v4l2-device for the v4l2 device node\n");
        exit(EXIT_FAILURE);
    } else {
        video.v4l2_dev = v4l2_backend_init(v4l2_path);
        if (!video.v4l2_dev) {
            g_printerr("v4l2 backend init failed!\n");
            exit(EXIT_FAILURE);
        }
    }

    /*
     * Now create a vhost-user socket that we will receive messages
     * on. Once we have our handler set up we can enter the glib main
     * loop.
     */
    if (socket_path) {
        g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(socket_path);
        g_autoptr(GSocket) bind_socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                                      G_SOCKET_PROTOCOL_DEFAULT, &error);

        if (!g_socket_bind(bind_socket, addr, false, &error)) {
            g_printerr("Failed to bind to socket at %s (%s).\n",
                       socket_path, error->message);
            exit(EXIT_FAILURE);
        }
        if (!g_socket_listen(bind_socket, &error)) {
            g_printerr("Failed to listen on socket %s (%s).\n",
                       socket_path, error->message);
        }
        g_message("awaiting connection to %s", socket_path);
        socket = g_socket_accept(bind_socket, NULL, &error);
        if (!socket) {
            g_printerr("Failed to accept on socket %s (%s).\n",
                       socket_path, error->message);
        }
    } else {
        socket = g_socket_new_from_fd(socket_fd, &error);
        if (!socket) {
            g_printerr("Failed to connect to FD %d (%s).\n",
                       socket_fd, error->message);
            exit(EXIT_FAILURE);
        }
    }

    /*
     * Create the main loop first so all the various sources can be
     * added. As well as catching signals we need to ensure vug_init
     * can add it's GSource watches.
     */

    video.loop = g_main_loop_new(NULL, FALSE);
    /* catch exit signals */
    g_unix_signal_add(SIGHUP, hangup, video.loop);
    g_unix_signal_add(SIGINT, hangup, video.loop);

    if (!vug_init(&video.dev, VHOST_USER_VIDEO_MAX_QUEUES, g_socket_get_fd(socket),
                  video_panic, &vuiface)) {
        g_printerr("Failed to initialize libvhost-user-glib.\n");
        exit(EXIT_FAILURE);
    }


    g_message("entering main loop, awaiting messages");
    g_main_loop_run(video.loop);
    g_message("finished main loop, cleaning up");

    g_main_loop_unref(video.loop);
    video_destroy(&video);
}
