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

#include <cutils/properties.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "hinDev.h"
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicBuffer.h>
#include <linux/videodev2.h>

#define V4L2_ROTATE_ID 0x980922

#ifndef container_of
#define container_of(ptr, type, member) ({                      \
        const typeof(((type *) 0)->member) *__mptr = (ptr);     \
        (type *) ((char *) __mptr - (char *)(&((type *)0)->member)); })
#endif

#define BOUNDRY 32
#define ALIGN_32(x) ((x + (BOUNDRY) - 1)& ~((BOUNDRY) - 1))
#define ALIGN(b,w) (((b)+((w)-1))/(w)*(w))

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
        case V4L2_PIX_FMT_RGB24:
            buf_size = width * height * 3;
            break;
        case V4L2_PIX_FMT_RGB32:
            buf_size = width * height * 4;
            break;
        default:
            ALOGE("Invalid format");
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
    int nativeFormat = HAL_PIXEL_FORMAT_YCbCr_422_I;

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
        case V4L2_PIX_FMT_RGB24:
            nativeFormat = HAL_PIXEL_FORMAT_RGB_888;
            break;
        case V4L2_PIX_FMT_RGB32:
            nativeFormat = HAL_PIXEL_FORMAT_RGBA_8888;
            break;
        default:
            ALOGE("Invalid format,Use default format");
    }
    return nativeFormat;
}

/*
static ANativeWindowBuffer* handle_to_buffer(buffer_handle_t *handle)
{
    return container_of(handle, ANativeWindowBuffer, handle);
}
*/
HinDevImpl::HinDevImpl()
                  : mHinDevHandle(-1),
                    mHinNodeInfo(NULL)
{
    ALOGD("%s %d", __FUNCTION__, __LINE__);
}

int HinDevImpl::init(int id) {
    ALOGD("%s %d device_id=%d", __FUNCTION__, __LINE__, id);
    if (1) {
        if (access(HIN_DEV_NODE_MAIN, F_OK|R_OK) != 0) {
            ALOGE("%s access failed!", HIN_DEV_NODE_MAIN);
            return -1;
        } else {
            ALOGE("%s access successful!", HIN_DEV_NODE_MAIN);
        }
        mHinDevHandle = open(HIN_DEV_NODE_MAIN, O_RDWR| O_NONBLOCK);
        if (mHinDevHandle < 0)
        {
            ALOGE("[%s %d] mHinDevHandle:%x [%s]", __FUNCTION__, __LINE__, mHinDevHandle,strerror(errno));
            return -1;
        } else {
            ALOGD("%s open device %s successful.", __FUNCTION__, HIN_DEV_NODE_MAIN);
        }
    } else {
        if (access(HIN_DEV_NODE_OTHERS, F_OK|R_OK) != 0) {
            ALOGE("%s access failed!", HIN_DEV_NODE_OTHERS);
            return -1;
        }
        mHinDevHandle = open(HIN_DEV_NODE_OTHERS, O_RDWR| O_NONBLOCK);
        if (mHinDevHandle < 0)
        {
            ALOGE("[%s %d] mHinDevHandle:%x [%s]", __FUNCTION__, __LINE__, mHinDevHandle,strerror(errno));
            return -1;
        } else {
            ALOGD("%s open device %s successful.", __FUNCTION__, HIN_DEV_NODE_OTHERS);
        }
    }

    mHinNodeInfo = (struct HinNodeInfo *) calloc (1, sizeof (struct HinNodeInfo));
    if (mHinNodeInfo == NULL)
    {
        ALOGE("[%s %d] no memory for mHinNodeInfo", __FUNCTION__, __LINE__);
        close(mHinDevHandle);
        return NO_MEMORY;
    }
    memset(mHinNodeInfo, 0, sizeof(struct HinNodeInfo));
    mHinNodeInfo->currBufferHandleIndex = -1;
    mHinNodeInfo->currBufferHandleFd = -1;

    mFramecount = 0;
    mBufferCount = SIDEBAND_WINDOW_BUFF_CNT;
    mPixelFormat = V4L2_PIX_FMT_NV21;
    mNativeWindowPixelFormat = HAL_PIXEL_FORMAT_YCrCb_420_SP;
    mFrameWidth = 1920;
    mFrameHeight = 1080;
    mBufferSize = mFrameWidth * mFrameHeight * 3/2;
    mSetStateCB = NULL;
    mState = STOP;
    mANativeWindow = NULL;
    mFrameType = 0;
    mWorkThread = NULL;
    mDataCB = NULL;
    mOpen = false;

    /**
     *  init RTSidebandWindow
     */
    mSidebandWindow = new RTSidebandWindow();

    RTSidebandInfo info;
    memset(&info, 0, sizeof(RTSidebandInfo));
    info.structSize = sizeof(RTSidebandInfo);
    info.top = 0;
    info.left = 0;
    info.width = mFrameWidth;
    info.height = mFrameHeight;
    info.usage = 0; //Todo
    info.format = HAL_PIXEL_FORMAT_YCrCb_NV12; //0x15

    if(-1 == mSidebandWindow->init(info)) {
        ALOGE("mSidebandWindow->init failed !!!");
        return -1;
    }
    buffer_handle_t buffer = NULL;

    mSidebandWindow->allocateSidebandHandle(&buffer);
    if (!buffer) {
        ALOGE("allocate buffer from sideband window failed!");
        return -1;
    }
    mSidebandHandle = buffer;
	return NO_ERROR;
}

