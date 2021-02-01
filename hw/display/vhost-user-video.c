/*
 * Vhost-user VIDEO virtio device
 *
 * This is the boilerplate for instantiating a vhost-user device
 * implementing a Replay Protected Memory Block (RPMB) device. This is
 * a type of flash chip that is protected from replay attacks and used
 * for tamper resistant storage. The actual back-end for this driver
 * is the vhost-user-rpmb daemon. The code here just connects up the
 * device in QEMU and allows it to be instantiated.
 *
 * Copyright (c) 2021 Linaro Ltd
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/vhost-user-video.h"
#include "qemu/error-report.h"

/* currently there is no VIDEO enc/dec defined in Linux virtio_ids.h */
#define VIRTIO_ID_VIDEO_ENC         30
#define VIRTIO_ID_VIDEO_DEC         31

static void vhost_user_video_get_config(VirtIODevice *vdev, uint8_t *config)
{
    /* this somehow needs to come from the vhost-user daemon */
}

static void vhost_user_video_start(VirtIODevice *vdev)
{
    VHostUserVIDEO *video = VHOST_USER_VIDEO(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;
    int i;

    if (!k->set_guest_notifiers) {
        error_report("binding does not support guest notifiers");
        return;
    }

    ret = vhost_dev_enable_notifiers(&video->vhost_dev, vdev);
    if (ret < 0) {
        error_report("Error enabling host notifiers: %d", -ret);
        return;
    }

    ret = k->set_guest_notifiers(qbus->parent, video->vhost_dev.nvqs, true);
    if (ret < 0) {
        error_report("Error binding guest notifier: %d", -ret);
        goto err_host_notifiers;
    }

    video->vhost_dev.acked_features = vdev->guest_features;

    //TODO PAG prepare_inflight?
    
    ret = vhost_dev_start(&video->vhost_dev, vdev);
    if (ret < 0) {
        error_report("Error starting vhost-user-video: %d", -ret);
        goto err_guest_notifiers;
    }


    //TODO PAG check this
    /*
     * guest_notifier_mask/pending not used yet, so just unmask
     * everything here.  virtio-pci will do the right thing by
     * enabling/disabling irqfd.
     */
    for (i = 0; i < video->vhost_dev.nvqs; i++) {
        vhost_virtqueue_mask(&video->vhost_dev, vdev, i, false);
    }

    return;

err_guest_notifiers:
    k->set_guest_notifiers(qbus->parent, video->vhost_dev.nvqs, false);
err_host_notifiers:
    vhost_dev_disable_notifiers(&video->vhost_dev, vdev);
}

static void vhost_user_video_stop(VirtIODevice *vdev)
{
    VHostUserRPMB *video = VHOST_USER_VIDEO(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;

    if (!k->set_guest_notifiers) {
        return;
    }

    vhost_dev_stop(&video->vhost_dev, vdev);

    ret = k->set_guest_notifiers(qbus->parent, video->vhost_dev.nvqs, false);
    if (ret < 0) {
        error_report("vhost guest notifier cleanup failed: %d", ret);
        return;
    }

    vhost_dev_disable_notifiers(&rpmb->vhost_dev, vdev);
}

static void vhost_user_video_set_status(VirtIODevice *vdev, uint8_t status)
{
    VHostUserRPMB *video = VHOST_USER_VIDEO(vdev);
    bool should_start = status & VIRTIO_CONFIG_S_DRIVER_OK;

    if (!vdev->vm_running) {
        should_start = false;
    }

    if (video->vhost_dev.started == should_start) {
        return;
    }

    if (should_start) {
        vhost_user_video_start(vdev);
    } else {
        vhost_user_video_stop(vdev);
    }
}

static uint64_t vhost_user_video_get_features(VirtIODevice *vdev,
                                      uint64_t requested_features,
                                      Error **errp)
{

    /* TODO PAG 
       VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES
       VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG
       VIRTIO_VIDEO_F_RESOURCE_VIRTIO_OBJECT */

    /* 5.14.3.1 The device MUST present at least one of: 
       VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES or
       VIRTIO_VIDEO_F_RESOURCE_VIRTIO_OBJECT */

    /* for now just set guest pages */
    virtio_add_feature(&features, VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES);
    
    return requested_features;
}

static void vhost_user_video_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    /*
     * Not normally called; it's the daemon that handles the queue;
     * however virtio's cleanup path can call this.
     */

    /* TODO PAG check if needed */
}

static void vhost_user_video_guest_notifier_mask(VirtIODevice *vdev, int idx,
                                            bool mask)
{
    VHostUserVIDEO *video = VHOST_USER_VIDEO(vdev);
    vhost_virtqueue_mask(&video->vhost_dev, vdev, idx, mask);
}

static bool vhost_user_video_guest_notifier_pending(VirtIODevice *vdev, int idx)
{
    VHostUserVIDEO *video = VHOST_USER_VIDEO(vdev);
    return vhost_virtqueue_pending(&video->vhost_dev, idx);
}

/*
 * Chardev connect/disconnect events
 */

static int vhost_user_video_handle_config_change(struct vhost_dev *dev)
{
    int ret;
    VHostUserVIDEO *video = VHOST_USER_VIDEO(dev->vdev);

    ret = vhost_dev_get_config(dev, (uint8_t *)&video->conf.config,
                               sizeof(struct virtio_video_config));
    if (ret < 0) {
        error_report("get config space failed");
        return -1;
    }

    return 0;
}

const VhostDevConfigOps video_ops = {
    .vhost_dev_config_notifier = vhost_user_video_handle_config_change,
};

static int vhost_user_video_connect(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserRPMB *video = VHOST_USER_RPMB(vdev);

    if (video->connected) {
        return 0;
    }
    video->connected = true;

    /* restore vhost state */
    if (virtio_device_started(vdev, vdev->status)) {
        vhost_user_video_start(vdev);
    }

    return 0;
}

static void vhost_user_video_disconnect(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserVIDEO *video = VHOST_USER_VIDEO(vdev);

    if (!video->connected) {
        return;
    }
    video->connected = false;

    if (video->vhost_dev.started) {
        vhost_user_video_stop(vdev);
    }

    vhost_dev_cleanup(&video->vhost_dev);
}

static void vhost_user_video_event(void *opaque, QEMUChrEvent event)
{
    DeviceState *dev = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserVIDEO *video = VHOST_USER_VIDEO(vdev);

    switch (event) {
    case CHR_EVENT_OPENED:
        if (vhost_user_video_connect(dev) {
            qemu_chr_fe_disconnect(&video->conf.chardev);
            return;
        }
        break;
    case CHR_EVENT_CLOSED:
        vhost_user_video_disconnect(dev);
        break;
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        /* Ignore */
        break;
    }
}

static void do_vhost_user_cleanup(VirtIODevice *vdev, VHostUserVIDEO *video)
{
    vhost_user_cleanup(&video->vhost_user);
    virtio_delete_queue(video->command_vq);
    virtio_delete_queue(video->event_vq);
    virtio_cleanup(vdev);
    g_free(video->vhost_dev.vqs);
    video->vhost_dev.vqs = NULL;
}


static void vhost_user_video_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserVIDEO *video = VHOST_USER_VIDEO(dev);
    int ret;

    if (!vhost_user_init(&video->vhost_user, &video->conf.chardev, errp)) {
        return;
    }

    /* TODO hardcode to VIDEO_DEC, but we need to figure out somehow if we
       are video encode or decode */
    virtio_init(vdev, "vhost-user-video", VIRTIO_ID_VIDEO_DEC,
                sizeof(struct virtio_video_config));

    /* One command queue, for sending commands */
    video->command_vq = virtio_add_queue(vdev, 128, vhost_user_video_handle_output);
    /* One event queue, for sending events events */
    video->event_vq = virtio_add_queue(vdev, 128, vhost_user_video_handle_output);

    /* one command queue and one event queue */
    video->vhost_dev.nvqs = 2;
    video->vhost_dev.vqs = g_new0(struct vhost_virtqueue, video->vhost_dev.nvqs);
    ret = vhost_dev_init(&video->vhost_dev, &video->vhost_user,
                         VHOST_BACKEND_TYPE_USER, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "vhost_dev_init failed");
        do_vhost_user_cleanup(vdev, video);
    }

    /*
     * At this point the next event we will get is a connection from
     * the daemon on the control socket.
     */

    qemu_chr_fe_set_handlers(&video->conf.chardev,  NULL, NULL, vhost_user_video_event,
                             NULL, (void *)dev, NULL, true);

    return;
}

