/*
 * Copyright (c) 2021 Rockchip Electronics Co., Ltd
 */

#include <utils/Log.h>
#include <utils/String8.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <sys/time.h>
#include <utils/Timers.h>

#include <cutils/properties.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "HinDev.h"
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicBuffer.h>
#include <linux/videodev2.h>
#include <RockchipRga.h>

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG "tv_input_HinDevImpl"
#endif

#define V4L2_ROTATE_ID 0x980922

#ifndef container_of
#define container_of(ptr, type, member) ({                      \
        const typeof(((type *) 0)->member) *__mptr = (ptr);     \
        (type *) ((char *) __mptr - (char *)(&((type *)0)->member)); })
#endif

#define BOUNDRY 32
#define ALIGN_32(x) ((x + (BOUNDRY) - 1)& ~((BOUNDRY) - 1))
#define ALIGN(b,w) (((b)+((w)-1))/(w)*(w))

const int kMaxDevicePathLen = 256;
const char* kDevicePath = "/dev/";
constexpr char kPrefix[] = "video";
constexpr int kPrefixLen = sizeof(kPrefix) - 1;
//constexpr int kDevicePrefixLen = sizeof(kDevicePath) + kPrefixLen + 1;
constexpr char kHdmiNodeName[] = "rk_hdmirx";


nsecs_t now = 0;
nsecs_t mLastTime = 0;
nsecs_t diff = 0;


static v4l2_buf_type TVHAL_V4L2_BUF_TYPE = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
static size_t getBufSize(int format, int width, int height)
{
    size_t buf_size = 0;
    switch (format) {
        case V4L2_PIX_FMT_YVU420:
        case V4L2_PIX_FMT_NV21:
        case V4L2_PIX_FMT_NV12:
        {
            buf_size = width * height * 3 / 2;
            break;
        }
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_RGB565:
        case V4L2_PIX_FMT_RGB565X:
            buf_size = width * height * 2;
            break;
        case V4L2_PIX_FMT_BGR24:
            buf_size = width * height * 3;
            break;
        case V4L2_PIX_FMT_RGB32:
            buf_size = width * height * 4;
            break;
        default:
            DEBUG_PRINT(3, "Invalid format");
            buf_size = width * height * 3 / 2;
    }
    return buf_size;
}

static void two_bytes_per_pixel_memcpy_align32(unsigned char *dst, unsigned char *src, int width, int height)
{
        int stride = (width + 31) & ( ~31);
        int h;
        for (h=0; h<height; h++)
        {
                memcpy( dst, src, width*2);
                dst += width*2;
                src += stride*2;
        }
}

static void nv21_memcpy_align32(unsigned char *dst, unsigned char *src, int width, int height)
{
        int stride = (width + 31) & ( ~31);
        int h;
        for (h=0; h<height*3/2; h++)
        {
                memcpy( dst, src, width);
                dst += width;
                src += stride;
        }
}

static void yv12_memcpy_align32(unsigned char *dst, unsigned char *src, int width, int height)
{
        int new_width = (width + 63) & ( ~63);
        int stride;
        int h;
        for (h=0; h<height; h++)
        {
                memcpy( dst, src, width);
                dst += width;
                src += new_width;
        }

        stride = ALIGN(width/2, 16);
        for (h=0; h<height; h++)
        {
                memcpy( dst, src, width/2);
                dst += stride;
                src += new_width/2;
        }
}

static void rgb24_memcpy_align32(unsigned char *dst, unsigned char *src, int width, int height)
{
        int stride = (width + 31) & ( ~31);
        int  h;
        for (h=0; h<height; h++)
        {
                memcpy( dst, src, width*3);
                dst += width*3;
                src += stride*3;
        }
}

static void rgb32_memcpy_align32(unsigned char *dst, unsigned char *src, int width, int height)
{
        int stride = (width + 31) & ( ~31);
        int h;
        for (h=0; h<height; h++)
        {
                memcpy( dst, src, width*4);
                dst += width*4;
                src += stride*4;
        }
}

static int  getNativeWindowFormat(int format)
{
    int nativeFormat = -1;//HAL_PIXEL_FORMAT_YCbCr_422_I;

    switch(format){
        case V4L2_PIX_FMT_YVU420:
            nativeFormat = HAL_PIXEL_FORMAT_YV12;
            break;
        case V4L2_PIX_FMT_NV21:
            nativeFormat = HAL_PIXEL_FORMAT_YCrCb_420_SP;
            break;
        case V4L2_PIX_FMT_YUYV:
            nativeFormat = HAL_PIXEL_FORMAT_YCbCr_422_I;
            break;
        case V4L2_PIX_FMT_RGB565:
            nativeFormat = HAL_PIXEL_FORMAT_RGB_565;
            break;
        case V4L2_PIX_FMT_BGR24:
            nativeFormat = HAL_PIXEL_FORMAT_BGR_888;
            break;
        case V4L2_PIX_FMT_RGB32:
            nativeFormat = HAL_PIXEL_FORMAT_RGBA_8888;
            break;
        case V4L2_PIX_FMT_ABGR32:
            nativeFormat = HAL_PIXEL_FORMAT_BGRA_8888;
            break;
	case V4L2_PIX_FMT_NV12:
	    nativeFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;
	    break;
	case V4L2_PIX_FMT_NV16:
	    nativeFormat = HAL_PIXEL_FORMAT_YCbCr_422_SP;
	    break;
        case V4L2_PIX_FMT_NV24:
            nativeFormat = HAL_PIXEL_FORMAT_YCbCr_444_888;
            break;
        default:
            DEBUG_PRINT(3, "Invalid format %d, Use default format", format);
    }
    return nativeFormat;
}

HinDevImpl::HinDevImpl()
    : mHinDevHandle(-1),
                    mHinNodeInfo(NULL),
                    mSidebandHandle(NULL),
		    mDumpType(0),
                    mDumpFrameCount(30),
		    mFirstRequestCapture(true)
{
    char prop_value[PROPERTY_VALUE_MAX] = {0};
    property_get(DEBUG_LEVEL_PROPNAME, prop_value, "0");
    mDebugLevel = (int)atoi(prop_value);

    property_get(TV_INPUT_SKIP_FRAME, prop_value, "0");
    mSkipFrame = (int)atoi(prop_value);

    property_get(TV_INPUT_DUMP_TYPE, prop_value, "0");
    mDumpType = (int)atoi(prop_value);
    if (mDumpType == 1)
        mDumpFrameCount = 3;

    property_get(TV_INPUT_SHOW_FPS, prop_value, "0");
    mShowFps = (int)atoi(prop_value);

    property_get(TV_INPUT_HAS_ENCODE, prop_value, "0");
    //mHasEncode = (int)atoi(prop_value);
    DEBUG_PRINT(1, "prop value : mDebugLevel=%d, mSkipFrame=%d, mDumpType=%d", mDebugLevel, mSkipFrame, mDumpType);
    mV4l2Event = new V4L2DeviceEvent();
    mSidebandWindow = new RTSidebandWindow();
}

