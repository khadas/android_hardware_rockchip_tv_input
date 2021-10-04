/*
 * Copyright (c) 2021 Rockchip Electronics Co., Ltd
 */

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>

#include <cutils/native_handle.h>
#include <log/log.h>
#include <cutils/log.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicBuffer.h>
#include <gralloc_priv.h>
#include <gralloc_helper.h>

#include <linux/videodev2.h>
#include <android/native_window.h>
#include <hardware/hardware.h>
#include <hardware/tv_input.h>
#include "TvDeviceV4L2Event.h"
#include "HinDev.h"
#include "Utils.h"

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG "tv_input"
#endif

using namespace android;
#define UNUSED(x) (void *)x

#ifndef container_of
#define container_of(ptr, type, member) \
    (type *)((char*)(ptr) - offsetof(type, member))
#endif

#define MAX_HIN_DEVICE_SUPPORTED    10

#define STREAM_ID_GENERIC       1
#define STREAM_ID_FRAME_CAPTURE 2

// tv input source type
typedef enum tv_input_source_type {
    SOURCE_INVALID = -1,
    SOURCE_HDMI1 = 0,
    SOURCE_HDMI2,
    SOURCE_TV,
    SOURCE_DTV,
    SOURCE_MAX,
} tv_input_source_t;


typedef struct tv_input_private {
    tv_input_device_t device;

    // Callback related data
    const tv_input_callback_ops_t* callback;
    void* callback_data;
    HinDevImpl* mDev;
} tv_input_private_t;
static tv_input_private_t *s_TvInputPriv;
static int s_HinDevStreamWidth = 0;
static int s_HinDevStreamHeight = 0;

//static unsigned int gHinDevOpened = 0;
//static Mutex gHinDevOpenLock;
//static HinDevImpl* gHinHals[MAX_HIN_DEVICE_SUPPORTED];

//static native_handle_t *pTvStream = NULL;

/*****************************************************************************/

static int tv_input_device_open(const struct hw_module_t* module,
        const char* name, struct hw_device_t** device);

static struct hw_module_methods_t tv_input_module_methods = {
    .open = tv_input_device_open
};

tv_input_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = TV_INPUT_HARDWARE_MODULE_ID,
        .name = "Rockchip TV input module",
        .author = "Rockchip",
        .methods = &tv_input_module_methods,
    }
};

V4L2EventCallBack hinDevEventCallback(int width, int height,int isHdmiIn) {
    if (s_HinDevStreamWidth == width || s_HinDevStreamHeight == height)
        return 0;
    ALOGE("%s %d,%d,%d", __FUNCTION__, width,height,isHdmiIn);
    tv_input_event_t event;
    event.device_info.device_id = SOURCE_HDMI1;
    event.device_info.type = TV_INPUT_TYPE_HDMI;
    event.device_info.audio_type = AUDIO_DEVICE_NONE;
    event.device_info.audio_address = NULL;
    if (isHdmiIn == 1) {
        event.type = TV_INPUT_EVENT_STREAM_CONFIGURATIONS_CHANGED;
        s_HinDevStreamWidth = width;
        s_HinDevStreamHeight = height;
    } else {
        event.type = TV_INPUT_EVENT_DEVICE_UNAVAILABLE;
    }
    s_TvInputPriv->callback->notify(nullptr, &event, nullptr);

    return 0;
}

static int getHdmiPortID(tv_input_source_t source_type) {
    int port_id = 0;
    switch (source_type) {
    case SOURCE_HDMI1:
        port_id = 1;
        break;
    case SOURCE_HDMI2:
        port_id = 2;
        break;
    default:
        break;
    }
    return port_id;
}

/*****************************************************************************/
static int generateEvent(tv_input_private_t *priv, tv_input_source_t source_type, int event_type) {
    tv_input_event_t event;
    event.device_info.device_id = source_type;
    event.device_info.audio_type = AUDIO_DEVICE_NONE;
    event.device_info.audio_address = NULL;
    event.type = event_type;
    switch (source_type) {
        case SOURCE_HDMI1:
        case SOURCE_HDMI2:
            event.device_info.type = TV_INPUT_TYPE_HDMI;
            event.device_info.hdmi.port_id = getHdmiPortID(source_type);
            break;
        case SOURCE_TV:
        case SOURCE_DTV:
            event.device_info.type = TV_INPUT_TYPE_TUNER;
            break;
        default:
            break;
    }
    static int connection_status = 1;
    //priv->callback->notify(&priv->device, &event, priv->callback_data);
    // TODO: data --> connection status
    priv->callback->notify(&priv->device, &event, &connection_status);
    return 0;
}


