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

#include <utils/KeyedVector.h>
#include <cutils/properties.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <utils/threads.h>
#include <android/native_window.h>
#include <system/window.h>
#include <hardware/gralloc.h>
#include <map>

#include "sideband/RTSidebandWindow.h"
#include "common/Utils.h"

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG "tv_input"
#endif

using namespace android;

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

enum State{
    START,
    PAUSE,
    STOPING,
    STOP,
};

enum FrameType{
    NATIVE_WINDOW_DATA = 0x1,
    CALL_BACK_DATA = 0x2,
};

typedef void (*olStateCB)(int state);

typedef void (*app_data_callback)(void *user, source_buffer_info_t *buff_info);

#define HIN_GRALLOC_USAGE  GRALLOC_USAGE_HW_TEXTURE | \
                                    GRALLOC_USAGE_HW_RENDER | \
                                    GRALLOC_USAGE_SW_READ_RARELY | \
                                    GRALLOC_USAGE_SW_WRITE_NEVER

class HinDevImpl {
    public:
        HinDevImpl();
        ~HinDevImpl();
        int init(int id);
        int start();
        int stop();
        int pause();
        int get_format();
        int set_format(int width = 640, int height = 480, int color_format = V4L2_PIX_FMT_NV21);
        int set_rotation(int degree);
        int set_crop(int x, int y, int width, int height);
        int get_hin_crop(int *x, int *y, int *width, int *height);
        int set_hin_crop(int x, int y, int width, int height);
        int aquire_buffer();
        // int inc_buffer_refcount(int* ptr);
        int release_buffer();
        int set_state_callback(olStateCB callback);
        int set_data_callback(app_data_callback callback, void* user);
        int set_frame_rate(int frameRate);
        int get_current_sourcesize(int * width,int * height);
        int set_screen_mode(int mode);
        int start_device();
        int stop_device();
        int set_mode(int display_mode);
        buffer_handle_t getSindebandBufferHandle();
    private:
        int workThread();
        int makeHwcSidebandHandle();
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
    private:
        //int mCurrentIndex;
        //KeyedVector<long *, long> mBufs;
        //KeyedVector<long *, long> mTemp_Bufs;
        int mBufferCount;
        int mFrameWidth;
        int mFrameHeight;
        int mBufferSize;
        unsigned int flex_ratio;
        unsigned int flex_original;
        int mFramecount = 0;
        int m_FrameHeight = 0;
        int m_FrameWidth = 0;
        int m_rest = 0;
        int m_displaymode;
        volatile int mState;
        olStateCB mSetStateCB;
        int mPixelFormat;
        int mNativeWindowPixelFormat;
        sp<ANativeWindow> mANativeWindow;
        sp<WorkThread>   mWorkThread;
        mutable Mutex mLock;
        int mHinDevHandle;
        struct HinNodeInfo *mHinNodeInfo;
        buffer_handle_t         mSidebandHandle;
        sp<RTSidebandWindow>    mSidebandWindow;
        int mFrameType;
        app_data_callback mDataCB;
        bool mOpen;
        int mDebugLevel;
        int mSkipFrame;
        bool mDumpType;
        int mDumpFrameCount;
        void *mUser;
        std::map<int, buffer_handle_t> mBufferHandleMap;
};