int HinDevImpl::init(int id,int initType) {
    if(get_HdmiIn(true) <= 0 || getNativeWindowFormat(mPixelFormat) == -1){
	DEBUG_PRINT(3, "[%s %d] hdmi isnt in", __FUNCTION__, __LINE__);
        return -1;
    }
    mHinNodeInfo = (struct HinNodeInfo *) calloc (1, sizeof (struct HinNodeInfo));
    if (mHinNodeInfo == NULL)
    {
        DEBUG_PRINT(3, "[%s %d] no memory for mHinNodeInfo", __FUNCTION__, __LINE__);
        close(mHinDevHandle);
        return NO_MEMORY;
    }
    memset(mHinNodeInfo, 0, sizeof(struct HinNodeInfo));
    mHinNodeInfo->currBufferHandleIndex = 0;
    mHinNodeInfo->currBufferHandleFd = 0;

    mFramecount = 0;
    mNotifyQueueCb = NULL;
    mState = STOPED;
    mANativeWindow = NULL;
    mWorkThread = NULL;
    mV4L2DataFormatConvert = false;
    // mPreviewThreadRunning = false;
    // mPreviewBuffThread = NULL;
    mTvInputCB = NULL;
    mOpen = false;

    /**
     *  init RTSidebandWindow
     */
    RTSidebandInfo info;
    memset(&info, 0, sizeof(RTSidebandInfo));
    info.structSize = sizeof(RTSidebandInfo);
    info.top = 0;
    info.left = 0;
    info.width = mFrameWidth;
    info.height = mFrameHeight;
    info.usage = STREAM_BUFFER_GRALLOC_USAGE;
    if (initType == TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE) {
        mFrameType |= TYPF_SIDEBAND_WINDOW;
        mBufferCount = SIDEBAND_WINDOW_BUFF_CNT;
        info.usage |= GRALLOC_USAGE_HW_COMPOSER
            | RK_GRALLOC_USAGE_STRIDE_ALIGN_64;
        mFirstRequestCapture = false;
        mRequestCaptureCount = 1;
    } else {
        mFrameType |= TYPE_STREAM_BUFFER_PRODUCER;
        mBufferCount = APP_PREVIEW_BUFF_CNT;
    }
    info.streamType = mFrameType;
    info.format = mPixelFormat; //0x15

    if(-1 == mSidebandWindow->init(info)) {
        DEBUG_PRINT(3, "mSidebandWindow->init failed !!!");
        return -1;
    }
    return NO_ERROR;
}

int HinDevImpl::findDevice(int id, int& initWidth, int& initHeight,int& initFormat ) {
    ALOGD("%s called", __func__);
    // Find existing /dev/video* devices
    DIR* devdir = opendir(kDevicePath);
    int videofd,ret;
    if(devdir == 0) {
        ALOGE("%s: cannot open %s! Exiting threadloop", __FUNCTION__, kDevicePath);
        return -1;
    }
    struct dirent* de;
    while ((de = readdir(devdir)) != 0) {
        // Find external v4l devices that's existing before we start watching and add them
        if (!strncmp(kPrefix, de->d_name, kPrefixLen)) {
		std::string deviceId(de->d_name + kPrefixLen);
		ALOGD(" v4l device %s found", de->d_name);
		char v4l2DevicePath[kMaxDevicePathLen];
		char v4l2DeviceDriver[16];
		snprintf(v4l2DevicePath, kMaxDevicePathLen,"%s%s", kDevicePath, de->d_name);
		videofd = open(v4l2DevicePath, O_RDWR);
		if (videofd < 0){
			DEBUG_PRINT(3, "[%s %d] mHinDevHandle:%x [%s]", __FUNCTION__, __LINE__, videofd,strerror(errno));
			continue;
		} else {
			DEBUG_PRINT(1, "%s open device %s successful.", __FUNCTION__, v4l2DevicePath);
			struct v4l2_capability cap;
			ret = ioctl(videofd, VIDIOC_QUERYCAP, &cap);
			if (ret < 0) {
				DEBUG_PRINT(3, "VIDIOC_QUERYCAP Failed, error: %s", strerror(errno));
				close(videofd);
				continue;
		}
		snprintf(v4l2DeviceDriver, 16,"%s",cap.driver);
		DEBUG_PRINT(3, "VIDIOC_QUERYCAP driver=%s,%s", cap.driver,v4l2DeviceDriver);
		DEBUG_PRINT(3, "VIDIOC_QUERYCAP card=%s", cap.card);
		DEBUG_PRINT(3, "VIDIOC_QUERYCAP version=%d", cap.version);
		DEBUG_PRINT(3, "VIDIOC_QUERYCAP capabilities=0x%08x,0x%08x", cap.capabilities,V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		DEBUG_PRINT(3, "VIDIOC_QUERYCAP device_caps=0x%08x", cap.device_caps);
		if(!strncmp(kHdmiNodeName, v4l2DeviceDriver, sizeof(kHdmiNodeName)-1)){
			mHinDevHandle =  videofd;
			if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
				ALOGE("V4L2_CAP_VIDEO_CAPTURE is  a video capture device, capabilities: %x\n", cap.capabilities);
					TVHAL_V4L2_BUF_TYPE = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		}else if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
				ALOGE("V4L2_CAP_VIDEO_CAPTURE_MPLANE is  a video capture device, capabilities: %x\n", cap.capabilities);
				TVHAL_V4L2_BUF_TYPE = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			}
			break;
		}else{
			close(videofd);
			DEBUG_PRINT(3, "isnot hdmirx,VIDIOC_QUERYCAP driver=%s", cap.driver);
		}
            }
        }
    }
    closedir(devdir);
    if (mHinDevHandle < 0){
    	DEBUG_PRINT(3, "[%s %d] mHinDevHandle:%x", __FUNCTION__, __LINE__, mHinDevHandle);
    	return -1;
    }
    mV4l2Event->initialize(mHinDevHandle);
    if (get_format(0, initWidth, initHeight, initFormat) == 0)
    {
        DEBUG_PRINT(3, "[%s %d] get_format fail ", __FUNCTION__, __LINE__);
        close(mHinDevHandle);
        return -1;
    }
   // mPixelFormat = DEFAULT_TVHAL_STREAM_FORMAT;
    mFrameWidth = initWidth;
    mFrameHeight = initHeight;
    mBufferSize = mFrameWidth * mFrameHeight * 3/2;

    return 0;
}
int HinDevImpl::makeHwcSidebandHandle() {
    buffer_handle_t buffer = NULL;

    mSidebandWindow->allocateSidebandHandle(&buffer, -1, -1, -1);
    if (!buffer) {
        DEBUG_PRINT(3, "allocate buffer from sideband window failed!");
        return -1;
    }
    mSidebandHandle = buffer;
    return 0;
}

buffer_handle_t HinDevImpl::getSindebandBufferHandle() {
    if (mSidebandHandle == NULL)
        makeHwcSidebandHandle();
    return mSidebandHandle;
}

HinDevImpl::~HinDevImpl()
{
    DEBUG_PRINT(3, "%s %d", __FUNCTION__, __LINE__);
    if (mSidebandWindow) {
        mSidebandWindow->stop();
    }
    if (mV4l2Event)
        mV4l2Event->closeEventThread();
    if (mHinNodeInfo)
        free (mHinNodeInfo);
    if (mHinDevHandle >= 0)
        close(mHinDevHandle);
}

