LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE = libqtic2module
LOCAL_VENDOR_MODULE := true

LOCAL_CPP_EXTENSION := .cc
LOCAL_CPPFLAGS += -fexceptions

LOCAL_SRC_FILES := \
    c2-module.cc

LOCAL_SHARED_LIBRARIES := \
    libcodec2 \
    libqcodec2_core \
    libcodec2_vndk

LOCAL_HEADER_LIBRARIES := libqcodec2_core_api_headers

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE := libqtic2headers

LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)

include $(BUILD_HEADER_LIBRARY)