buffer_handle_t HinDevImpl::getSindebandBufferHandle() {
    return mSidebandHandle;
}

HinDevImpl::~HinDevImpl()
{
    if (mHinNodeInfo)
        free (mHinNodeInfo);
    if (mHinDevHandle >= 0)
        close(mHinDevHandle);
}

int HinDevImpl::start_device()
{
    int ret = -1;

    ALOGD("[%s %d] mHinDevHandle:%x", __FUNCTION__, __LINE__, mHinDevHandle);

    ret = ioctl(mHinDevHandle, VIDIOC_QUERYCAP, &mHinNodeInfo->cap);
    if (ret < 0) {
        ALOGE("VIDIOC_QUERYCAP Failed, error: %s", strerror(errno));
        return ret;
    }

    mHinNodeInfo->reqBuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    mHinNodeInfo->reqBuf.memory = V4L2_MEMORY_DMABUF;
    mHinNodeInfo->reqBuf.count = mBufferCount;

    ret = ioctl(mHinDevHandle, VIDIOC_REQBUFS, &mHinNodeInfo->reqBuf);
    if (ret < 0) {
        ALOGE("VIDIOC_REQBUFS Failed, error: %s", strerror(errno));
        return ret;
    }

    for (int i = 0; i < mBufferCount; i++) {
        memset(&mHinNodeInfo->bufferArray[i], 0, sizeof(struct v4l2_buffer));

        mHinNodeInfo->bufferArray[i].index = i;
        mHinNodeInfo->bufferArray[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        mHinNodeInfo->bufferArray[i].memory = V4L2_MEMORY_DMABUF;

        ret = ioctl(mHinDevHandle, VIDIOC_QUERYBUF, &mHinNodeInfo->bufferArray[i]);
        if (ret < 0) {
            ALOGE("VIDIOC_QUERYBUF Failed, error: %s", strerror(errno));
            return ret;
        }

        ret = mSidebandWindow->allocateBuffer(&mHinNodeInfo->handle_t[i]);
        if (ret != 0) {
            ALOGE("mSidebandWindow->allocateBuffer failed !!!");
            return ret;
        }
        mHinNodeInfo->bufferArray[i].m.fd = mSidebandWindow->getBufferHandleFd(mHinNodeInfo->handle_t[i]);
        mHinNodeInfo->bufferArray[i].length = mSidebandWindow->getBufferLength(mHinNodeInfo->handle_t[i]);
        mBufferHandleMap.insert(std::make_pair(mHinNodeInfo->bufferArray[i].m.fd, mHinNodeInfo->handle_t[i]));
    }
    ALOGD("[%s %d] VIDIOC_QUERYBUF successful", __FUNCTION__, __LINE__);

    for (int i = 0; i < mBufferCount; i++) {
        ALOGD("bufferArray index = %d", mHinNodeInfo->bufferArray[i].index);
        ALOGD("bufferArray type = %d", mHinNodeInfo->bufferArray[i].type);
        ALOGD("bufferArray memory = %d", mHinNodeInfo->bufferArray[i].memory);
        ALOGD("bufferArray m.fd = %d", mHinNodeInfo->bufferArray[i].m.fd);
        ALOGD("bufferArray length = %d", mHinNodeInfo->bufferArray[i].length);
        ret = ioctl(mHinDevHandle, VIDIOC_QBUF, &mHinNodeInfo->bufferArray[i]);
        if (ret < 0) {
            ALOGE("VIDIOC_QBUF Failed, error: %s", strerror(errno));
            return -1;
        }
    }
    enum v4l2_buf_type bufType;
    bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ret = ioctl (mHinDevHandle, VIDIOC_STREAMON, &bufType);
    if (ret < 0) {
        ALOGE("VIDIOC_STREAMON Failed, error: %s", strerror(errno));
        return -1;
    }

    ALOGD("[%s %d] VIDIOC_STREAMON:%x", __FUNCTION__, __LINE__, ret);
    return ret;
}

int HinDevImpl::stop_device()
{
    ALOGE("%s %d", __FUNCTION__, __LINE__);
    int ret;
    enum v4l2_buf_type bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl (mHinDevHandle, VIDIOC_STREAMOFF, &bufType);
    if (ret < 0) {
        ALOGE("StopStreaming: Unable to stop capture: %s", strerror(errno));
    }
    return ret;
}

int HinDevImpl::cacheToDisplay() {
    int ret = -1;

    mHinNodeInfo->queueBuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    mHinNodeInfo->queueBuf.memory = V4L2_MEMORY_DMABUF;
    ret = ioctl(mHinDevHandle, VIDIOC_DQBUF, &mHinNodeInfo->queueBuf);
    if (ret < 0) {
        ALOGE("VIDIOC_DQBUF Failed, error: %s", strerror(errno));
        return -1;
    }

    ALOGD("makeDisplayCache -> VIDIOC_DQBUF queueBuf.index=%d", mHinNodeInfo->queueBuf.index);
    ALOGD("makeDisplayCache -> VIDIOC_DQBUF queueBuf.m.fd=%d", mHinNodeInfo->queueBuf.m.fd);
    ALOGD("makeDisplayCache -> VIDIOC_DQBUF queueBuf.tv_sec=%ld", mHinNodeInfo->queueBuf.timestamp.tv_sec);
    ALOGD("makeDisplayCache -> VIDIOC_DQBUF queueBuf.tv_usec=%ld", mHinNodeInfo->queueBuf.timestamp.tv_usec);

    std::map<int, buffer_handle_t>::iterator it;
    for (it=mBufferHandleMap.begin(); it!= mBufferHandleMap.end(); ++it) {
        int tmpHandleId = it->first;
        buffer_handle_t tmpHandle_t = it->second;
        if (tmpHandleId == mHinNodeInfo->queueBuf.m.fd) {
            mSidebandWindow->remainBuffer(tmpHandle_t);
            mHinNodeInfo->currBufferHandleIndex = mHinNodeInfo->queueBuf.index;
            mHinNodeInfo->currBufferHandleFd = tmpHandleId;
            break;
        }
    }

    memset(&mHinNodeInfo->queueBuf,0,sizeof(v4l2_buffer));
    mHinNodeInfo->queueBuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    mHinNodeInfo->queueBuf.memory = V4L2_MEMORY_DMABUF;
    mHinNodeInfo->queueBuf.m.fd = mHinNodeInfo->currBufferHandleFd;
    mHinNodeInfo->queueBuf.length = mSidebandWindow->getBufferLength(mHinNodeInfo->handle_t[mHinNodeInfo->currBufferHandleIndex]);

    ret = ioctl(mHinDevHandle, VIDIOC_QBUF, &mHinNodeInfo->queueBuf);
    if (ret < 0) {
        ALOGE("VIDIOC_QBUF Failed, error: %s", strerror(errno));
        return -1;
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
        ALOGE("Start v4l2 device failed:%d",ret);
        return ret;
    }

    ret = cacheToDisplay();
    if (ret != 0) {
        ALOGE("cacheToDisplay Buffer failed");
    }

    ALOGD("Create Work Thread");
    mWorkThread = new WorkThread(this);

    mState = START;
    mOpen = true;
    ALOGD("%s %d ret:%d", __FUNCTION__, __LINE__, ret);
    return NO_ERROR;
}


int HinDevImpl::stop()
{
    ALOGE("!!!!!!!!!%s %d", __FUNCTION__, __LINE__);
    int ret;
    mState = STOPING;

    if(mWorkThread != NULL){
        mWorkThread->requestExitAndWait();
        mWorkThread.clear();
    }

    enum v4l2_buf_type bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl (mHinDevHandle, VIDIOC_STREAMOFF, &bufType);
    if (ret < 0) {
        ALOGE("StopStreaming: Unable to stop capture: %s", strerror(errno));
    }

    if (mBufferHandleMap.size() != 0) {
        mBufferHandleMap.clear();
    }

    if (mSidebandHandle) {
        mSidebandWindow->freeBuffer(&mSidebandHandle);
        mSidebandWindow->flush();
    }
    if (mSidebandWindow.get()) {
        mSidebandWindow->release();
        mSidebandWindow.clear();
    }

    mBufferCount = 0;
    mState = STOP;
    if(mSetStateCB != NULL)
        mSetStateCB(STOP);
    mOpen = false;
    return ret;
}

int HinDevImpl::set_state_callback(olStateCB callback)
{
    if (!callback){
        ALOGE("NULL state callback pointer");
        return BAD_VALUE;
    }
    mSetStateCB = callback;
    return NO_ERROR;
}

int HinDevImpl::set_data_callback(app_data_callback callback, void* user)
{
    ALOGD("%s %d", __FUNCTION__, __LINE__);
    if (callback == NULL){
        ALOGE("NULL data callback pointer");
        return BAD_VALUE;
    }
    mDataCB = callback;
    mUser = user;
    mFrameType |= CALL_BACK_DATA;
    return NO_ERROR;
}

int HinDevImpl::get_format()
{
    return mPixelFormat;
}

int HinDevImpl::set_mode(int displayMode)
{
    ALOGE("run into set_mode,displaymode = %d\n", displayMode);
    mHinNodeInfo->displaymode = displayMode;
    m_displaymode = displayMode;
    return 0;
}

int HinDevImpl::set_format(int width, int height, int color_format)
{
    ALOGD("[%s %d]", __FUNCTION__, __LINE__);
    Mutex::Autolock autoLock(mLock);
    if (mOpen == true)
        return NO_ERROR;
    int ret;

    mHinNodeInfo->width = width;
    mHinNodeInfo->height = height;
    mHinNodeInfo->formatIn = color_format;
    mHinNodeInfo->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    mHinNodeInfo->format.fmt.pix.width = width;
    mHinNodeInfo->format.fmt.pix.height = height;
    mHinNodeInfo->format.fmt.pix.pixelformat = color_format;
    mFrameWidth = width;
    mFrameHeight = height;
    ret = ioctl(mHinDevHandle, VIDIOC_S_FMT, &mHinNodeInfo->format);
    if (ret < 0) {
        ALOGE("[%s %d] failed, set VIDIOC_S_FMT %d, %s", __FUNCTION__, __LINE__, ret, strerror(ret));
        return ret;
    }

    mSidebandWindow->setBufferGeometry(mFrameWidth, mFrameHeight, color_format);
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
        ALOGE("Set rotate value invalid: %d.", degree);
        return -1;
    }

    memset( &ctl, 0, sizeof(ctl));
    ctl.value=degree;
    ctl.id = V4L2_ROTATE_ID;
    ret = ioctl(mHinDevHandle, VIDIOC_S_CTRL, &ctl);

    if(ret<0){
        ALOGE("Set rotate value fail: %s. ret=%d", strerror(errno),ret);
    }
    return ret ;
}

