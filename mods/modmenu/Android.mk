LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_MODULE    := modmenu
LOCAL_SRC_FILES := modmenu.cpp modmenu_core.cpp mod/logger.cpp mod/config.cpp
LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17
LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_LDLIBS += -llog
include $(BUILD_SHARED_LIBRARY)
