ifeq ($(filter-out exynos4 exynos5,$(TARGET_BOARD_PLATFORM)),)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
#lt, mr1@20121225
ifeq ($(BOARD_USE_EXYNOS_OMX), true)
LOCAL_SRC_FILES := \
    Exynos_OMX_Plugin.cpp
else
LOCAL_SRC_FILES := \
    SEC_OMX_Plugin.cpp
endif

LOCAL_CFLAGS += $(PV_CFLAGS_MINUS_VISIBILITY)

LOCAL_C_INCLUDES:= \
      $(TOP)/frameworks/native/include/media/openmax \
      $(TOP)/frameworks/native/include/media/hardware

LOCAL_SHARED_LIBRARIES :=    \
        libbinder            \
        libutils             \
        libcutils            \
        libui                \
        libdl               

LOCAL_MODULE := libstagefrighthw

LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)

endif

