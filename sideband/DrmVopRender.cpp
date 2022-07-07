#include <fcntl.h>
#include <errno.h>
#include "DrmVopRender.h"
#include "log/log.h"
#include <unistd.h>

#include <sys/mman.h>
#include <cutils/properties.h>

#include "common/TvInput_Buffer_Manager.h"
#include "common/Utils.h"

#define HAS_ATOMIC 1

#define PROPERTY_TYPE "vendor"

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG "tv_input_VopRender"
#endif

namespace android {

#define ALIGN_DOWN( value, base)     (value & (~(base-1)) )

DrmVopRender::DrmVopRender()
    : mDrmFd(0)
{
    memset(&mOutputs, 0, sizeof(mOutputs));
    mSidebandPlaneId = -1;
    ALOGE("DrmVopRender");
}

DrmVopRender::~DrmVopRender()
{
   // WARN_IF_NOT_DEINIT();
    ALOGE("DrmVopRender delete ");
}

DrmVopRender* DrmVopRender::GetInstance() {
    static DrmVopRender instance;
    return &instance;
}

bool DrmVopRender::initialize()
{
    ALOGE("initialize in");
    /*if (mInitialized) {
        ALOGE(">>Drm object has been initialized");
        return true;
    }*/

    const char *path = "/dev/dri/card0";

    mDrmFd = open(path, O_RDWR);
    if (mDrmFd < 0) {
        ALOGD("failed to open Drm, error: %s", strerror(errno));
        return false;
    }
    ALOGE("mDrmFd = %d", mDrmFd);

    memset(&mOutputs, 0, sizeof(mOutputs));
    mInitialized = true;
    int ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                      (const hw_module_t **)&gralloc_);

    if (ret) {
        ALOGE("Failed to open gralloc module");
        return ret;
    } else {
        ALOGD("open gralloc module successful !");
    }
    return true;
}

void DrmVopRender::deinitialize()
{
    ALOGE("deinitialize in");
    if(!mInitialized) return;
    for (int i = 0; i < OUTPUT_MAX; i++) {
        resetOutput(i);
    }

    if (mDrmFd) {
        close(mDrmFd);
        mDrmFd = 0;
    }

    for (const auto &fbidMap : mFbidMap) {
        int fbid = fbidMap.second;
        if (drmModeRmFB(mDrmFd, fbid))
            ALOGE("Failed to rm fb");
    }

    mInitialized = false;
}

void DrmVopRender::DestoryFB() {
    for (const auto &fbidMap : mFbidMap) {
        int fbid = fbidMap.second;
        ALOGV("%s fbid=%d", __FUNCTION__, fbid);
        if (drmModeRmFB(mDrmFd, fbid))
            ALOGE("Failed to rm fb");
    }
    mFbidMap.clear();
}

bool DrmVopRender::detect() {

    detect(HWC_DISPLAY_PRIMARY);
    return true;
}

