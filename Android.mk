# Copyright (C) 2021 The Rockchip HDMI input Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := tv_input.rockchip
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_CFLAGS += -Wno-unused-parameter -Wno-uninitialized -Wno-unused-variable -Wno-unused-function

LOCAL_SRC_FILES := \
    common/TvInput_Buffer_Manager_gralloc4_impl.cpp \
    sideband/RTSidebandWindow.cpp \
    sideband/DrmVopRender.cpp \
    sideband/MessageThread.cpp \
    HinDevImpl.cpp \
    TvDeviceV4L2Event.cpp \
    tv_input.cpp

LOCAL_SHARED_LIBRARIES := \
    libbase \
    libcutils \
    libutils \
    libbinder \
    libui \
    liblog \
    libdrm \
    libhardware

LOCAL_C_INCLUDES += \
    system/core/libion/include \
    system/core/libion/kernel-headers \
    hardware/libhardware/include \
    hardware/libhardware/include/hardware \
    system/core/libutils/include \
	system/core/libcutils/include/cutils/ \
    hardware/rockchip/libgralloc/bifrost \
    hardware/rockchip/libgralloc/bifrost/src \
    hardware/rockchip/hdmi_capture \
    frameworks/native/libs/nativewindow/include \
    $(LOCAL_PATH)/common \
    $(LOCAL_PATH)/sideband

LOCAL_STATIC_LIBRARIES += \
       libgrallocusage

LOCAL_SHARED_LIBRARIES += \
    android.hardware.graphics.allocator@2.0 \
    android.hardware.graphics.allocator@3.0 \
    android.hardware.graphics.allocator@4.0 \
    android.hardware.graphics.common@1.2 \
    android.hardware.graphics.mapper@2.0 \
    android.hardware.graphics.mapper@2.1 \
    android.hardware.graphics.mapper@3.0 \
    android.hardware.graphics.mapper@4.0 \
    libgralloctypes \
    libhidlbase \
    libsync_vendor

LOCAL_HEADER_LIBRARIES += \
    android.hardware.graphics.common@1.2 \
    android.hardware.graphics.mapper@4.0 \
    libgralloctypes


include $(BUILD_SHARED_LIBRARY)