int HinDevImpl::set_crop(int x, int y, int width, int height)
{
    ALOGD("[%s %d]", __FUNCTION__, __LINE__);
    mSidebandWindow->setCrop(x, y, width, height);
    return NO_ERROR;
}

int HinDevImpl::set_frame_rate(int frameRate)
{
    ALOGD("[%s %d]", __FUNCTION__, __LINE__);
    int ret = 0;
    //struct v4l2_control ctl;

    if(mHinDevHandle<0)
        return -1;

    struct v4l2_streamparm sparm;
    memset(&sparm, 0, sizeof( sparm ));
    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;//stream_flag;
    sparm.parm.output.timeperframe.denominator = frameRate;
    sparm.parm.output.timeperframe.numerator = 1;

    ret = ioctl(mHinDevHandle, VIDIOC_S_PARM, &sparm);
    if(ret < 0){
        ALOGE("Set frame rate fail: %s. ret=%d", strerror(errno),ret);
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
        ALOGE("get amlvideo2 crop  fail: %s. ret=%d", strerror(errno),ret);
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

    crop.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    crop.c.left = x;
    crop.c.top = y;
    crop.c.width = width;
    crop.c.height = height;
    ret = ioctl(mHinDevHandle, VIDIOC_S_CROP, &crop);
    if (ret) {
        ALOGE("Set amlvideo2 crop  fail: %s. ret=%d", strerror(errno),ret);
    }

    return ret ;
}

int HinDevImpl::get_current_sourcesize(int *width,  int *height)
{
    int ret = NO_ERROR;
    struct v4l2_format format;
    memset(&format, 0,sizeof(struct v4l2_format));

    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ret = ioctl(mHinDevHandle, VIDIOC_G_FMT, &format);
    if (ret < 0) {
        ALOGE("Open: VIDIOC_G_FMT Failed: %s", strerror(errno));
        return ret;
    }
    *width = format.fmt.pix.width;
    *height = format.fmt.pix.height;
    ALOGD("VIDIOC_G_FMT, w * h: %5d x %5d", *width,  *height);
    return ret;
}

int HinDevImpl::set_screen_mode(int mode)
{
    int ret = NO_ERROR;
    ret = ioctl(mHinDevHandle, VIDIOC_S_OUTPUT, &mode);
    if (ret < 0) {
        ALOGE("VIDIOC_S_OUTPUT Failed: %s", strerror(errno));
        return ret;
    }
    return ret;
}

int HinDevImpl::aquire_buffer()
{
    ALOGE("%s %d", __FUNCTION__, __LINE__);
    int ret = -1;
    Mutex::Autolock autoLock(mLock);
    mHinNodeInfo->queueBuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    mHinNodeInfo->queueBuf.memory = V4L2_MEMORY_DMABUF;

    ret = ioctl(mHinDevHandle, VIDIOC_DQBUF, &mHinNodeInfo->queueBuf);
    if (ret < 0) {
        ALOGE("VIDIOC_DQBUF Failed, error: %s", strerror(errno));
        return ret;
    }

    ALOGD("VIDIOC_DQBUF queueBuf.index=%d", mHinNodeInfo->queueBuf.index);
    ALOGD("VIDIOC_DQBUF queueBuf.m.fd=%d", mHinNodeInfo->queueBuf.m.fd);
    ALOGD("VIDIOC_DQBUF queueBuf.tv_sec=%ld", mHinNodeInfo->queueBuf.timestamp.tv_sec);
    ALOGD("VIDIOC_DQBUF queueBuf.tv_usec=%ld", mHinNodeInfo->queueBuf.timestamp.tv_usec);

    std::map<int, buffer_handle_t>::iterator it;
    for (it=mBufferHandleMap.begin(); it!= mBufferHandleMap.end(); ++it) {
        int tmpHandleId = it->first;
        buffer_handle_t tmpHandle_t = it->second;
        if (tmpHandleId == mHinNodeInfo->queueBuf.m.fd) {
            mSidebandWindow->queueBuffer(tmpHandle_t);
            mHinNodeInfo->currBufferHandleIndex = mHinNodeInfo->queueBuf.index;
            mHinNodeInfo->currBufferHandleFd = tmpHandleId;
            break;
        }
    }

    ALOGE("%s finish %d ", __FUNCTION__, __LINE__);
    return ret;
}

int HinDevImpl::release_buffer()
{
    ALOGD("%s %d", __FUNCTION__, __LINE__);
    v4l2_buffer qBuf;
    int ret = -1;

    Mutex::Autolock autoLock(mLock);
    memset(&qBuf,0,sizeof(v4l2_buffer));
    if (mHinNodeInfo->currBufferHandleIndex != -1) {
        mSidebandWindow->dequeueBuffer(&(mHinNodeInfo->handle_t[mHinNodeInfo->currBufferHandleIndex]));
    } else {
        return ret;
    }
    
    qBuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    qBuf.memory = V4L2_MEMORY_DMABUF;
    qBuf.m.fd = mHinNodeInfo->currBufferHandleFd;
    qBuf.length = mSidebandWindow->getBufferLength(mHinNodeInfo->handle_t[mHinNodeInfo->currBufferHandleIndex]);

    ret = ioctl(mHinDevHandle, VIDIOC_QBUF, &qBuf);
    if (ret != 0) {
        ALOGE("VIDIOC_QBUF Buffer failed %s", strerror(errno));
    }
    return ret;
}

int HinDevImpl::workThread()
{
    ALOGD("HinDevImpl::workThread()");
    int ret;
    if (mState == START) {
        ret = aquire_buffer();
        if (ret == -1) {
            return BAD_VALUE;
        }
        usleep(1000);
        ret = release_buffer();
        if (ret == -1) {
            return BAD_VALUE;
        }

    }
    return NO_ERROR;
}