bool DrmVopRender::detect(int device)
{
    ALOGE("detect device=%d", device);
    mSidebandPlaneId = -1;
    int outputIndex = getOutputIndex(device);
    if (outputIndex < 0 ) {
        return false;
    }

    resetOutput(outputIndex);
    drmModeConnectorPtr connector = NULL;
    DrmOutput *output = &mOutputs[outputIndex];
    bool ret = false;
    // get drm resources
    drmModeResPtr resources = drmModeGetResources(mDrmFd);
    if (!resources) {
        ALOGE("fail to get drm resources, error: %s", strerror(errno));
        return false;
    }

    ret = drmSetClientCap(mDrmFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret) {
        ALOGE("Failed to set atomic cap %s", strerror(errno));
        return ret;
    }
    ret = drmSetClientCap(mDrmFd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret) {
        ALOGE("Failed to set atomic cap %s", strerror(errno));
        return ret;
    }

    output->res = resources;
    ALOGD("resources->count_connectors=%d",resources->count_connectors);
    // find connector for the given device
    for (int i = 0; i < resources->count_connectors; i++) {
        if (!resources->connectors || !resources->connectors[i]) {
            ALOGE("fail to get drm resources connectors, error: %s", strerror(errno));
            continue;
        }

        connector = drmModeGetConnector(mDrmFd, resources->connectors[i]);
        if (!connector) {
            ALOGE("drmModeGetConnector failed");
            continue;
        }

        if (connector->connection != DRM_MODE_CONNECTED) {
            ALOGE("+++device %d is not connected", device);
            if (i == resources->count_connectors -1) {
                drmModeFreeConnector(connector);
                ret = true;
                break;
            }
            continue;
        }

        DrmModeInfo drmModeInfo;
        drmModeInfo.connector = connector;
        output->connected = true;
        ALOGD("connector %d connected",outputIndex);
        // get proper encoder for the given connector
        if (connector->encoder_id) {
            ALOGD("Drm connector has encoder attached on device %d", device);
            drmModeInfo.encoder = drmModeGetEncoder(mDrmFd, connector->encoder_id);
            if (!drmModeInfo.encoder) {
                ALOGD("failed to get encoder from a known encoder id");
                // fall through to get an encoder
            }
        }

        if (!drmModeInfo.encoder) {
            ALOGD("getting encoder for device %d", device);
            drmModeEncoderPtr encoder;
            for (int j = 0; j < resources->count_encoders; j++) {
                if (!resources->encoders || !resources->encoders[j]) {
                    ALOGE("fail to get drm resources encoders, error: %s", strerror(errno));
                    continue;
                }

                encoder = drmModeGetEncoder(mDrmFd, resources->encoders[i]);
                if (!encoder) {
                    ALOGE("drmModeGetEncoder failed");
                    continue;
                }
                ALOGD("++++encoder_type=%d,device=%d",encoder->encoder_type,getDrmEncoder(device));
                if (encoder->encoder_type == getDrmEncoder(device)) {
                    drmModeInfo.encoder = encoder;
                    break;
                }
                drmModeFreeEncoder(encoder);
                encoder = NULL;
            }
        }
        if (!drmModeInfo.encoder) {
            ALOGE("failed to get drm encoder");
            break;
        }

        // get an attached crtc or spare crtc
        if (drmModeInfo.encoder->crtc_id) {
            ALOGD("Drm encoder has crtc attached on device %d", device);
            drmModeInfo.crtc = drmModeGetCrtc(mDrmFd, drmModeInfo.encoder->crtc_id);
            if (!drmModeInfo.crtc) {
                ALOGE("failed to get crtc from a known crtc id");
                // fall through to get a spare crtc
            }
        }
        if (!drmModeInfo.crtc) {
            ALOGE("getting crtc for device %d %d", device, i);
            drmModeCrtcPtr crtc;
            for (int j = 0; j < resources->count_crtcs; j++) {
                if (!resources->crtcs || !resources->crtcs[j]) {
                    ALOGE("fail to get drm resources crtcs, error: %s", strerror(errno));
                    continue;
                }

                crtc = drmModeGetCrtc(mDrmFd, resources->crtcs[j]);
                if (!crtc) {
                    ALOGE("drmModeGetCrtc failed");
                    continue;
                }

                // check if legal crtc to the encoder
                if (drmModeInfo.encoder->possible_crtcs & (1<<j)) {
                    drmModeInfo.crtc = crtc;
                }
                drmModeObjectPropertiesPtr props;
                drmModePropertyPtr prop;
                props = drmModeObjectGetProperties(mDrmFd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC);
                if (!props) {
                    ALOGD("Failed to found props crtc[%d] %s\n",
                        crtc->crtc_id, strerror(errno));
                    continue;
                }
                for (uint32_t i = 0; i < props->count_props; i++) {
                    prop = drmModeGetProperty(mDrmFd, props->props[i]);
                    if (!strcmp(prop->name, "ACTIVE")) {
                        ALOGD("Crtc id=%d is ACTIVE.", crtc->crtc_id);
                        if (props->prop_values[i]) {
                            drmModeInfo.crtc = crtc;
                            ALOGD("Crtc id=%d is active",crtc->crtc_id);
                            break;
                        }
                    }
                }
            }
        }
        if (!drmModeInfo.crtc) {
            ALOGE("failed to get drm crtc");
            break;
        }
        output->plane_res = drmModeGetPlaneResources(mDrmFd);
        ALOGD("drmModeGetPlaneResources successful.");
        output->mDrmModeInfos.push_back(drmModeInfo);
        //break;
    }

    if (output->mDrmModeInfos.empty()) {
        ALOGD("final mDrmModeInfos is empty");
    } else {
        for (int i=0; i<output->mDrmModeInfos.size(); i++) {
            if (output->mDrmModeInfos[i].crtc) {
               ALOGD("final  crtc->crtc_id %d", output->mDrmModeInfos[i].crtc->crtc_id);
               output->mDrmModeInfos[i].props = drmModeObjectGetProperties(mDrmFd, output->mDrmModeInfos[i].crtc->crtc_id, DRM_MODE_OBJECT_CRTC);
               if (!output->mDrmModeInfos[i].props) {
                   ALOGE("Failed to found props crtc[%d] %s\n", output->mDrmModeInfos[i].crtc->crtc_id, strerror(errno));
               }
            }
        }
    }

    drmModeFreeResources(resources);

    return ret;
}

