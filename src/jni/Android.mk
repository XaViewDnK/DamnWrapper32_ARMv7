LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := DamnWrapper32
LOCAL_SRC_FILES := main.cpp dylb.cpp sqlite3.c symbolizer.cpp ipavfs.cpp
LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv2 -lz
# В ABI iOS ARM32 r9 — регистр-мусорка, гостевой код затирает его без сохранения, тогда как
# AAPCS считает r9 сохраняемым. Любое наше значение, пережившее в r9 вызов в игру, портилось.
LOCAL_CFLAGS += -ffixed-r9
LOCAL_STRIP_MODE := none
include $(BUILD_SHARED_LIBRARY)