int HinDevImpl::start_device()
{
    if (mFrameType & TYPF_SIDEBAND_WINDOW) {
        //mRequestCaptureCount = 1;
    } else {
        mRequestCaptureCount = 0;
        mFirstRequestCapture = true;
    }
    int ret = -1;

    DEBUG_PRINT(1, "[%s %d] mHinDevHandle:%x", __FUNCTION__, __LINE__, mHinDevHandle);

    ret = ioctl(mHinDevHandle, VIDIOC_QUERYCAP, &mHinNodeInfo->cap);
    if (ret < 0) {
        DEBUG_PRINT(3, "VIDIOC_QUERYCAP Failed, error: %s", strerror(errno));
        return ret;
    }
    DEBUG_PRINT(1, "VIDIOC_QUERYCAP driver=%s", mHinNodeInfo->cap.driver);
    DEBUG_PRINT(1, "VIDIOC_QUERYCAP card=%s", mHinNodeInfo->cap.card);
    DEBUG_PRINT(1, "VIDIOC_QUERYCAP version=%d", mHinNodeInfo->cap.version);
    DEBUG_PRINT(1, "VIDIOC_QUERYCAP capabilities=0x%08x,0x%08x", mHinNodeInfo->cap.capabilities,V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    DEBUG_PRINT(1, "VIDIOC_QUERYCAP device_caps=0x%08x", mHinNodeInfo->cap.device_caps);

    mHinNodeInfo->reqBuf.type = TVHAL_V4L2_BUF_TYPE;
    mHinNodeInfo->reqBuf.memory = TVHAL_V4L2_BUF_MEMORY_TYPE;
    mHinNodeInfo->reqBuf.count = mBufferCount;

    ret = ioctl(mHinDevHandle, VIDIOC_REQBUFS, &mHinNodeInfo->reqBuf);
    if (ret < 0) {
        DEBUG_PRINT(3, "VIDIOC_REQBUFS Failed, error: %s", strerror(errno));
        return ret;
    } else {
        ALOGD("VIDIOC_REQBUFS successful.");
    }

    aquire_buffer();
    for (int i = 0; i < mBufferCount; i++) {
        DEBUG_PRINT(mDebugLevel, "bufferArray index = %d", mHinNodeInfo->bufferArray[i].index);
        DEBUG_PRINT(mDebugLevel, "bufferArray type = %d", mHinNodeInfo->bufferArray[i].type);
        DEBUG_PRINT(mDebugLevel, "bufferArray memory = %d", mHinNodeInfo->bufferArray[i].memory);
        DEBUG_PRINT(mDebugLevel, "bufferArray m.fd = %d", mHinNodeInfo->bufferArray[i].m.planes[0].m.fd);
        DEBUG_PRINT(mDebugLevel, "bufferArray length = %d", mHinNodeInfo->bufferArray[i].length);
        DEBUG_PRINT(mDebugLevel, "buffer length = %d", mSidebandWindow->getBufferLength(mHinNodeInfo->buffer_handle_poll[i]));

 	//mHinNodeInfo->bufferArray[i].flags = V4L2_BUF_FLAG_NO_CACHE_INVALIDATE |
        //                 V4L2_BUF_FLAG_NO_CACHE_CLEAN;
        ret = ioctl(mHinDevHandle, VIDIOC_QBUF, &mHinNodeInfo->bufferArray[i]);
        if (ret < 0) {
            DEBUG_PRINT(3, "VIDIOC_QBUF Failed, error: %s", strerror(errno));
            return -1;
        }
    }
    ALOGD("[%s %d] VIDIOC_QBUF successful", __FUNCTION__, __LINE__);

    v4l2_buf_type bufType;
    bufType = TVHAL_V4L2_BUF_TYPE;
    ret = ioctl(mHinDevHandle, VIDIOC_STREAMON, &bufType);
    if (ret < 0) {
        DEBUG_PRINT(3, "VIDIOC_STREAMON Failed, error: %s", strerror(errno));
        return -1;
    }

    ALOGD("[%s %d] VIDIOC_STREAMON return=:%d", __FUNCTION__, __LINE__, ret);
    return ret;
}

int HinDevImpl::stop_device()
{
    DEBUG_PRINT(3, "%s %d", __FUNCTION__, __LINE__);
    int ret;
    enum v4l2_buf_type bufType = TVHAL_V4L2_BUF_TYPE;

    ret = ioctl (mHinDevHandle, VIDIOC_STREAMOFF, &bufType);
    if (ret < 0) {
        DEBUG_PRINT(3, "StopStreaming: Unable to stop capture: %s", strerror(errno));
    }
    return ret;
}

int HinDevImpl::start()
{
    ALOGD("%s %d", __FUNCTION__, __LINE__);
    int ret;
    if(mOpen == true){
        ALOGI("already open");
        return NO_ERROR;
    }

    ret = start_device();
    if(ret != NO_ERROR) {
        DEBUG_PRINT(3, "Start v4l2 device failed:%d",ret);
        return ret;
    }

    mSidebandWindow->allocateSidebandHandle(&mSignalHandle, -1, -1, HAL_PIXEL_FORMAT_BGR_888);

    ALOGD("Create Work Thread");

    std::string g_stream_dev_name = "/dev/video17";

    //if (g_stream_dev_name.length() > 0 && mHasEncode) {
    /*if (g_stream_dev_name.length() > 0) {
        init_encodeserver(g_stream_dev_name.c_str(), mFrameWidth, mFrameHeight);
    }*/

    mWorkThread = new WorkThread(this);
    mState = START;
    mOpen = true;
    ALOGD("%s %d ret:%d", __FUNCTION__, __LINE__, ret);
    return NO_ERROR;
}


int HinDevImpl::stop()
{
    ALOGD("%s %d", __FUNCTION__, __LINE__);
    int ret;
    mState = STOPED;

    if(gMppEnCodeServer != nullptr) {
        ALOGD("zj add file: %s func %s line %d \n",__FILE__,__FUNCTION__,__LINE__);
        gMppEnCodeServer->stop();
    }
    if(mWorkThread != NULL){
        mWorkThread->requestExit();
        mWorkThread.clear();
        mWorkThread = NULL;
    }

    if (mFrameType & TYPF_SIDEBAND_WINDOW) {
        mSidebandWindow->clearVopArea();
    }
    enum v4l2_buf_type bufType = TVHAL_V4L2_BUF_TYPE;
    ret = ioctl (mHinDevHandle, VIDIOC_STREAMOFF, &bufType);
    if (ret < 0) {
        DEBUG_PRINT(3, "StopStreaming: Unable to stop capture: %s", strerror(errno));
    } else {
        DEBUG_PRINT(3, "StopStreaming: successful.");
    }

    // cancel request buff
    v4l2_requestbuffers req_buffers{};
    req_buffers.type = TVHAL_V4L2_BUF_TYPE;
    req_buffers.memory = TVHAL_V4L2_BUF_MEMORY_TYPE;
    req_buffers.count = 0;
    ret = ioctl(mHinDevHandle, VIDIOC_REQBUFS, &req_buffers);
    if (ret < 0) {
        ALOGE("%s: cancel REQBUFS failed: %s", __FUNCTION__, strerror(errno));
    } else {
        ALOGE("%s: cancel REQBUFS successful.", __FUNCTION__);
    }

    if (mSidebandWindow) {
        mSidebandWindow->stop();
    }
    release_buffer();
    mDumpFrameCount = 3;

    mOpen = false;
    mFrameType = 0;

    if (mHinNodeInfo)
        free(mHinNodeInfo);

    if (mV4l2Event)
        mV4l2Event->closePipe();

    if (mHinDevHandle >= 0)
        close(mHinDevHandle);

    mFirstRequestCapture = true;
    mRequestCaptureCount = 0;

    deinit_encodeserver();

    DEBUG_PRINT(3, "============================= %s end ================================", __FUNCTION__);
    return ret;
}

int HinDevImpl::set_preview_callback(NotifyQueueDataCallback callback)
{
    if (!callback) {
        DEBUG_PRINT(3, "NULL state callback pointer");
        return BAD_VALUE;
    }
    mNotifyQueueCb = callback;
    return NO_ERROR;
}

int HinDevImpl::set_data_callback(V4L2EventCallBack callback)
{
    ALOGD("%s %d", __FUNCTION__, __LINE__);
    if (callback == NULL){
        DEBUG_PRINT(3, "NULL data callback pointer");
        return BAD_VALUE;
    }
    mV4l2Event->RegisterEventvCallBack(callback);
    return NO_ERROR;
}

int HinDevImpl::get_format(int fd, int &hdmi_in_width, int &hdmi_in_height,int& initFormat)
{
    std::vector<int> formatList;
    struct v4l2_fmtdesc fmtdesc;
    fmtdesc.index = 0;
    fmtdesc.type = TVHAL_V4L2_BUF_TYPE;

    while (ioctl(mHinDevHandle, VIDIOC_ENUM_FMT, &fmtdesc) != -1)
    {
        formatList.push_back( fmtdesc.pixelformat);
        DEBUG_PRINT(3, "   V4L2 driver: idx=%d, \t desc:%s,format:0x%x", fmtdesc.index + 1, fmtdesc.description, fmtdesc.pixelformat);
        fmtdesc.index++;
    }
    v4l2_format format;
    format.type = TVHAL_V4L2_BUF_TYPE;
    vector<int>::iterator it;
    for(it = formatList.begin();it != formatList.end();it++){
    	format.fmt.pix.pixelformat = (int)*it;
    	if (ioctl(mHinDevHandle, VIDIOC_TRY_FMT, &format) != -1)
    	{
    		DEBUG_PRINT(3, "V4L2 driver try: width:%d,height:%d,format:0x%x", format.fmt.pix.width, format.fmt.pix.height,format.fmt.pix.pixelformat);
    		hdmi_in_width =  format.fmt.pix.width;
    		hdmi_in_height = format.fmt.pix.height;
    		mPixelFormat = format.fmt.pix.pixelformat;
    		initFormat = getNativeWindowFormat(format.fmt.pix.pixelformat);//V4L2_PIX_FMT_BGR24;
    		break;
    	}
    }
    int err = ioctl(mHinDevHandle, VIDIOC_G_FMT, &format);
    if (err < 0)
    {
        DEBUG_PRINT(3, "[%s %d] failed, VIDIOC_G_FMT %d, %s", __FUNCTION__, __LINE__, err, strerror(err));
    }
    else
    {
        DEBUG_PRINT(3, "after %s get from v4l2 format.type = %d ", __FUNCTION__, format.type);
        DEBUG_PRINT(3, "after %s get from v4l2 format.fmt.pix.width =%d", __FUNCTION__, format.fmt.pix.width);
        DEBUG_PRINT(3, "after %s get from v4l2 format.fmt.pix.height =%d", __FUNCTION__, format.fmt.pix.height);
        DEBUG_PRINT(3, "after %s get from v4l2 format.fmt.pix.pixelformat =%d", __FUNCTION__, format.fmt.pix.pixelformat);
    }

    err = ioctl(mHinDevHandle, RK_HDMIRX_CMD_GET_FPS, &mFrameFps);
    if (err < 0) {
        DEBUG_PRINT(3, "[%s %d] failed, RK_HDMIRX_CMD_GET_FPS %d, %s", __FUNCTION__, __LINE__, err, strerror(err));
        mFrameFps = 60;
    } else {
        DEBUG_PRINT(3, "[%s %d] RK_HDMIRX_CMD_GET_FPS %d", __FUNCTION__, __LINE__, mFrameFps);
    }

    if(hdmi_in_width == 0 || hdmi_in_height == 0) return 0;
    return -1;
}
int HinDevImpl::get_HdmiIn(bool enforce){
    if(enforce && mIsHdmiIn) return mIsHdmiIn;
    struct v4l2_control control;
    memset(&control, 0, sizeof(struct v4l2_control));
    control.id = V4L2_CID_DV_RX_POWER_PRESENT;
    int err = ioctl(mHinDevHandle, VIDIOC_G_CTRL, &control);
    if (err < 0) {
        ALOGE("Set POWER_PRESENT failed ,%d(%s)", errno, strerror(errno));
        return UNKNOWN_ERROR;
    }
    mIsHdmiIn = control.value;
    //enum v4l2_buf_type bufType = TVHAL_V4L2_BUF_TYPE;

    if(mIsHdmiIn && mState == START){
       /*err = ioctl(mHinDevHandle, VIDIOC_STREAMON, &bufType);
       if (err < 0) {
          DEBUG_PRINT(3, "VIDIOC_STREAMON Failed, error: %s", strerror(errno));
       }
       for (int i = 0; i < mBufferCount; i++) {
          err = ioctl(mHinDevHandle, VIDIOC_QBUF, &mHinNodeInfo->bufferArray[i]);
          if (err < 0) {
            DEBUG_PRINT(3, "VIDIOC_QBUF Failed, error: %s", strerror(errno));
          }
       }
       ALOGD("[%s %d] VIDIOC_STREAMON return=:%d", __FUNCTION__, __LINE__, err);*/
       //mState = START;
    }else{
       /*err = ioctl (mHinDevHandle, VIDIOC_STREAMOFF, &bufType);
       if (err < 0) {
          DEBUG_PRINT(3, "StopStreaming: Unable to stop capture: %s", strerror(errno));
       }*/
       mState = STOPED;
    }
    DEBUG_PRINT(3, "getHdmiIn : %d.", mIsHdmiIn);
    return mIsHdmiIn;
}
int HinDevImpl::set_mode(int displayMode)
{
    DEBUG_PRINT(3, "run into set_mode,displaymode = %d\n", displayMode);
    mHinNodeInfo->displaymode = displayMode;
    m_displaymode = displayMode;
    return 0;
}

int HinDevImpl::set_format(int width, int height, int color_format)
{
    ALOGD("[%s %d] width=%d, height=%d, color_format=%d, mPixelFormat=%d", __FUNCTION__, __LINE__, width, height, color_format, mPixelFormat);
    Mutex::Autolock autoLock(mLock);
    if (mOpen == true)
        return NO_ERROR;
    int ret;

    mFrameWidth = width;
    mFrameHeight = height;
    //mPixelFormat = color_format;
    mHinNodeInfo->width = width;
    mHinNodeInfo->height = height;
    mHinNodeInfo->formatIn = mPixelFormat;
    mHinNodeInfo->format.type = TVHAL_V4L2_BUF_TYPE;
    mHinNodeInfo->format.fmt.pix.width = width;
    mHinNodeInfo->format.fmt.pix.height = height;
    mHinNodeInfo->format.fmt.pix.pixelformat = mPixelFormat;

    ret = ioctl(mHinDevHandle, VIDIOC_S_FMT, &mHinNodeInfo->format);
    if (ret < 0) {
        DEBUG_PRINT(3, "[%s %d] failed, set VIDIOC_S_FMT %d, %s", __FUNCTION__, __LINE__, ret, strerror(ret));
        return ret;
    } else {
        ALOGD("%s VIDIOC_S_FMT success. ", __FUNCTION__);
    }
    int format = getNativeWindowFormat(mPixelFormat);
    mSidebandWindow->setBufferGeometry(mFrameWidth, mFrameHeight, format);
    return ret;
}

int HinDevImpl::set_rotation(int degree)
{
    ALOGD("[%s %d]", __FUNCTION__, __LINE__);
    int ret = 0;
    struct v4l2_control ctl;
    if(mHinDevHandle<0)
        return -1;
    if((degree!=0)&&(degree!=90)&&(degree!=180)&&(degree!=270)){
        DEBUG_PRINT(3, "Set rotate value invalid: %d.", degree);
        return -1;
    }

    memset( &ctl, 0, sizeof(ctl));
    ctl.value=degree;
    ctl.id = V4L2_ROTATE_ID;
    ret = ioctl(mHinDevHandle, VIDIOC_S_CTRL, &ctl);

    if(ret<0){
        DEBUG_PRINT(3, "Set rotate value fail: %s. ret=%d", strerror(errno),ret);
    }
    return ret ;
}

int HinDevImpl::set_crop(int x, int y, int width, int height)
{
    ALOGD("[%s %d] crop [%d - %d -%d - %d]", __FUNCTION__, __LINE__, x, y, width, height);
    mSidebandWindow->setCrop(x, y, width, height);
    return NO_ERROR;
}

int HinDevImpl::set_frame_rate(int frameRate)
{
    ALOGD("[%s %d]", __FUNCTION__, __LINE__);
    int ret = 0;

    if(mHinDevHandle<0)
        return -1;

    struct v4l2_streamparm sparm;
    memset(&sparm, 0, sizeof( sparm ));
    sparm.type = TVHAL_V4L2_BUF_TYPE;//stream_flag;
    sparm.parm.output.timeperframe.denominator = frameRate;
    sparm.parm.output.timeperframe.numerator = 1;

    ret = ioctl(mHinDevHandle, VIDIOC_S_PARM, &sparm);
    if(ret < 0){
        DEBUG_PRINT(3, "Set frame rate fail: %s. ret=%d", strerror(errno),ret);
    }
    return ret ;
}

int HinDevImpl::get_hin_crop(int *x, int *y, int *width, int *height)
{
    ALOGD("[%s %d]", __FUNCTION__, __LINE__);
    int ret = 0;

    struct v4l2_crop crop;
    memset(&crop, 0, sizeof(struct v4l2_crop));
    crop.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    ret = ioctl(mHinDevHandle, VIDIOC_S_CROP, &crop);
    if (ret) {
        DEBUG_PRINT(3, "get amlvideo2 crop  fail: %s. ret=%d", strerror(errno),ret);
    }
    *x = crop.c.left;
    *y = crop.c.top;
    *width = crop.c.width;
    *height = crop.c.height;
     return ret ;
}

int HinDevImpl::set_hin_crop(int x, int y, int width, int height)
{
    ALOGD("[%s %d]", __FUNCTION__, __LINE__);
    int ret = 0;

    struct v4l2_crop crop;
    memset(&crop, 0, sizeof(struct v4l2_crop));

    crop.type = TVHAL_V4L2_BUF_TYPE;
    crop.c.left = x;
    crop.c.top = y;
    crop.c.width = width;
    crop.c.height = height;
    ret = ioctl(mHinDevHandle, VIDIOC_S_CROP, &crop);
    if (ret) {
        DEBUG_PRINT(3, "Set amlvideo2 crop  fail: %s. ret=%d", strerror(errno),ret);
    }

    return ret ;
}

int HinDevImpl::get_current_sourcesize(int& width,  int& height,int& pixelformat)
{
    int ret = NO_ERROR;
    v4l2_format format;
    memset(&format, 0,sizeof(struct v4l2_format));

    format.type = TVHAL_V4L2_BUF_TYPE;
    ret = ioctl(mHinDevHandle, VIDIOC_G_FMT, &format);
    if (ret < 0) {
        DEBUG_PRINT(3, "Open: VIDIOC_G_FMT Failed: %s", strerror(errno));
        return ret;
    }
    width = format.fmt.pix.width;
    height = format.fmt.pix.height;
    pixelformat = getNativeWindowFormat(format.fmt.pix.pixelformat);

    mFrameWidth = width;
    mFrameHeight = height;
    mBufferSize = mFrameWidth * mFrameHeight * 3/2;
    mPixelFormat = format.fmt.pix.pixelformat;
    ALOGD("VIDIOC_G_FMT, w * h: %5d x %5d, fomat 0x%x", width,  height,pixelformat);
    /*if(mIsHdmiIn){
       enum v4l2_buf_type bufType = TVHAL_V4L2_BUF_TYPE;
       ret = ioctl(mHinDevHandle, VIDIOC_STREAMON, &bufType);
    ALOGD("[%s %d] VIDIOC_STREAMON return=:%d", __FUNCTION__, __LINE__, ret);
       if (ret < 0) {
          DEBUG_PRINT(3, "VIDIOC_STREAMON Failed, error: %s", strerror(errno));
       }
       mState = START;
    }*/
    mState = START;
    return ret;
}

int HinDevImpl::set_screen_mode(int mode)
{
    int ret = NO_ERROR;
    ret = ioctl(mHinDevHandle, VIDIOC_S_OUTPUT, &mode);
    if (ret < 0) {
        DEBUG_PRINT(3, "VIDIOC_S_OUTPUT Failed: %s", strerror(errno));
        return ret;
    }
    return ret;
}

int HinDevImpl::aquire_buffer()
{
    int ret = UNKNOWN_ERROR;
    DEBUG_PRINT(mDebugLevel, "%s %d", __FUNCTION__, __LINE__);
    for (int i = 0; i < mBufferCount; i++) {
        memset(&mHinNodeInfo->planes[i], 0, sizeof(struct v4l2_plane));
        memset(&mHinNodeInfo->bufferArray[i], 0, sizeof(struct v4l2_buffer));

        mHinNodeInfo->bufferArray[i].index = i;
        mHinNodeInfo->bufferArray[i].type = TVHAL_V4L2_BUF_TYPE;
        mHinNodeInfo->bufferArray[i].memory = TVHAL_V4L2_BUF_MEMORY_TYPE;
        if (mHinNodeInfo->cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
            mHinNodeInfo->bufferArray[i].m.planes = &mHinNodeInfo->planes[i];
            mHinNodeInfo->bufferArray[i].length = PLANES_NUM;
        }

        ret = ioctl(mHinDevHandle, VIDIOC_QUERYBUF, &mHinNodeInfo->bufferArray[i]);
        if (ret < 0) {
            DEBUG_PRINT(3, "VIDIOC_QUERYBUF Failed, error: %s", strerror(errno));
            return ret;
        }


       if (mFrameType & TYPF_SIDEBAND_WINDOW) {
            ret = mSidebandWindow->allocateBuffer(&mHinNodeInfo->buffer_handle_poll[i]);
            if (ret != 0) {
                DEBUG_PRINT(3, "mSidebandWindow->allocateBuffer failed !!!");
                return ret;
            }
        } else {
            mHinNodeInfo->buffer_handle_poll[i] = mPreviewRawHandle[i].outHandle;
        }

	 if (mHinNodeInfo->cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
            for (int j=0; j<PLANES_NUM; j++) {
                //mHinNodeInfo->bufferArray[i].m.planes[j].m.fd = mSidebandWindow->getBufferHandleFd(mHinNodeInfo->buffer_handle_poll[i]);
		if (mFrameType & TYPF_SIDEBAND_WINDOW) {
                    mHinNodeInfo->bufferArray[i].m.planes[j].m.fd = mSidebandWindow->getBufferHandleFd(mHinNodeInfo->buffer_handle_poll[i]);
                } else {
                    mHinNodeInfo->bufferArray[i].m.planes[j].m.fd = mPreviewRawHandle[i].bufferFd;
                }
                mHinNodeInfo->bufferArray[i].m.planes[j].length = 0;
            }
        }
    }
    ALOGD("[%s %d] VIDIOC_QUERYBUF successful", __FUNCTION__, __LINE__);
    return -1;
}

int HinDevImpl::release_buffer()
{
    DEBUG_PRINT(mDebugLevel, "%s %d", __FUNCTION__, __LINE__);
    if (mSidebandHandle) {
        mSidebandWindow->freeBuffer(&mSidebandHandle, 0);
        mSidebandHandle = NULL;
    }

    if (mSignalHandle) {
        mSidebandWindow->freeBuffer(&mSignalHandle, 0);
        mSignalHandle = NULL;
    }

    if (!mRecordHandle.empty()){
        for (int i=0; i<mRecordHandle.size(); i++) {
            mSidebandWindow->freeBuffer(&mRecordHandle[i].outHandle, 1);
            mRecordHandle[i].outHandle = NULL;
        }
        mRecordHandle.clear();
    }

    if (mFrameType & TYPE_STREAM_BUFFER_PRODUCER) {
        if (!mPreviewRawHandle.empty()) {
            for (int i=0; i<mPreviewRawHandle.size(); i++) {
                mSidebandWindow->freeBuffer(&mPreviewRawHandle[i].outHandle, 1);
                mPreviewRawHandle[i].outHandle = NULL;
                mHinNodeInfo->buffer_handle_poll[i] = NULL;
            }
            mPreviewRawHandle.clear();
        }
    } else {
        for (int i=0; i<mBufferCount; i++) {
            if (mSidebandWindow) {
                mSidebandWindow->freeBuffer(&mHinNodeInfo->buffer_handle_poll[i], 0);
            }
            mHinNodeInfo->buffer_handle_poll[i] = NULL;
        }
    }
    return 0;
}

int HinDevImpl::set_preview_info(int top, int left, int width, int height) {
    mPreviewRawHandle.resize(APP_PREVIEW_BUFF_CNT);
    return 0;
}

int HinDevImpl::set_preview_buffer(buffer_handle_t rawHandle, uint64_t bufferId) {
    ALOGD("%s called, rawHandle=%p bufferId=%" PRIu64, __func__, rawHandle, bufferId);
    int buffHandleFd = mSidebandWindow->importHidlHandleBufferLocked(rawHandle);
    ALOGD("%s buffHandleFd=%d, after import rawHandle=%p", __FUNCTION__, buffHandleFd, rawHandle);
    mPreviewRawHandle[mPreviewBuffIndex].bufferFd = buffHandleFd;
    mPreviewRawHandle[mPreviewBuffIndex].bufferId = bufferId;
    mPreviewRawHandle[mPreviewBuffIndex].outHandle = rawHandle;
    mPreviewRawHandle[mPreviewBuffIndex].isRendering = false;
    mPreviewRawHandle[mPreviewBuffIndex].isFilled = false;
    mPreviewBuffIndex++;
    if(mPreviewBuffIndex == APP_PREVIEW_BUFF_CNT) mPreviewBuffIndex = 0;
    return 0;
}


int HinDevImpl::request_capture(buffer_handle_t rawHandle, uint64_t bufferId) {
    //int ret;
    //int bufferIndex = -1;
    //ALOGD("rawHandle = %p,bufferId=%lld,%lld" PRIu64, rawHandle,(long long)bufferId,(long long)mPreviewRawHandle[0].bufferId);
    int previewBufferIndex = -1;
    for (int i=0; i<mPreviewRawHandle.size(); i++) {
        if (mPreviewRawHandle[i].bufferId == bufferId) {
            previewBufferIndex = i;
            break;
        }
    }

    int bufferIndex = -1;
    int requestFd = -1;
    for (int i=0; i<mBufferCount; i++) {
        int fd = mHinNodeInfo->bufferArray[i].m.planes[0].m.fd;
        if (fd == mPreviewRawHandle[previewBufferIndex].bufferFd) {
            bufferIndex = i;
            requestFd = fd;
            break;
        }
    }
    DEBUG_PRINT(mDebugLevel, "request_capture previewBufferIndex=%d, bufferIndex=%d, requestFd=%d, bufferId %" PRIu64,
        previewBufferIndex, bufferIndex, requestFd, bufferId);
    if ( mFirstRequestCapture/* && mPreviewRawHandle[0].bufferId == bufferId*/) {
        ALOGW("first request_capture, deque first two buffer for skip");
        mFirstRequestCapture = false;
        mHinNodeInfo->currBufferHandleIndex = 0;
        mRequestCaptureCount = 2;
        // mPreviewBuffThread = new PreviewBuffThread(this);
        // mPreviewThreadRunning = true;
        //mPreviewBuffIndex = 0;
        return 0;
    }
    if (mState != START) {
        return 0;
    }

    mRequestCaptureCount++;

    //ALOGD("rawHandle = %p, bufferId=%" PRIu64, rawHandle, bufferId);
    for (int i=0; i<mPreviewRawHandle.size(); i++) {
        if (mPreviewRawHandle[i].bufferId == bufferId) {
            if (mPreviewRawHandle[i].isFilled) {
                mPreviewRawHandle[i].isRendering = false;
                mPreviewRawHandle[i].isFilled = false;
                break;
            }
        }
    }
    int ret = ioctl(mHinDevHandle, VIDIOC_QBUF, &mHinNodeInfo->bufferArray[bufferIndex]);
    if (ret != 0) {
        ALOGE("VIDIOC_QBUF Buffer failed err=%s bufferIndex %d requestFd=%d %" PRIu64,
            strerror(errno), bufferIndex, requestFd, bufferId);
    }

    ALOGV("%s end.", __FUNCTION__);
    return mHinNodeInfo->currBufferHandleIndex;
}

void HinDevImpl::wrapCaptureResultAndNotify(uint64_t buffId,buffer_handle_t handle) {
    if (mState == STOPED) {
        return;
    };
    /*if (mFirstRequestCapture && mPreviewRawHandle[0].bufferId == buffId) {
        ALOGD("first wrapCaptureResultAndNotify, ignore it.");
        mFirstRequestCapture = false;
        return;
    }*/
    tv_input_capture_result_t result;
    result.buff_id = buffId;
    //ALOGD("%s %lld,end.", __FUNCTION__,(long long)buffId);
    // result.buffer = handle;  //if need
    if(mNotifyQueueCb != NULL)
    	mNotifyQueueCb(result);
}

void OnInputAvailableCB(int32_t index){
    //ALOGD("InputAvailable index = %d",index);
    if (!mRecordHandle.empty()){
        if (!mRecordHandle[index].isCoding) {
            DEBUG_PRINT(3, "%d not send to coding but return it???", index);
        }
        mRecordHandle[index].isCoding = false;
    }
}

int HinDevImpl::init_encodeserver(MppEncodeServer::MetaInfo* info) {
    if (gMppEnCodeServer == nullptr) {
        gMppEnCodeServer = new MppEncodeServer();
    }

    if (!gMppEnCodeServer->init(info)) {
        ALOGE("Failed to init gMppEnCodeServer");
        return -1;
    }
    NotifyCallback cB = {OnInputAvailableCB};
    gMppEnCodeServer->setNotifyCallback(cB,this);
    // gMppEnCodeServer->start();

    return 0;
}

void HinDevImpl::deinit_encodeserver() {
    ALOGD("deinit_encodeserver enter");
    if(gMppEnCodeServer!=nullptr){
        delete gMppEnCodeServer;
        gMppEnCodeServer = nullptr;
    }
}

void HinDevImpl::stopRecord() {
    if (gMppEnCodeServer != nullptr) {
        gMppEnCodeServer->stop();
    }
    deinit_encodeserver();
    if (!mRecordHandle.empty()){
        for (int i=0; i<mRecordHandle.size(); i++) {
            mSidebandWindow->freeBuffer(&mRecordHandle[i].outHandle, 1);
            mRecordHandle[i].outHandle = NULL;
        }
        mRecordHandle.clear();
    }
}

void HinDevImpl::doRecordCmd(const map<string, string> data) {
    int width = mFrameWidth;
    int height = mFrameHeight;
    if (mFrameFps < 1) {
        ioctl(mHinDevHandle, RK_HDMIRX_CMD_GET_FPS, &mFrameFps);
        ALOGD("%s RK_HDMIRX_CMD_GET_FPS %d", __FUNCTION__, mFrameFps);
    }
    int fps = mFrameFps;
    bool allowRecord = false;
    ALOGD("%s %d %d", __FUNCTION__, fps, mFrameFps);
    string storePath = "";
    for (auto it : data) {
        ALOGD("%s %s %s", __FUNCTION__, it.first.c_str(), it.second.c_str());
        if (it.first.compare("status") == 0) {
            if (it.second.compare("0") == 0) {
                allowRecord = false;
            } else if (it.second.compare("1") == 0) {
                if (!mRecordHandle.empty()){
                    for (int i=0; i<mRecordHandle.size(); i++) {
                        mSidebandWindow->freeBuffer(&mRecordHandle[i].outHandle, 1);
                        mRecordHandle[i].outHandle = NULL;
                    }
                    mRecordHandle.clear();
                }
                mRecordHandle.resize(SIDEBAND_RECORD_BUFF_CNT);
                mRecordCodingBuffIndex = 0;
                if (height == 1080) {
                    height = 1088;
                }
                for (int i=0; i<mRecordHandle.size(); i++) {
                    mSidebandWindow->allocateSidebandHandle(&mRecordHandle[i].outHandle,
                        width, height, HAL_PIXEL_FORMAT_YCrCb_NV12);
                    mRecordHandle[i].isCoding = false;
                    mRecordHandle[i].width = width;
                    mRecordHandle[i].height = height;
                }

                allowRecord = true;
            } else {
                return;
            }
        } else if (it.first.compare("storePath") == 0) {
            storePath = it.second;
        } else if (it.first.compare("width")) {
            width = stoi(it.second);
        } else if (it.first.compare("height")) {
            height = stoi(it.second);
        } else if (it.first.compare("fps")) {
            fps = stoi(it.second);
        }
    }

    if (fps < 1) {
        fps = 60;
        ALOGD("fps == 0");
    }
    if (V4L2_PIX_FMT_NV24 == mPixelFormat && fps > 50) {
        fps = 30;
    }

    MppEncodeServer::MetaInfo info;
    info.width = width;
    info.height = height;
    info.fps = fps;
    info.port_num = 1234;
    strcat(info.dev_name, "v");
    strcat(info.stream_name, "v");
    ALOGD("%s %dx%d fps=%d %s", __FUNCTION__, width, height, fps, storePath.c_str());

    if (allowRecord && init_encodeserver(&info) != -1) {
        if (storePath.compare("") != 0) {
            gMppEnCodeServer->mOutputFile = fopen(storePath.c_str(), "w+b");
        }
        if (gMppEnCodeServer->mOutputFile == nullptr) {
            ALOGD("%s mOutputFile is null %s " , __FUNCTION__, strerror(errno));
        }
        gMppEnCodeServer->start();
    } else {
        stopRecord();
    }
}

int HinDevImpl::deal_priv_message(const std::string action, const std::map<std::string, std::string> data) {
    ALOGD("%s %s ", __FUNCTION__, action.c_str());
    if (action.compare("record") == 0) {
        doRecordCmd(data);
        return 1;
    } else if (action.compare("hdmiinout") == 0) {
        if (mFrameType & TYPF_SIDEBAND_WINDOW && NULL != mSidebandHandle) {
            //mSidebandWindow->clearVopArea();
            stopRecord();
            if (mSignalHandle != NULL) {
                mSidebandWindow->show(mSignalHandle);
            }
        }
        return 1;
    }
    return 0;
}

int HinDevImpl::getRecordBufferFd(int previewHandlerIndex) {
    if(mRecordHandle.empty()) {
        return -1;
    } else {
        if (mRecordHandle[mRecordCodingBuffIndex].isCoding) {
            return -1;
        }
    }

    int recordFd = -1;
    tv_record_buffer_info_t recordBuffer = mRecordHandle[mRecordCodingBuffIndex];

    if (V4L2_PIX_FMT_BGR24 == mPixelFormat
            || V4L2_PIX_FMT_NV12 == mPixelFormat
            || V4L2_PIX_FMT_NV16 == mPixelFormat) {
        RgaCropScale::Params original, out;
        original.fd = mHinNodeInfo->bufferArray[previewHandlerIndex].m.planes[0].m.fd;
        original.offset_x = 0;
        original.offset_y = 0;
        original.width_stride = mFrameWidth;
        original.height_stride = mFrameHeight;
        original.width = mFrameWidth;
        original.height = mFrameHeight;
        int rgaFormat = mPixelFormat;
        if (V4L2_PIX_FMT_BGR24 == mPixelFormat) {
            rgaFormat = RK_FORMAT_BGR_888;
        } else if (V4L2_PIX_FMT_NV16 == mPixelFormat) {
            rgaFormat = RK_FORMAT_YCbCr_422_SP;
        } else if (V4L2_PIX_FMT_NV12 == mPixelFormat) {
            rgaFormat = RK_FORMAT_YCbCr_420_SP;
        }
        original.fmt = rgaFormat;
        original.mirror = false;

        out.fd = recordBuffer.outHandle->data[0];
        out.offset_x = 0;
        out.offset_y = 0;
        out.width_stride = recordBuffer.width;
        out.height_stride = recordBuffer.height;
        out.width = recordBuffer.width;
        out.height = recordBuffer.height;
        out.fmt = RK_FORMAT_YCbCr_420_SP;
        out.mirror = false;
        RgaCropScale::CropScaleNV12Or21(&original, &out);
    } else if (V4L2_PIX_FMT_NV24 == mPixelFormat) {
        mSidebandWindow->NV24ToNV12(
            mHinNodeInfo->buffer_handle_poll[previewHandlerIndex],
            recordBuffer.outHandle,
            mFrameWidth, mFrameHeight);
    } else {
        return -1;
    }
    recordFd = recordBuffer.outHandle->data[0];
    recordBuffer.isCoding = true;
    //DEBUG_PRINT(3, "record out.fd %d", recordFd);
    return recordFd;
}

int HinDevImpl::workThread()
{
    int ret;
    if (mState == START /*&& !mFirstRequestCapture*/ && mRequestCaptureCount > 0) {
        //DEBUG_PRINT(3, "%s %d currBufferHandleIndex = %d", __FUNCTION__, __LINE__, mHinNodeInfo->currBufferHandleIndex);
 	//mHinNodeInfo->bufferArray[mHinNodeInfo->currBufferHandleIndex].flags = V4L2_BUF_FLAG_NO_CACHE_INVALIDATE |
        //                 V4L2_BUF_FLAG_NO_CACHE_CLEAN;
        if (mFrameType & TYPF_SIDEBAND_WINDOW) {
            if (mHinNodeInfo->currBufferHandleIndex == SIDEBAND_WINDOW_BUFF_CNT)
                mHinNodeInfo->currBufferHandleIndex = mHinNodeInfo->currBufferHandleIndex % SIDEBAND_WINDOW_BUFF_CNT;
        } else {
            if (mHinNodeInfo->currBufferHandleIndex == APP_PREVIEW_BUFF_CNT)
                mHinNodeInfo->currBufferHandleIndex = mHinNodeInfo->currBufferHandleIndex % APP_PREVIEW_BUFF_CNT;
            mRequestCaptureCount--;
        }
        ret = ioctl(mHinDevHandle, VIDIOC_DQBUF, &mHinNodeInfo->bufferArray[mHinNodeInfo->currBufferHandleIndex]);
        if (ret < 0) {
            DEBUG_PRINT(3, "VIDIOC_DQBUF Failed, error: %s", strerror(errno));
            return 0;
        } else {
            DEBUG_PRINT(mDebugLevel, "VIDIOC_DQBUF successful.mDumpType=%d,mDumpFrameCount=%d",mDumpType,mDumpFrameCount);
        }
#ifdef DUMP_YUV_IMG
            if (mDumpType == 0 && mDumpFrameCount > 0) {
                char fileName[128] = {0};
                sprintf(fileName, "/data/system/dumpimage/tv_input_dump_%dx%d_%d.yuv", mFrameWidth, mFrameHeight, mDumpFrameCount);
                mSidebandWindow->dumpImage(mHinNodeInfo->buffer_handle_poll[mHinNodeInfo->currBufferHandleIndex], fileName, 0);
                mDumpFrameCount--;
            } else if (mDumpType == 1 && mDumpFrameCount > 0) {
                char fileName[128] = {0};
                sprintf(fileName, "/data/system/dumpimage/tv_input_dump_%dx%d.h264", mFrameWidth, mFrameHeight);
                mSidebandWindow->dumpImage(mHinNodeInfo->buffer_handle_poll[mHinNodeInfo->currBufferHandleIndex], fileName, 0);
                mDumpFrameCount--;
            }
#endif

        if (mFrameType & TYPF_SIDEBAND_WINDOW) {
            // add flushCache to prevent image tearing and ghosting caused by
            // cache consistency issues
            int currPreviewHandlerIndex = mHinNodeInfo->currBufferHandleIndex;
            ret = mSidebandWindow->flushCache(
                mHinNodeInfo->buffer_handle_poll[currPreviewHandlerIndex]);
            if (ret != 0) {
                DEBUG_PRINT(3, "mSidebandWindow->flushCache failed !!!");
                return ret;
            }

            mSidebandWindow->show(
                mHinNodeInfo->buffer_handle_poll[currPreviewHandlerIndex]);

//encode:sendFrame
            if (gMppEnCodeServer != nullptr && gMppEnCodeServer->mThreadEnabled.load()) {
                RKMppEncApi::MyDmaBuffer_t inDmaBuf;
                memset(&inDmaBuf, 0, sizeof(RKMppEncApi::MyDmaBuffer_t));
                inDmaBuf.fd = getRecordBufferFd(currPreviewHandlerIndex);
                if (inDmaBuf.fd == -1) {
                    DEBUG_PRINT(3, "skip record");
                } else {
                inDmaBuf.size = gMppEnCodeServer->mEncoder->mHorStride *
                                gMppEnCodeServer->mEncoder->mVerStride * 3 / 2;
                inDmaBuf.handler =
                    (void *)mHinNodeInfo
                    ->buffer_handle_poll[currPreviewHandlerIndex];
                inDmaBuf.index = mRecordCodingBuffIndex;
                mRecordHandle[mRecordCodingBuffIndex].isCoding = true;
                mRecordCodingBuffIndex++;
                if (mRecordCodingBuffIndex == SIDEBAND_RECORD_BUFF_CNT) {
                    mRecordCodingBuffIndex = 0;
                }
                mLastTime = systemTime();
                bool enc_ret = gMppEnCodeServer->mEncoder->sendFrame(
                                   (RKMppEncApi::MyDmaBuffer_t)inDmaBuf,
                                   getBufSize(V4L2_PIX_FMT_NV12, mFrameWidth, mFrameHeight),
                                   systemTime(), 0);

                now = systemTime();
                diff = now - mLastTime;

                if (!enc_ret) {
                    DEBUG_PRINT(3, "sendFrame failed");
                }
                }
            }
//start encode threads
             if (gMppEnCodeServer != nullptr && !mEncodeThreadRunning) {
                gMppEnCodeServer->start();
                mEncodeThreadRunning = true;
             }
            ret = ioctl(mHinDevHandle, VIDIOC_QBUF, &mHinNodeInfo->bufferArray[mHinNodeInfo->currBufferHandleIndex]);
            if (ret != 0) {
                DEBUG_PRINT(3, "VIDIOC_QBUF Buffer failed %s", strerror(errno));
            } else {
                DEBUG_PRINT(mDebugLevel, "VIDIOC_QBUF %d successful.", mHinNodeInfo->currBufferHandleIndex);
            }
        } else {
            if (mV4L2DataFormatConvert) {
                mSidebandWindow->buffDataTransfer(mHinNodeInfo->buffer_handle_poll[mHinNodeInfo->currBufferHandleIndex], mPreviewRawHandle[mPreviewBuffIndex].outHandle);
            }
            for (int i=0; i<mPreviewRawHandle.size(); i++) {
                if (mPreviewRawHandle[i].bufferFd == mHinNodeInfo->bufferArray[mHinNodeInfo->currBufferHandleIndex].m.planes[0].m.fd) {
                    wrapCaptureResultAndNotify(mPreviewRawHandle[i].bufferId,mPreviewRawHandle[i].outHandle);
                    break;
                }
            }
        }
        debugShowFPS();
        mHinNodeInfo->currBufferHandleIndex++;
    }
    return NO_ERROR;
}

void HinDevImpl::debugShowFPS() {
    if (mShowFps == 0)
        return;
    static int mFrameCount = 0;
    static int mLastFrameCount = 0;
    static nsecs_t mLastFpsTime = 0;
    static float mFps = 0;
    mFrameCount++;
    if (!(mFrameCount & 0x1F)) {
        nsecs_t now = systemTime();
        nsecs_t diff = now - mLastFpsTime;
        mFps = ((mFrameCount - mLastFrameCount) * float(s2ns(1))) / diff;
        mLastFpsTime = now;
        mLastFrameCount = mFrameCount;
        DEBUG_PRINT(3, "tvinput: %d Frames, %2.3f FPS", mFrameCount, mFps);
    }
}

// int HinDevImpl::previewBuffThread() {
//     int ret;

//     if (mState == START) {
//         if (mHinNodeInfo->currBufferHandleIndex == SIDEBAND_WINDOW_BUFF_CNT)
//              mHinNodeInfo->currBufferHandleIndex = mHinNodeInfo->currBufferHandleIndex % SIDEBAND_WINDOW_BUFF_CNT;

//         DEBUG_PRINT(mDebugLevel, "%s %d currBufferHandleIndex = %d", __FUNCTION__, __LINE__, mHinNodeInfo->currBufferHandleIndex);

//         ret = ioctl(mHinDevHandle, VIDIOC_DQBUF, &mHinNodeInfo->bufferArray[mHinNodeInfo->currBufferHandleIndex]);
//         if (ret < 0) {
//             DEBUG_PRINT(3, "VIDIOC_DQBUF Failed, error: %s", strerror(errno));
//             return -1;
//         } else {
//             DEBUG_PRINT(mDebugLevel, "VIDIOC_DQBUF successful.");
//         }

//         mSidebandWindow->buffDataTransfer(mHinNodeInfo->buffer_handle_poll[mHinNodeInfo->currBufferHandleIndex], mPreviewRawHandle[mPreviewBuffIndex].outHandle);
//         mPreviewRawHandle[mPreviewBuffIndex++].isFilled = true;
//         if (mPreviewBuffIndex == APP_PREVIEW_BUFF_CNT)
//             mPreviewBuffIndex = mPreviewBuffIndex % APP_PREVIEW_BUFF_CNT;
//         wrapCaptureResultAndNotify(mPreviewRawHandle[mPreviewBuffIndex].bufferId, mPreviewRawHandle[mPreviewBuffIndex].outHandle);
//         debugShowFPS();

//         ret = ioctl(mHinDevHandle, VIDIOC_QBUF, &mHinNodeInfo->bufferArray[mHinNodeInfo->currBufferHandleIndex]);
//         if (ret != 0) {
//             DEBUG_PRINT(3, "VIDIOC_QBUF Buffer failed %s", strerror(errno));
//         } else {
//             DEBUG_PRINT(mDebugLevel, "VIDIOC_QBUF successful.");
//         }
//         mHinNodeInfo->currBufferHandleIndex++;

//     }
//     return NO_ERROR;
// }