uint32_t DrmVopRender::getDrmEncoder(int device)
{
    if (device == HWC_DISPLAY_PRIMARY)
        return 2;
    else if (device == HWC_DISPLAY_EXTERNAL)
        return DRM_MODE_ENCODER_TMDS;
    return DRM_MODE_ENCODER_NONE;
}

uint32_t DrmVopRender::ConvertHalFormatToDrm(uint32_t hal_format) {
  switch (hal_format) {
    case HAL_PIXEL_FORMAT_BGR_888:
	return DRM_FORMAT_RGB888;
    case HAL_PIXEL_FORMAT_RGB_888:
      return DRM_FORMAT_BGR888;
    case HAL_PIXEL_FORMAT_BGRA_8888:
      return DRM_FORMAT_ARGB8888;
    case HAL_PIXEL_FORMAT_RGBX_8888:
      return DRM_FORMAT_XBGR8888;
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
      return DRM_FORMAT_ABGR8888;
    //Fix color error in NenaMark2.
    case HAL_PIXEL_FORMAT_RGB_565:
      return DRM_FORMAT_RGB565;
    case HAL_PIXEL_FORMAT_YV12:
      return DRM_FORMAT_YVU420;
    case HAL_PIXEL_FORMAT_YCrCb_NV12:
      return DRM_FORMAT_NV12;
    case HAL_PIXEL_FORMAT_YCrCb_NV12_10:
      return DRM_FORMAT_NV12_10;
    case HAL_PIXEL_FORMAT_YCbCr_422_SP: 
      return DRM_FORMAT_NV16;
    case HAL_PIXEL_FORMAT_YCbCr_444_888:
      return DRM_FORMAT_NV24;
    default:
      ALOGE("Cannot convert hal format to drm format %u", hal_format);
      return -EINVAL;
  }
}

