/*
 * Copyright (c) 2021 Rockchip Electronics Co., Ltd
 */

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

#include <unordered_map>
#include <utils/KeyedVector.h>
#include <cutils/properties.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <utils/threads.h>
#include <hardware/gralloc.h>
#include <hardware/tv_input.h>
#include <map>
#include "TvDeviceV4L2Event.h"
#include "sideband/RTSidebandWindow.h"
#include "common/RgaCropScale.h"
#include "common/HandleImporter.h"
#include "common/rk_hdmirx_config.h"
#include <rkpq.h>
#include "rkiep.h"
#include "MppEncodeServer.h"
#include "RKMppEncApi.h"

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG "tv_input"
#endif

using namespace android;
using ::android::tvinput::RgaCropScale;

typedef struct source_buffer_info {
    buffer_handle_t source_buffer_handle_t;
    int    reserved_data;
    long   tv_sec;
    long   tv_usec;
} source_buffer_info_t;

struct HinNodeInfo {
    struct v4l2_capability cap;
    struct v4l2_format format;
    struct v4l2_plane planes[SIDEBAND_WINDOW_BUFF_CNT];
    struct v4l2_buffer onceBuff;
    struct v4l2_requestbuffers reqBuf;
    struct v4l2_buffer bufferArray[SIDEBAND_WINDOW_BUFF_CNT];
    buffer_handle_t buffer_handle_poll[SIDEBAND_WINDOW_BUFF_CNT];
//    long *mem[SIDEBAND_WINDOW_BUFF_CNT];
//    unsigned reservedData[SIDEBAND_WINDOW_BUFF_CNT];
//    unsigned refcount[SIDEBAND_WINDOW_BUFF_CNT];
    int currBufferHandleFd;
    int currBufferHandleIndex;
    bool isStreaming;
    int width;
    int height;
    int formatIn;
    int framesizeIn;
    int displaymode;
};

typedef struct tv_record_buffer_info {
    buffer_handle_t outHandle;
    int width;
    int height;
    int verStride;
    int horStride;
    bool isCoding;
} tv_record_buffer_info_t;

typedef struct tv_pq_buffer_info {
    buffer_handle_t srcHandle = NULL;
    buffer_handle_t outHandle;
    bool isFilled;
} tv_pq_buffer_info_t;

enum State {
    START,
    PAUSE,
    STOPING,
    STOPED,
};

typedef struct tv_preview_buff_app {
    int bufferFd;
    uint64_t bufferId;
    // buffer_handle_t rawHandle;
    buffer_handle_t outHandle;
    bool isRendering;
    bool isFilled;
} tv_preview_buff_app_t;

// typedef struct tv_input_preview_buff {
//     uint64_t bufferId;
//     buffer_handle_t* buffPtr;
//     bool isRendering;
//     bool isFilled;
// } tv_input_preview_buff_t;

typedef void (*NotifyQueueDataCallback)(tv_input_capture_result_t result);

typedef void (*app_data_callback)(void *user, source_buffer_info_t *buff_info);

#define HIN_GRALLOC_USAGE  GRALLOC_USAGE_HW_TEXTURE | \
                                    GRALLOC_USAGE_HW_RENDER | \
                                    GRALLOC_USAGE_SW_READ_RARELY | \
                                    GRALLOC_USAGE_SW_WRITE_NEVER

#ifndef container_of
#define container_of(ptr, type, member) \
    (type *)((char*)(ptr) - offsetof(type, member))
#endif

static std::vector<tv_record_buffer_info_t> mRecordHandle;
//static std::mutex mRecordMutex;

class HinDevImpl {
    public:
        HinDevImpl();
        ~HinDevImpl();
        int init(int id,int type);
        int findDevice(int id, int& initWidth, int& initHeight,int& initFormat);
        int start();
        int stop();
        int pause();
	int get_format(int fd, int &hdmi_in_width, int &hdmi_in_height,int& initFormat);
        int set_format(int width = 640, int height = 480, int color_format = V4L2_PIX_FMT_NV21);
        int get_HdmiIn(bool enforce);
        int set_rotation(int degree);
        int set_crop(int x, int y, int width, int height);
        int get_hin_crop(int *x, int *y, int *width, int *height);
        int set_hin_crop(int x, int y, int width, int height);
        int set_preview_info(int top, int left, int width, int height);
        int set_preview_buffer(buffer_handle_t rawHandle, uint64_t bufferId);
        int aquire_buffer();
        // int inc_buffer_refcount(int* ptr);
        int release_buffer();
        int set_preview_callback(NotifyQueueDataCallback callback);
        int set_data_callback(V4L2EventCallBack callback);
        int set_frame_rate(int frameRate);
        int get_current_sourcesize(int&  width,int&  height,int& format);
        int set_screen_mode(int mode);
        int start_device();
        int stop_device();
        int set_mode(int display_mode);
        buffer_handle_t getSindebandBufferHandle();
        int deal_priv_message(const string action, const map<string, string> data);
        int request_capture(buffer_handle_t rawHandle, uint64_t bufferId);
        bool check_zme(int src_width, int src_height, int* dst_width, int* dst_height);
        int check_interlaced();
        void set_interlaced(int interlaced);

        const tv_input_callback_ops_t* mTvInputCB;

