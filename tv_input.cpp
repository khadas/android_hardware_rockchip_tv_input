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
//#include <gralloc_priv.h>
//#include <gralloc_helper.h>

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
    HinDevImpl* mDev;
    int mStreamType;
    bool isOpened;
    bool isInitialized;
} tv_input_private_t;

typedef struct tv_input_request_info {
    int deviceId;
    int streamId;
    int seq;
} tv_input_request_info_t;

static tv_input_private_t *s_TvInputPriv;
static tv_input_request_info_t requestInfo;
static int s_HinDevStreamWidth = 1280;
static int s_HinDevStreamHeight = 720;
static int s_HinDevStreamFormat = DEFAULT_TVHAL_STREAM_FORMAT;
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

V4L2EventCallBack hinDevEventCallback(int event_type) {
    ALOGD("%s event type: %d", __FUNCTION__,event_type);
    bool isHdmiIn;
    tv_input_event_t event;
    if (s_TvInputPriv && !s_TvInputPriv->isOpened) {
       ALOGE("%s The device is not open ", __FUNCTION__);
       return 0;
    }
    switch (event_type) {
	case V4L2_EVENT_CTRL:
             isHdmiIn = s_TvInputPriv->mDev->get_HdmiIn(false);
	     /*if(!isHdmiIn)
		event.type = TV_INPUT_EVENT_DEVICE_UNAVAILABLE;
	     else
		event.type = TV_INPUT_EVENT_DEVICE_AVAILABLE;
             */
             break;
        case V4L2_EVENT_SOURCE_CHANGE:
             isHdmiIn = s_TvInputPriv->mDev->get_current_sourcesize(s_HinDevStreamWidth, s_HinDevStreamHeight,s_HinDevStreamFormat);
             event.type = TV_INPUT_EVENT_STREAM_CONFIGURATIONS_CHANGED;
	     break;
    }
    ALOGE("%s width:%d,height:%d,format:0x%x,%d", __FUNCTION__,s_HinDevStreamWidth,s_HinDevStreamHeight,s_HinDevStreamFormat,isHdmiIn);
    event.device_info.device_id = SOURCE_HDMI1;
    event.device_info.type = TV_INPUT_TYPE_HDMI;
    event.device_info.audio_type = AUDIO_DEVICE_NONE;
    event.device_info.audio_address = NULL;
    if(event.type > 0)
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

static int hin_dev_open(int deviceId, int type)
{   
    ALOGD("hin_dev_open deviceId:=%d",deviceId); 
    HinDevImpl* hinDevImpl = NULL;
    //Mutex::Autolock lock(gHinDevOpenLock);
    if (!s_TvInputPriv->isOpened && deviceId == SOURCE_DTV) {
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
            if (s_TvInputPriv->mDev->findDevice(deviceId, s_HinDevStreamWidth, s_HinDevStreamHeight,s_HinDevStreamFormat)!= 0) {
                ALOGE("hinDevImpl->findDevice %d failed!", deviceId);
                delete s_TvInputPriv->mDev;
	        s_TvInputPriv->mDev = nullptr;
                return -1;
            }
            ALOGD("hinDevImpl->findDevice %d ,%d,0x%x,0x%x!", s_HinDevStreamWidth,s_HinDevStreamHeight,s_HinDevStreamFormat,DEFAULT_V4L2_STREAM_FORMAT);
            s_TvInputPriv->isOpened = true;
        }
    }
    return 0;
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
static native_handle_t* out_buffer;

static int tv_input_get_stream_configurations(
        const struct tv_input_device *dev, int device_id, int *num_of_configs, const tv_stream_config_t **configs)
{
    ALOGD("%s called device_id=%d,s_TvInputPriv=%p", __func__, device_id,s_TvInputPriv);
    UNUSED(dev);
    if (device_id == -1) {
        *num_of_configs = -1;
    }
	if (hin_dev_open(device_id, 0) < 0) {
		ALOGD("Open hdmi failed!!!\n");
		return -EINVAL;
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
        mconfig[0].format = s_HinDevStreamFormat;//DEFAULT_TVHAL_STREAM_FORMAT;
        mconfig[0].width = s_HinDevStreamWidth;
        mconfig[0].height = s_HinDevStreamHeight;
        mconfig[0].usage = STREAM_BUFFER_GRALLOC_USAGE;
        mconfig[0].buffCount = APP_PREVIEW_BUFF_CNT;

        mconfig[1].stream_id = STREAM_ID_FRAME_CAPTURE;
        mconfig[1].type = TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE;
        mconfig[1].max_video_width = s_HinDevStreamWidth;
        mconfig[1].max_video_height = s_HinDevStreamHeight;
        mconfig[1].format = s_HinDevStreamFormat;//DEFAULT_TVHAL_STREAM_FORMAT;
        mconfig[1].width = s_HinDevStreamWidth;
        mconfig[1].height = s_HinDevStreamWidth;
        mconfig[1].usage = STREAM_BUFFER_GRALLOC_USAGE;
        mconfig[1].buffCount = APP_PREVIEW_BUFF_CNT;
        *num_of_configs = NUM_OF_CONFIGS_DEFAULT;
        *configs = mconfig;
        ALOGE("config %d ,%d,0x%x,0x%x!", s_HinDevStreamWidth,s_HinDevStreamHeight,s_HinDevStreamFormat,DEFAULT_V4L2_STREAM_FORMAT);
        break;
    default:
        break;
    }
    return 0;
}

static int tv_input_open_stream(struct tv_input_device *dev, int device_id, tv_stream_t *stream)
{
    ALOGD("func: %s, device_id: %d, stream_id=%d, type=%d", __func__, device_id, stream->stream_id, stream->type);
    if (s_TvInputPriv) {
        if (s_TvInputPriv->mDev && s_TvInputPriv->isInitialized) {
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
            requestInfo.streamId = stream->stream_id;

            if(s_TvInputPriv->mDev->set_format(width, height, s_HinDevStreamFormat))
		return -EINVAL;
            s_TvInputPriv->mDev->set_crop(0, 0, width, height);
            if (stream->type & TYPF_SIDEBAND_WINDOW) {
                s_TvInputPriv->mStreamType = TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE;
                stream->sideband_stream_source_handle = native_handle_clone(s_TvInputPriv->mDev->getSindebandBufferHandle());
                out_buffer = stream->sideband_stream_source_handle;
            }
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
            if (out_buffer) {
                native_handle_close(out_buffer);
                native_handle_delete(out_buffer);
                out_buffer=NULL;
            }

            s_TvInputPriv->mDev->stop();
            s_TvInputPriv->isInitialized = false;
            s_TvInputPriv->isOpened = false;
            s_TvInputPriv->mDev = nullptr;
            return 0;
        }
    }
    return -EINVAL;
}

NotifyQueueDataCallback dataCallback(tv_input_capture_result_t result) {
    ALOGV("%s req:%u ,%u in result.buff_id=%" PRIu64, __FUNCTION__,requestInfo.seq,result.seq, result.buff_id);
    tv_input_event_t event;
    event.capture_result.device_id = requestInfo.deviceId;
    event.capture_result.stream_id = requestInfo.streamId;
    event.capture_result.seq = requestInfo.seq++;
    if (result.buff_id != -1) {
        event.type = TV_INPUT_EVENT_CAPTURE_SUCCEEDED;
        event.capture_result.buff_id = result.buff_id;
        event.capture_result.buffer = result.buffer;
    } else {
        event.type = TV_INPUT_EVENT_CAPTURE_FAILED;
    }
    s_TvInputPriv->callback->notify(nullptr, &event, nullptr);
    return 0;
}

static int tv_input_request_capture(struct tv_input_device* dev, int device_id,
            int stream_id, uint64_t buff_id, buffer_handle_t buffer, uint32_t seq) {
    ALOGV("%s called,req=%u", __func__,seq);
    if (s_TvInputPriv && s_TvInputPriv->isInitialized && s_TvInputPriv->mDev && buffer != nullptr) {
        //requestInfo.seq = seq;
        s_TvInputPriv->mDev->set_preview_callback((NotifyQueueDataCallback)dataCallback);
        s_TvInputPriv->mDev->request_capture(buffer, buff_id);
        return 0;
    }

    return -EINVAL;
}

static int tv_input_cancel_capture(struct tv_input_device*, int, int, uint32_t)
{
    ALOGD("%s called", __func__);
    return 0;
}

static int tv_input_set_preview_info(int32_t deviceId, int32_t streamId,
            int32_t top, int32_t left, int32_t width, int32_t height, int32_t extInfo)
{
    ALOGD("%s device id %d,called,%p", __func__,deviceId,s_TvInputPriv->mDev);
    if (s_TvInputPriv && s_TvInputPriv->mDev && !s_TvInputPriv->isInitialized) {
        if (s_TvInputPriv->mDev->init(deviceId, extInfo)!= 0) {
            ALOGE("hinDevImpl->init %d failed!", deviceId);
            return -1;
        }
        s_TvInputPriv->isInitialized = true;
        requestInfo.deviceId = deviceId;
    }
    if(s_TvInputPriv->isInitialized){
    	requestInfo.deviceId = deviceId;
    	s_TvInputPriv->mDev->set_preview_info(top, left, width, height);
    	return 0;
    }
    return -1;
}

static int tv_input_set_preview_buffer(buffer_handle_t rawHandle, uint64_t bufferId)
{
    ALOGD("%s called", __func__);
    if (!s_TvInputPriv->isInitialized || !s_TvInputPriv->mDev) {
    	return -EINVAL;
    }
    s_TvInputPriv->mDev->set_preview_buffer(rawHandle, bufferId);
    return 0;
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
        s_TvInputPriv->isOpened = false;
        s_TvInputPriv->isInitialized = false;
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
//memset(s_TvInputPriv->mDev,0,sizeof(s_TvInputPriv->mDev));
    s_TvInputPriv->mDev = NULL;
    s_TvInputPriv->isOpened = false;
    s_TvInputPriv->isInitialized = false;
    s_TvInputPriv->callback = callback;
    
    findTvDevices(s_TvInputPriv);
    return 0;
}

/*****************************************************************************/

static int tv_input_device_open(const struct hw_module_t* module,
        const char* name, struct hw_device_t** device)
{
    ALOGD("%s in, name: %s", __func__, name);
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
        dev->device.set_preview_info = tv_input_set_preview_info;
        dev->device.set_preview_buffer = tv_input_set_preview_buffer;

        dev->device.request_capture = tv_input_request_capture;
        dev->device.cancel_capture = tv_input_cancel_capture;

        *device = &dev->device.common;
        status = 0;
        ALOGD("%s end. name: %s %d", __FUNCTION__, name, status);
    }
    return status;
}