/**
 * List all of the devices, may register hotplug listener here.
 * */
void findTvDevices(tv_input_private_t *priv) {
    generateEvent(priv, SOURCE_HDMI1, TV_INPUT_EVENT_DEVICE_AVAILABLE);
    generateEvent(priv, SOURCE_HDMI1, TV_INPUT_EVENT_STREAM_CONFIGURATIONS_CHANGED);
    generateEvent(priv, SOURCE_DTV, TV_INPUT_EVENT_DEVICE_AVAILABLE);
    generateEvent(priv, SOURCE_DTV, TV_INPUT_EVENT_STREAM_CONFIGURATIONS_CHANGED);
}

#define NUM_OF_CONFIGS_DEFAULT 2
static tv_stream_config_t mconfig[NUM_OF_CONFIGS_DEFAULT];
static int tv_input_get_stream_configurations(
        const struct tv_input_device *dev, int device_id, int *num_of_configs, const tv_stream_config_t **configs)
{
    ALOGD("%s called device_id=%d", __func__, device_id);
    UNUSED(dev);
    if (device_id == -1) {
        *num_of_configs = -1;
    }
    
    switch (device_id) {
    case SOURCE_TV:
    case SOURCE_DTV:
    case SOURCE_HDMI1:
    case SOURCE_HDMI2:
        mconfig[0].stream_id = STREAM_ID_GENERIC;
        mconfig[0].type = TV_STREAM_TYPE_BUFFER_PRODUCER;
        mconfig[0].max_video_width = s_HinDevStreamWidth;
        mconfig[0].max_video_height = s_HinDevStreamHeight;

        mconfig[1].stream_id = STREAM_ID_FRAME_CAPTURE;
        mconfig[1].type = TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE;
        mconfig[1].max_video_width = s_HinDevStreamWidth;
        mconfig[1].max_video_height = s_HinDevStreamHeight;
        *num_of_configs = NUM_OF_CONFIGS_DEFAULT;
        *configs = mconfig;
        break;
    default:
        break;
    }
    return 0;
}

static int hin_dev_open(int deviceId)
{
    ALOGD("hin_dev_open");
    HinDevImpl* hinDevImpl = NULL;
    //Mutex::Autolock lock(gHinDevOpenLock);

    if (deviceId != -1) {
        if (deviceId >=  MAX_HIN_DEVICE_SUPPORTED) {
            ALOGD("provided device id out of bounds , deviceid = %d .\n" , deviceId);
            return -EINVAL;
        }

        if (!s_TvInputPriv->mDev) {
            hinDevImpl = new HinDevImpl;
            if (!hinDevImpl) {
                ALOGE("no memory to new hinDevImpl");
                return -ENOMEM;
            }
            s_TvInputPriv->mDev = hinDevImpl;
            s_TvInputPriv->mDev->set_data_callback((V4L2EventCallBack)hinDevEventCallback);
            usleep(10*1000);
        }

        if (s_TvInputPriv->mDev->init(deviceId, s_HinDevStreamWidth, s_HinDevStreamHeight)!= 0) {
            ALOGE("hinDevImpl->init %d failed!", deviceId);
            delete s_TvInputPriv->mDev;
            return -1;
        }

    }
    return 0;
}

static int tv_input_open_stream(struct tv_input_device *dev, int device_id, tv_stream_t *stream)
{
    ALOGD("func: %s, device_id: %d, stream_id=%d, type=%d", __func__, device_id, stream->stream_id, stream->type);
    if (s_TvInputPriv) {

        if (hin_dev_open(device_id) < 0) {
            ALOGD("Open hdmi failed!!!\n");
            return -EINVAL;
        }

        if (s_TvInputPriv->mDev) {
            int width = 0, height = 0;
            char prop_value[PROPERTY_VALUE_MAX] = {0};
            property_get(TV_INPUT_USER_FORMAT, prop_value, "default");
            if (strcmp(prop_value, "default") != 0) {
                sscanf(prop_value, "%dx%d", &width, &height);
                DEBUG_PRINT(1, "user format = %s, width=%d, height=%d\n", prop_value, width, height);
            } else {
                width = s_HinDevStreamWidth;
                height = s_HinDevStreamHeight;
            }

            s_TvInputPriv->mDev->set_format(width, height, DEFAULT_V4L2_STREAM_FORMAT);
            s_TvInputPriv->mDev->set_crop(0, 0, width, height);

            stream->type = TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE;
            stream->sideband_stream_source_handle = native_handle_clone(s_TvInputPriv->mDev->getSindebandBufferHandle());

            s_TvInputPriv->mDev->start();
        }
        return 0;
    }
    return -EINVAL;
}