static void vurpmb_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserRPMB *video = VHOST_USER_VIDEO(dev);

    /* This will stop vhost backend if appropriate. */
    vhost_user_video_set_status(vdev, 0);

    do_vhost_user_cleanup(vdev, video);
}

static const VMStateDescription vhost_user_video_vmstate = {
    .name = "vhost-user-video",
    .unmigratable = 1,
};

static Property vhost_user_video_properties[] = {
    DEFINE_PROP_CHR("chardev", VHostUserVIDEO, conf.chardev),
    DEFINE_PROP_END_OF_LIST(),
};

static void vhost_user_video_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, vhost_user_video_properties);
    dc->vmsd = &vhost_user_video_vmstate;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = vhost_user_video_device_realize;
    vdc->unrealize = vhost_user_video_device_unrealize;
    vdc->get_features = vhost_user_video_get_features;
    vdc->get_config = vhost_user_video_get_config;
    vdc->set_status = vhost_user_video_set_status;
    vdc->guest_notifier_mask = vhost_user_video_guest_notifier_mask;
    vdc->guest_notifier_pending = vhost_user_video_guest_notifier_pending;
}

static const TypeInfo vhost_user_video_info = {
    .name = TYPE_VHOST_USER_VIDEO,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VHostUserVIDEO),
    .class_init = vhost_user_video_class_init,
};

static void vhost_user_video_register_types(void)
{
    type_register_static(&vhost_user_video_info);
}

type_init(vhost_user_video_register_types)