int DrmVopRender::FindSidebandPlane(int device) {
    if (mSidebandPlaneId != -1) {
        return mSidebandPlaneId;
    }
    drmModePlanePtr plane;
    drmModeObjectPropertiesPtr props;
    drmModePropertyPtr prop;
    int find_plan_id = 0;
    int outputIndex = getOutputIndex(device);
    if (outputIndex < 0 ) {
        ALOGE("invalid device");
        return -1;
    }
    DrmOutput *output= &mOutputs[outputIndex];
    if (!output->connected) {
        ALOGE("device is not connected,outputIndex=%d",outputIndex);
        return -1;
    }
    //ALOGD("output->plane_res->count_planes %d", output->plane_res->count_planes);
    if (output->plane_res == NULL) {
        ALOGE("%s output->plane_res is NULL", __FUNCTION__);
        mSidebandPlaneId = -1;
        return -1;
    }
    for(uint32_t i = 0; i < output->plane_res->count_planes; i++) {
        plane = drmModeGetPlane(mDrmFd, output->plane_res->planes[i]);
        props = drmModeObjectGetProperties(mDrmFd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
        if (!props) {
            ALOGE("Failed to found props plane[%d] %s\n",plane->plane_id, strerror(errno));
           return -ENODEV;
        }
        for (uint32_t j = 0; j < props->count_props; j++) {
            prop = drmModeGetProperty(mDrmFd, props->props[j]);
            if (!strcmp(prop->name, "ASYNC_COMMIT")) {
                ALOGV("find ASYNC_COMMIT plane id=%d value=%lld", plane->plane_id, (long long)props->prop_values[j]);
                if (props->prop_values[j] != 0) {
                    find_plan_id = plane->plane_id;
                    if (prop)
                        drmModeFreeProperty(prop);
                    if (find_plan_id > 0) {
                        ALOGD("find_plan_id=%d", find_plan_id);
                        if (!output->mDrmModeInfos.empty()) {
                            for (int k=0; k<output->mDrmModeInfos.size(); k++) {
                                int plane_id = output->mDrmModeInfos[k].plane_id;
                                if (plane_id > 0) {
                                    continue;
                                }
                                output->mDrmModeInfos[k].plane_id = find_plan_id;
                                ALOGD("set plan_id=%d  to pos=%d", find_plan_id, k);
                                break;
                            }
                        }
                        mSidebandPlaneId = find_plan_id;
                        //break;
                    }

                    break;
                }
            }
            if (prop)
                drmModeFreeProperty(prop);
        }
        if(props)
            drmModeFreeObjectProperties(props);
        if(plane)
            drmModeFreePlane(plane);
        //if (find_plan_id > 0) {
        //    break;
        //}
    }
    if (find_plan_id == 0) {
       mSidebandPlaneId = -1;
       return -1;
    }
    return find_plan_id;
}

int DrmVopRender::getFbLength(buffer_handle_t handle) {
    if (!handle) {
        ALOGE("%s buffer_handle_t is NULL.", __FUNCTION__);
        return -1;
    } else {
        ALOGE("%s %p", __FUNCTION__, handle);
    }

    common::TvInputBufferManager* tvBufferMgr = common::TvInputBufferManager::GetInstance();
    return tvBufferMgr->GetHandleBufferSize(handle);
}

int DrmVopRender::getFbid(buffer_handle_t handle) {
    if (!handle) {
        ALOGE("%s buffer_handle_t is NULL.", __FUNCTION__);
        return -1;
    }

    common::TvInputBufferManager* tvBufferMgr = common::TvInputBufferManager::GetInstance();

    hwc_drm_bo_t bo;
    int fd = 0;
    int ret = 0;
    int src_w = 0;
    int src_h = 0;
    int src_format = 0;
    int src_stride = 0;

    fd = (int)tvBufferMgr->GetHandleFd(handle);
    std::map<int, int>::iterator it = mFbidMap.find(fd);
    int fbid = 0;
    if (it == mFbidMap.end()) {
        memset(&bo, 0, sizeof(hwc_drm_bo_t));
        uint32_t gem_handle;
        size_t plane_size;
        fd = (int)tvBufferMgr->GetHandleFd(handle);
        ret = drmPrimeFDToHandle(mDrmFd, fd, &gem_handle);
        src_w = tvBufferMgr->GetWidth(handle);
        src_h = tvBufferMgr->GetHeight(handle);
        src_format = tvBufferMgr->GetHalPixelFormat(handle);
        plane_size = tvBufferMgr->GetNumPlanes(handle);
        ALOGV("plane_size = %zu", plane_size);
        src_stride = (int)tvBufferMgr->GetPlaneStride(handle, 0);

        //gralloc_->perform(gralloc_, GRALLOC_MODULE_PERFORM_GET_HADNLE_WIDTH, handle, &src_w);
        //gralloc_->perform(gralloc_, GRALLOC_MODULE_PERFORM_GET_HADNLE_HEIGHT, handle, &src_h);
        //gralloc_->perform(gralloc_, GRALLOC_MODULE_PERFORM_GET_HADNLE_FORMAT, handle, &src_format);
        //gralloc_->perform(gralloc_, GRALLOC_MODULE_PERFORM_GET_HADNLE_BYTE_STRIDE, handle, &src_stride);
        bo.width = src_w;
        bo.height = src_h;
        //bo.format = ConvertHalFormatToDrm(HAL_PIXEL_FORMAT_YCrCb_NV12);
        bo.format = ConvertHalFormatToDrm(src_format);
        bo.pitches[0] = src_stride;
        bo.gem_handles[0] = gem_handle;
        bo.offsets[0] = 0;
        if(src_format == HAL_PIXEL_FORMAT_YCrCb_NV12
            || src_format == HAL_PIXEL_FORMAT_YCrCb_NV12_10
            || src_format == HAL_PIXEL_FORMAT_YCbCr_422_SP)
        {
            bo.pitches[1] = bo.pitches[0];
            bo.gem_handles[1] = gem_handle;
            bo.offsets[1] = bo.pitches[1] * bo.height;
        } else if (src_format == HAL_PIXEL_FORMAT_YCbCr_444_888) {
            bo.pitches[1] = bo.pitches[0] * 2;
            bo.gem_handles[1] = gem_handle;
            bo.offsets[1] = bo.pitches[0] * bo.height;
        }
        if (src_format == HAL_PIXEL_FORMAT_YCrCb_NV12_10) {
            bo.width = src_w / 1.25;
            bo.width = ALIGN_DOWN(bo.width, 2);
        }
        ALOGV("width=%d,height=%d,format=%x,fd=%d,src_stride=%d",bo.width, bo.height, bo.format, fd, src_stride);
        ret = drmModeAddFB2(mDrmFd, bo.width, bo.height, bo.format, bo.gem_handles,\
                     bo.pitches, bo.offsets, &bo.fb_id, 0);
        ALOGV("drmModeAddFB2 ret = %s", strerror(ret));
        fbid = bo.fb_id;
        mFbidMap.insert(std::make_pair(fd, fbid));
    } else {
        fbid = it->second;
    }
    if (fbid <= 0) {
        ALOGD("fbid is error.");
        return -1;
    }

    return fbid;
}

void DrmVopRender::resetOutput(int index)
{
    ALOGE("resetOutput index=%d", index);
    DrmOutput *output = &mOutputs[index];

    output->connected = false;
    memset(&output->mode, 0, sizeof(drmModeModeInfo));

    if (!output->mDrmModeInfos.empty()) {
        for (int i=0; i<output->mDrmModeInfos.size(); i++) {
            if (output->mDrmModeInfos[i].connector) {
                drmModeFreeConnector(output->mDrmModeInfos[i].connector);
                output->mDrmModeInfos[i].connector = 0;
            }
            if (output->mDrmModeInfos[i].encoder) {
                drmModeFreeEncoder(output->mDrmModeInfos[i].encoder);
                output->mDrmModeInfos[i].encoder = 0;
            }
            if (output->mDrmModeInfos[i].crtc) {
                drmModeFreeCrtc(output->mDrmModeInfos[i].crtc);
                output->mDrmModeInfos[i].crtc = 0;
            }
        }
        output->mDrmModeInfos.clear();
    }

    if (output->fbId) {
        drmModeRmFB(mDrmFd, output->fbId);
        output->fbId = 0;
    }
    if (output->fbHandle) {
        output->fbHandle = 0;
    }
    mSidebandPlaneId = -1;
}

bool DrmVopRender::SetDrmPlane(int device, int32_t width, int32_t height, buffer_handle_t handle) {
    ALOGV("%s come in, device=%d, handle=%p", __FUNCTION__, device, handle);
    int ret = 0;
    int plane_id = FindSidebandPlane(device);
    int fb_id = getFbid(handle);
    int flags = 0;
    int src_left = 0;
    int src_top = 0;
    int src_right = 0;
    int src_bottom = 0;
    int dst_left = 0;
    int dst_top = 0;
    int dst_right = 0;
    int dst_bottom = 0;
    int src_w = 0;
    int src_h = 0;
    int dst_w = 0;
    int dst_h = 0;
    char sideband_crop[PROPERTY_VALUE_MAX];
    memset(sideband_crop, 0, sizeof(sideband_crop));
    DrmOutput *output= &mOutputs[device];
    int length = 0;//property_get("vendor.hwc.sideband.crop", sideband_crop, NULL);
    if (length > 0) {
       sscanf(sideband_crop, "%d-%d-%d-%d-%d-%d-%d-%d",\
              &src_left, &src_top, &src_right, &src_bottom,\
              &dst_left, &dst_top, &dst_right, &dst_bottom);
       dst_w = dst_right - dst_left;
       dst_h = dst_bottom - dst_top;
    /*} else {
       dst_w = output->crtc->width;
       dst_h = output->crtc->height;*/
    }
    src_w = width;
    src_h = height;
    //gralloc_->perform(gralloc_, GRALLOC_MODULE_PERFORM_GET_HADNLE_FORMAT, handle, &src_format);
    //ALOGV("dst_w %d dst_h %d src_w %d src_h %d in", dst_w, dst_h, src_w, src_h);
    //ALOGV("mDrmFd=%d plane_id=%d, output->crtc->crtc_id=%d fb_id=%d flags=%d", mDrmFd, plane_id, output->crtc->crtc_id, fb_id, flags);
    if (!output->mDrmModeInfos.empty()) {
        for (int i=0; i<output->mDrmModeInfos.size(); i++) {
            DrmModeInfo_t drmModeInfo = output->mDrmModeInfos[i];
            plane_id = drmModeInfo.plane_id;
            if (plane_id > 0) {
                dst_w = drmModeInfo.crtc->width;
                dst_h = drmModeInfo.crtc->height;
                ret = drmModeSetPlane(mDrmFd, plane_id,
                          drmModeInfo.crtc->crtc_id, fb_id, flags,
                          dst_left, dst_top,
                          dst_w, dst_h,
                          0, 0,
                          src_w << 16, src_h << 16);
                ALOGV("drmModeSetPlane ret=%s mDrmFd=%d plane_id=%d, crtc_id=%d, fb_id=%d, flags=%d, %d %d",
                    strerror(ret), mDrmFd, plane_id, drmModeInfo.crtc->crtc_id, fb_id, flags, dst_w, dst_h);
            }
        }
    }
    ALOGV("%s end.", __FUNCTION__);
    return true;
}

bool DrmVopRender::ClearDrmPlaneContent(int device, int32_t width, int32_t height)
{
    ALOGD("%s come in, device=%d", __FUNCTION__, device);
    bool ret = true;
    int plane_id = 0;//FindSidebandPlane(device);
    // drmModeAtomicReqPtr reqPtr = drmModeAtomicAlloc();
    DrmOutput *output= &mOutputs[device];
    drmModePlanePtr plane;
    drmModeObjectPropertiesPtr props;
    drmModePropertyPtr prop;
    //props = drmModeObjectGetProperties(mDrmFd, output->crtc->crtc_id, DRM_MODE_OBJECT_CRTC);

    if (output->plane_res == NULL) {
        ALOGE("%s output->plane_res is NULL", __FUNCTION__);
        prop = NULL;
        return -1;
    }

    for(uint32_t i = 0; i < output->plane_res->count_planes; i++) {
        plane = drmModeGetPlane(mDrmFd, output->plane_res->planes[i]);
        props = drmModeObjectGetProperties(mDrmFd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
        if (!props) {
            ALOGE("Failed to found props plane[%d] %s\n",plane->plane_id, strerror(errno));
           return -ENODEV;
        }
        for (uint32_t j = 0; j < props->count_props; j++) {
            prop = drmModeGetProperty(mDrmFd, props->props[j]);
            if (!strcmp(prop->name, "ASYNC_COMMIT")) {
                if (props->prop_values[j] != 0) {
                    plane_id = plane->plane_id;
                    // ret = drmModeAtomicAddProperty(reqPtr, plane_id, prop->prop_id, 0) < 0;
                    ret =  drmModeObjectSetProperty(mDrmFd, plane_id, 0, prop->prop_id, 0) < 0;
                    if (ret) {
                        ALOGE("drmModeObjectSetProperty failed");
                        drmModeFreeProperty(prop);
                        // drmModeAtomicFree(reqPtr);
                        return false;
                    } else {
                        ALOGD("drmModeObjectSetProperty successful.");
                    }
                    break;
                }
            }
            if (prop)
                drmModeFreeProperty(prop);
        }
        if(props)
            drmModeFreeObjectProperties(props);
        if(plane)
            drmModeFreePlane(plane);
    }
    prop = NULL;


/*
    for (uint32_t i = 0; i < props->count_props; i++) {
        prop = drmModeGetProperty(mDrmFd, props->props[i]);
        ALOGD("%s prop->name=%s", __FUNCTION__, prop->name);
        if (!strcmp(prop->name, "CRTC_ID")) {
            // ret = drmModeAtomicAddProperty(reqPtr, plane_id, prop->prop_id, 0) < 0;
            ret =  drmModeObjectSetProperty(mDrmFd, plane_id, 0, prop->prop_id, 0) < 0;
            if (ret) {
                ALOGE("drmModeAtomicAddProperty failed");
                drmModeFreeProperty(prop);
                // drmModeAtomicFree(reqPtr);
                return false;
            }
        }
        if (!strcmp(prop->name, "FB_ID")) {
            // ret = drmModeAtomicAddProperty(reqPtr, plane_id, prop->prop_id, 0) < 0;
            ret =  drmModeObjectSetProperty(mDrmFd, plane_id, 0, prop->prop_id, 0) < 0;
            if (ret) {
                ALOGE("drmModeAtomicAddProperty failed");
                drmModeFreeProperty(prop);
                // drmModeAtomicFree(reqPtr);
                return false;
            }
        }
    }
    drmModeFreeProperty(prop);
    drmModeAtomicFree(reqPtr);
    prop = NULL;
*/
    /*int ret = 0;
    int plane_id = FindSidebandPlane(device);
    int flags = 0;
    int src_left = 0;
    int src_top = 0;
    int src_right = 0;
    int src_bottom = 0;
    int dst_left = 0;
    int dst_top = 0;
    int dst_right = 0;
    int dst_bottom = 0;
    int src_w = 0;
    int src_h = 0;
    int dst_w = 0;
    int dst_h = 0;
    char sideband_crop[PROPERTY_VALUE_MAX];
    memset(sideband_crop, 0, sizeof(sideband_crop));
    DrmOutput *output= &mOutputs[device];
    int length = property_get("vendor.hwc.sideband.crop", sideband_crop, NULL);
    if (length > 0) {
       sscanf(sideband_crop, "%d-%d-%d-%d-%d-%d-%d-%d",\
              &src_left, &src_top, &src_right, &src_bottom,\
              &dst_left, &dst_top, &dst_right, &dst_bottom);
       dst_w = dst_right - dst_left;
       dst_h = dst_bottom - dst_top;
    } else {
       dst_w = output->crtc->width;
       dst_h = output->crtc->height;
    }
    src_w = width;
    src_h = height;
    if (plane_id > 0) {
        ret = drmModeSetPlane(mDrmFd, plane_id,
                          0, 0, flags,
                          dst_left, dst_top,
                          dst_w, dst_h,
                          0, 0,
                          src_w << 16, src_h << 16);
        ALOGV("drmModeSetPlane ret=%s", strerror(ret));

    }*/
    return ret;
}

int DrmVopRender::getOutputIndex(int device)
{
    switch (device) {
    case HWC_DISPLAY_PRIMARY:
        return OUTPUT_PRIMARY;
    case HWC_DISPLAY_EXTERNAL:
        return OUTPUT_EXTERNAL;
    default:
        ALOGD("invalid display device");
        break;
    }

    return -1;
}

} // namespace android