static int tv_input_close_stream(struct tv_input_device *dev, int device_id, int stream_id)
{
    ALOGD("func: %s, device_id: %d, stream_id: %d", __func__, device_id, stream_id);
    if (s_TvInputPriv) {
        if (s_TvInputPriv->mDev) {
            s_TvInputPriv->mDev->stop();
            return 0;
        }
    }
    return -EINVAL;
}

static int tv_input_request_capture(struct tv_input_device* dev, int device_id,
            int stream_id, buffer_handle_t buffer, uint32_t seq)
{
    ALOGD("%s called", __func__);
 #if 0
    tv_input_private_t *priv = (tv_input_private_t *)dev;
    unsigned char *dest = NULL;
    if (priv->mDev) {
        source_buffer_info_t buffInfo;
        int ret = priv->mDev->aquire_buffer(&buffInfo);
        if (ret != 0 || buffInfo.buffer_mem == nullptr) {
            ALOGD("aquire_buffer FAILED!!!");
            notifyCaptureFail(priv,device_id,stream_id,--seq);
            return -EWOULDBLOCK;
        }
        long *src = (long*)buffInfo.buffer_mem;

        ANativeWindowBuffer *buf = container_of(buffer, ANativeWindowBuffer, handle);
        sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(buf->handle, GraphicBuffer::WRAP_HANDLE,
            buf->width, buf->height, buf->format, buf->layerCount, buf->usage, buf->stride));

        graphicBuffer->lock(SCREENSOURCE_GRALLOC_USAGE, (void **)&dest);
        if (dest == NULL) {
            ALOGD("grallocBuffer->lock FAILED!!!");
            return -EWOULDBLOCK;
        }
        memcpy(dest, src, DEFAULT_CAPTURE_WIDTH*DEFAULT_CAPTURE_HEIGHT);
        graphicBuffer->unlock();
        graphicBuffer.clear();
        priv->mDev->release_buffer(src);

        notifyCaptureSucceeded(priv, device_id, stream_id, seq);
        return 0;
    }
#endif
    return -EINVAL;
}

static int tv_input_cancel_capture(struct tv_input_device*, int, int, uint32_t)
{
    ALOGD("%s called", __func__);
    return -EINVAL;
}

/*****************************************************************************/

static int tv_input_device_close(struct hw_device_t *dev)
{
    ALOGD("%s called", __func__);
    if (s_TvInputPriv) {
        if (s_TvInputPriv->mDev) {
            delete s_TvInputPriv->mDev;
            s_TvInputPriv->mDev = nullptr;
        }
        free(s_TvInputPriv);
    }
    return 0;
}

static int tv_input_initialize(struct tv_input_device* dev,
        const tv_input_callback_ops_t* callback, void* data)
{
    ALOGD("%s called", __func__);
    if (dev == NULL || callback == NULL) {
        return -EINVAL;
    }
    s_TvInputPriv = (tv_input_private_t*)dev;

    s_TvInputPriv->callback = callback;
    s_TvInputPriv->callback_data = data;
    
    findTvDevices(s_TvInputPriv);
    return 0;
}

/*****************************************************************************/

static int tv_input_device_open(const struct hw_module_t* module,
        const char* name, struct hw_device_t** device)
{
    ALOGD("%s name: %s", __func__, name);
    int status = -EINVAL;
    if (!strcmp(name, TV_INPUT_DEFAULT_DEVICE)) {
        tv_input_private_t* dev = (tv_input_private_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = TV_INPUT_DEVICE_API_VERSION_0_1;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = tv_input_device_close;

        dev->device.initialize = tv_input_initialize;
        dev->device.get_stream_configurations =
                tv_input_get_stream_configurations;
        dev->device.open_stream = tv_input_open_stream;
        dev->device.close_stream = tv_input_close_stream;
        dev->device.request_capture = tv_input_request_capture;
        dev->device.cancel_capture = tv_input_cancel_capture;

        *device = &dev->device.common;
        status = 0;
        ALOGD("%s name: %s %d", __func__, name, status);
    }
    return status;
}

