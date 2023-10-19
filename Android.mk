ifeq ($(call is-board-platform-in-list,kona lahaina),true)

#Compiling only for codec2 enabled SPs
ifneq ($(TARGET_KERNEL_VERSION), 4.19)

IOT_CORE_OSS_PATH := $(call my-dir)

include $(IOT_CORE_OSS_PATH)/c2-module/Android.mk

endif
endif
