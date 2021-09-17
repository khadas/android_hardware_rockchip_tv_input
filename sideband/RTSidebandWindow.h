/*
 * Copyright 2019 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * author: rimon.xu@rock-chips.com
 *   date: 2019/12/31
 * module: sideband window
 */

#ifndef ROCKIT_OSAL_RTSIDEBANDWINDOW_H_
#define ROCKIT_OSAL_RTSIDEBANDWINDOW_H_

#include "tvinput_buffer_manager_gralloc4_impl.h"
#include <cutils/native_handle.h>
#include <utils/RefBase.h>
#include <hardware/hardware.h>
#include <hardware/gralloc.h>
#include "MessageQueue.h"
#include "MessageThread.h"
#include "BufferData.h"
#include <utils/Errors.h>
#include <utils/Mutex.h>
#include <utils/Condition.h>
#include <vector>
#include <system/window.h>
#include <sys/types.h>
#include <inttypes.h>
#include "rt_type.h"   // NOLINT
#include "Utils.h"

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG "tv_input_Sideband"
#endif

namespace android {

typedef struct RT_SIDEBAND_INFO {
    INT32 structSize;
    INT32 structVersion;
    INT32 left;
    INT32 top;
    INT32 right;
    INT32 bottom;
    INT32 usage;
    INT32 width;
    INT32 height;
    INT32 format;
    INT32 dataSpace;
    INT32 transform;
} RTSidebandInfo;

class DrmVopRender;
class RTSidebandWindow : public RefBase, IMessageHandler {
 public:
    RTSidebandWindow();
    virtual ~RTSidebandWindow();

    enum MessageId {
        MESSAGE_ID_UNKNOWN = 0,
        MESSAGE_ID_EXIT,
        MESSAGE_ID_RENDER_REQUEST,
        MESSAGE_ID_DEQUEUE_REQUEST,
        MESSAGE_ID_FLUSH,
        MESSAGE_ID_MAX
    };
    struct Message {
        MessageId id;
        rt_stream_buffer_t streamBuffer;
    };

    status_t init(RTSidebandInfo info);
    status_t release();
    status_t start();
    status_t stop();
    status_t flush();

    status_t allocateBuffer(buffer_handle_t *buffer);
    status_t freeBuffer(buffer_handle_t *buffer);
    status_t remainBuffer(buffer_handle_t buffer);
    status_t dequeueBuffer(buffer_handle_t *buffer);
    status_t queueBuffer(buffer_handle_t buffer);
    status_t allocateSidebandHandle(buffer_handle_t *handle);
    int getBufferHandleFd(buffer_handle_t buffer);
    int getBufferLength(buffer_handle_t buffer);

    status_t setBufferGeometry(int32_t width, int32_t height, int32_t format);
    status_t setCrop(int32_t left, int32_t top, int32_t right, int32_t bottom);

    status_t dumpImage(buffer_handle_t handle, char* fileName, int mode);

    int32_t  getWidth() { return mSidebandInfo.width; }
    int32_t  getHeight() { return mSidebandInfo.height; }
    int32_t  getFormat() { return mSidebandInfo.format; }
    status_t goDisplay(buffer_handle_t buffer);

 private:
    RTSidebandWindow(const RTSidebandWindow& other);
    RTSidebandWindow& operator=(const RTSidebandWindow& other);

    virtual void messageThreadLoop();
    virtual status_t requestExitAndWait();
    virtual status_t handleMessageExit();
    virtual status_t handleRenderRequest(Message &msg);
    virtual status_t handleDequeueRequest(Message &msg);
    virtual status_t handleFlush();

 private:
    common::TvInputBufferManager* mBuffMgr;
    alloc_device_t      *mAllocDevice;
    DrmVopRender        *mVopRender;
    RTSidebandInfo       mSidebandInfo;

    bool                                mThreadRunning;
    MessageQueue<Message, MessageId>    mMessageQueue;
    std::vector<buffer_handle_t>        mRenderingQueue;
    std::unique_ptr<MessageThread>      mMessageThread;
    android::Mutex                      mLock;
    android::Condition                  mBufferAvailCondition;
    int mDebugLevel;
};

}


#endif

