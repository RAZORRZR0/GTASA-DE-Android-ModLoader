LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_MODULE    := skygfx
LOCAL_SRC_FILES := core.cpp probes.cpp timecyc.cpp tools.cpp sky.cpp lights.cpp coronas.cpp postfx.cpp colour.cpp speedfx.cpp skycolours.cpp skyfog.cpp mod/logger.cpp mod/config.cpp
LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17
LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_LDLIBS += -llog
include $(BUILD_SHARED_LIBRARY)