    //Just for first start encoding thread control
    bool mEncodeThreadRunning = false;
    MppEncodeServer *gMppEnCodeServer=nullptr;
    private:
        int workThread();
        int pqBufferThread();
        int iepBufferThread();
        int getPqFmt(int V4L2Fmt);
        // int previewBuffThread();
        int makeHwcSidebandHandle();
        void debugShowFPS();
        void wrapCaptureResultAndNotify(uint64_t buffId, buffer_handle_t handle);
        void doRecordCmd(const map<string, string> data);
        void doPQCmd(const map<string, string> data);
        int getRecordBufferFd(int previewHandlerIndex);
        int init_encodeserver(MppEncodeServer::MetaInfo* info);
    void deinit_encodeserver();
        void stopRecord();
        void buffDataTransfer(buffer_handle_t srcHandle, int srcFmt, int srcWidth, int srcHeight,
            buffer_handle_t dstHandle, int dstFmt, int dstWidth, int dstHeight, int dstWStride, int dstHStride);
    private:
        class WorkThread : public Thread {
            HinDevImpl* mSource;
            public:
                WorkThread(HinDevImpl* source) :
                    Thread(false), mSource(source) { }
                virtual void onFirstRef() {
                    run("hdmi_input_source work thread", PRIORITY_URGENT_DISPLAY);
                }
                virtual bool threadLoop() {
                    mSource->workThread();
                    // loop until we need to quit
                    return true;
                }
        };
        class PqBufferThread : public Thread {
            HinDevImpl* mSource;
            public:
                PqBufferThread(HinDevImpl* source) :
                    Thread(false), mSource(source) { }
                virtual void onFirstRef() {
                    run("hdmi_input_source pq buffer thread", PRIORITY_URGENT_DISPLAY);
                }
                virtual bool threadLoop() {
                    mSource->pqBufferThread();
                    // loop until we need to quit
                    return true;
                }
        };

        class IepBufferThread : public Thread {
            HinDevImpl* mSource;
            public:
                IepBufferThread(HinDevImpl* source) :
                    Thread(false), mSource(source) { }
                virtual void onFirstRef() {
                    run("hdmi_input_source iep buffer thread", PRIORITY_URGENT_DISPLAY);
                }
                virtual bool threadLoop() {
                    mSource->iepBufferThread();
                    // loop until we need to quit
                    return true;
                }
        };

        // class PreviewBuffThread : public Thread {
        //     HinDevImpl* mSource;
        //     public:
        //         PreviewBuffThread(HinDevImpl* source) :
        //             Thread(false), mSource(source) { }
        //         virtual void onFirstRef() {
        //             run("hdmi_input_source preview buff work thread", PRIORITY_URGENT_DISPLAY);
        //         }
        //         virtual bool threadLoop() {
        //             mSource->previewBuffThread();
        //             // loop until we need to quit
        //             return true;
        //         }
        // };
    private:
        //int mCurrentIndex;
        //KeyedVector<long *, long> mBufs;
        //KeyedVector<long *, long> mTemp_Bufs;
        int mBufferCount;
        int mSrcFrameWidth;
        int mSrcFrameHeight;
        int mDstFrameWidth;
        int mDstFrameHeight;
        int mFrameFps;
        int mFrameColorRange = HDMIRX_DEFAULT_RANGE;
        int mFrameColorSpace = HDMIRX_XVYCC709;
        int mBufferSize;
        bool mIsHdmiIn;
        unsigned int flex_ratio;
        unsigned int flex_original;
        int mFramecount = 0;
        int m_FrameHeight = 0;
        int m_FrameWidth = 0;
        int m_rest = 0;
        int m_displaymode;
        volatile int mState;
        NotifyQueueDataCallback mNotifyQueueCb;
        int mPixelFormat;
        int mNativeWindowPixelFormat;
        sp<ANativeWindow> mANativeWindow;
        sp<WorkThread>   mWorkThread;
        sp<PqBufferThread> mPqBufferThread;
        sp<IepBufferThread> mIepBufferThread;
        // sp<PreviewBuffThread>   mPreviewBuffThread;
        mutable Mutex mLock;
        Mutex mBufferLock;
        int mHinDevHandle;
        struct HinNodeInfo *mHinNodeInfo;
        sp<V4L2DeviceEvent>     mV4l2Event;
        buffer_handle_t mSignalHandle = NULL;
        buffer_handle_t         mSidebandHandle;
        sp<RTSidebandWindow>    mSidebandWindow;
        int mFrameType;
        bool mOpen;
        int mDebugLevel;
        int mSkipFrame;
        bool mDumpType;
        int mShowFps;
        int mDumpFrameCount;
        void *mUser;
        bool mV4L2DataFormatConvert;
        int mPreviewBuffIndex = 0;
        bool mFirstRequestCapture;
        int mRequestCaptureCount = 0;
        std::vector<tv_preview_buff_app_t> mPreviewRawHandle;
        std::vector<tv_pq_buffer_info_t> mIepBufferHandle;
        int mRecordCodingBuffIndex = 0;
        int mDisplayRatio = FULL_SCREEN;
        int mPqMode = PQ_OFF;
        int mOutRange = HDMIRX_DEFAULT_RANGE;
        int mLastOutRange = mOutRange;
        std::vector<tv_pq_buffer_info_t> mPqBufferHandle;
        int mPqBuffIndex = 0;
        int mPqBuffOutIndex = 0;
        rkpq *mRkpq=nullptr;
        bool mUseZme;
        rkiep *mRkiep=nullptr;
        int mIepBuffIndex = 0;
        int mIepBuffOutIndex = 0;
        bool mUseIep = false;
        bool mPqIniting = false;
        int mLastPqStatus = 0;
        int mEnableDump = 0;
        // std::vector<tv_input_preview_buff_t> mPreviewBuff;
};
