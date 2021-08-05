#define LOG_TAG "TV_INPUT_ROCKCHIP"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>

#include <cutils/native_handle.h>
#include <log/log.h>

#include <hardware/tv_input.h>

// From libhardware_rockchip_headers
#include <hardware/tv_input_source.h>

#define UNUSED(x) (void *)x
/*****************************************************************************/

typedef struct tv_input_private {
    tv_input_device_t device;

    // Callback related data
    const tv_input_callback_ops_t* callback;
    void* callback_data;
} tv_input_private_t;

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

static int tv_input_initialize(struct tv_input_device* dev,
        const tv_input_callback_ops_t* callback, void* data)
{
    ALOGE("%s called", __func__);
    if (dev == NULL || callback == NULL) {
        return -EINVAL;
    }
    tv_input_private_t* priv = (tv_input_private_t*)dev;
    if (priv->callback != NULL) {
        return -EEXIST;
    }

    priv->callback = callback;
    priv->callback_data = data;
    
    findTvDevices(priv);
    return 0;
}

#define NUM_OF_CONFIGS_DEFAULT 2
static tv_stream_config_t mconfig[NUM_OF_CONFIGS_DEFAULT];
static int tv_input_get_stream_configurations(
        const struct tv_input_device *dev, int device_id, int *num_of_configs, const tv_stream_config_t **configs)
{
    ALOGE("%s called", __func__);
    UNUSED(dev);
    switch (device_id) {
    case SOURCE_TV:
    case SOURCE_DTV:
    case SOURCE_HDMI1:
    case SOURCE_HDMI2:
        mconfig[0].stream_id = STREAM_ID_GENERIC;
        mconfig[0].type = TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE;
        mconfig[0].max_video_width = 1920;
        mconfig[0].max_video_height = 1080;
        mconfig[1].stream_id = STREAM_ID_FRAME_CAPTURE;
        mconfig[1].type = TV_STREAM_TYPE_BUFFER_PRODUCER;
        mconfig[1].max_video_width = 1920;
        mconfig[1].max_video_height = 1080;
        *num_of_configs = NUM_OF_CONFIGS_DEFAULT;
        *configs = mconfig;
        break;
    default:
        break;
    }
    return 0;
}

static int tv_input_open_stream(struct tv_input_device *dev, int device_id, tv_stream_t *stream)
{
    UNUSED(dev);
    UNUSED(stream);
    ALOGE("func: %s, device_id: %d", __func__, device_id);
    return -EINVAL;
}

static int tv_input_close_stream(struct tv_input_device *dev, int device_id, int stream_id)
{
    UNUSED(dev);
    ALOGE("func: %s, device_id: %d, stream_id: %d", __func__, device_id, stream_id);
    return -EINVAL;
}

static int tv_input_request_capture(
        struct tv_input_device*, int, int, buffer_handle_t, uint32_t)
{
    ALOGE("%s called", __func__);
    return -EINVAL;
}

static int tv_input_cancel_capture(struct tv_input_device*, int, int, uint32_t)
{
    ALOGE("%s called", __func__);
    return -EINVAL;
}

/*****************************************************************************/

static int tv_input_device_close(struct hw_device_t *dev)
{
    ALOGE("%s called", __func__);
    tv_input_private_t* priv = (tv_input_private_t*)dev;
    if (priv) {
        free(priv);
    }
    return 0;
}

/*****************************************************************************/

static int tv_input_device_open(const struct hw_module_t* module,
        const char* name, struct hw_device_t** device)
{
    ALOGE("%s name: %s", __func__, name);
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
        ALOGE("%s name: %s %d", __func__, name, status);
    }
    return status;
}
