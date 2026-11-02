LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_MODULE    := loadingmusic
LOCAL_SRC_FILES := main.cpp stb_vorbis.c mod/logger.cpp
LOCAL_CFLAGS += -O2 -DNDEBUG
LOCAL_CPPFLAGS += -std=c++17
LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_LDLIBS += -llog -lOpenSLES
include $(BUILD_SHARED_LIBRARY)
