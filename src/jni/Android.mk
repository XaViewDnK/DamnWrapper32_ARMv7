LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := DamnWrapper32
LOCAL_SRC_FILES := main.cpp dylb.cpp sqlite3.c symbolizer.cpp
LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv2 -lz
LOCAL_STRIP_MODE := none
include $(BUILD_SHARED_LIBRARY)
