# function to find all *.cpp files under a directory
define all-cpp-files-under
$(patsubst ./%,%, \
  $(shell cd $(LOCAL_PATH) ; \
          find $(1) -name "*.cpp" -and -not -name ".*") \
 )
endef


LOCAL_PATH:= $(call my-dir)
NFA := src/nfa
NFC := src/nfc
HAL := src/hal
UDRV := src/udrv
HALIMPL := halimpl/bcm2079x
D_CFLAGS := -DANDROID -DBUILDCFG=1

ifeq ($(strip $(BOARD_HAVE_NXP_PN547)), true)
COMMON_CFLAGS += -DANDROID -DANDROID_USE_LOGCAT=TRUE

NFA_CFLAGS += -DNFC_CONTROLLER_ID=1
D_CFLAGS += -DDEBUG -D_DEBUG -O0 -g

#gki
GKI_FILES:=$(call all-c-files-under, src/gki)

#
# libnfc-nci
#
#nfa
NFA_ADAP_FILES:= \
    $(call all-c-files-under, src/adaptation) \
    $(call all-cpp-files-under, src/adaptation)

#nfa
NFA_FILES:= $(call all-c-files-under, $(NFA))
NFC_FILES:= $(call all-c-files-under, $(NFC))

D_CFLAGS += -DNXP_EXT
D_CFLAGS += -DNFA_APP_DOWNLOAD_NFC_PATCHRAM=TRUE

#NTAL includes
NTAL_CFLAGS += -I$(LOCAL_PATH)/src/include
NTAL_CFLAGS += -I$(LOCAL_PATH)/src/gki/ulinux
NTAL_CFLAGS += -I$(LOCAL_PATH)/src/gki/common

#NFA NFC includes
NFA_CFLAGS += -I$(LOCAL_PATH)/src/include
NFA_CFLAGS += -I$(LOCAL_PATH)/src/gki/ulinux
NFA_CFLAGS += -I$(LOCAL_PATH)/src/gki/common
NFA_CFLAGS += -I$(LOCAL_PATH)/$(NFA)/brcm
NFA_CFLAGS += -I$(LOCAL_PATH)/$(NFA)/include
NFA_CFLAGS += -I$(LOCAL_PATH)/$(NFA)/int
NFA_CFLAGS += -I$(LOCAL_PATH)/$(NFC)/brcm
NFA_CFLAGS += -I$(LOCAL_PATH)/$(NFC)/include
NFA_CFLAGS += -I$(LOCAL_PATH)/$(NFC)/int
NFA_CFLAGS += -I$(LOCAL_PATH)/$(UDRV)/include
NFA_CFLAGS += -I$(LOCAL_PATH)/src/hal/include
NFA_CFLAGS += -I$(LOCAL_PATH)/src/hal/int

ifneq ($(NCI_VERSION),)
NFA_CFLAGS += -DNCI_VERSION=$(NCI_VERSION)
endif

include $(CLEAR_VARS)
LOCAL_PRELINK_MODULE := false
LOCAL_ARM_MODE := arm
LOCAL_MODULE:= libnfc-nci
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libhardware_legacy libcutils libdl libstlport libhardware
LOCAL_CFLAGS := $(COMMON_CFLAGS) $(D_CFLAGS) $(NFA_CFLAGS)
LOCAL_C_INCLUDES := external/stlport/stlport bionic/ bionic/libstdc++/include
LOCAL_SRC_FILES := $(NFA_ADAP_FILES) $(GKI_FILES) $(NFA_FILES) $(NFC_FILES) $(LOG_FILES)
include $(BUILD_SHARED_LIBRARY)
endif # BOARD_HAVE_NXP_PN547

ifeq ($(BOARD_HAVE_BCM2079X),true)
######################################
# Build shared library system/lib/libnfc-nci.so for stack code.

include $(CLEAR_VARS)
LOCAL_PRELINK_MODULE := false
LOCAL_ARM_MODE := arm
LOCAL_MODULE := libnfc-nci
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libhardware_legacy libcutils liblog libdl libstlport libhardware
LOCAL_CFLAGS := $(D_CFLAGS)
LOCAL_C_INCLUDES := external/stlport/stlport bionic/ bionic/libstdc++/include \
    $(LOCAL_PATH)/src/include \
    $(LOCAL_PATH)/src/gki/ulinux \
    $(LOCAL_PATH)/src/gki/common \
    $(LOCAL_PATH)/$(NFA)/include \
    $(LOCAL_PATH)/$(NFA)/int \
    $(LOCAL_PATH)/$(NFC)/include \
    $(LOCAL_PATH)/$(NFC)/int \
    $(LOCAL_PATH)/src/hal/include \
    $(LOCAL_PATH)/src/hal/int
LOCAL_SRC_FILES := \
    $(call all-c-files-under, $(NFA)/ce $(NFA)/dm $(NFA)/ee) \
    $(call all-c-files-under, $(NFA)/hci $(NFA)/int $(NFA)/p2p $(NFA)/rw $(NFA)/sys) \
    $(call all-c-files-under, $(NFC)/int $(NFC)/llcp $(NFC)/nci $(NFC)/ndef $(NFC)/nfc $(NFC)/tags) \
    $(call all-c-files-under, src/adaptation) \
    $(call all-cpp-files-under, src/adaptation) \
    $(call all-c-files-under, src/gki) \
    src/nfca_version.c
include $(BUILD_SHARED_LIBRARY)

######################################
# Build shared library system/lib/hw/nfc_nci.*.so for Hardware Abstraction Layer.
# Android's generic HAL (libhardware.so) dynamically loads this shared library.

include $(CLEAR_VARS)
LOCAL_MODULE := nfc_nci.$(TARGET_DEVICE)
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SRC_FILES := $(call all-c-files-under, $(HALIMPL)) \
    $(call all-cpp-files-under, $(HALIMPL))
LOCAL_SHARED_LIBRARIES := liblog libcutils libhardware_legacy libstlport
LOCAL_MODULE_TAGS := optional
LOCAL_C_INCLUDES := external/stlport/stlport bionic/ bionic/libstdc++/include \
    $(LOCAL_PATH)/$(HALIMPL)/include \
    $(LOCAL_PATH)/$(HALIMPL)/gki/ulinux \
    $(LOCAL_PATH)/$(HALIMPL)/gki/common \
    $(LOCAL_PATH)/$(HAL)/include \
    $(LOCAL_PATH)/$(HAL)/int \
    $(LOCAL_PATH)/src/include \
    $(LOCAL_PATH)/$(NFC)/include \
    $(LOCAL_PATH)/$(NFA)/include \
    $(LOCAL_PATH)/$(UDRV)/include
LOCAL_CFLAGS := $(D_CFLAGS) -DNFC_HAL_TARGET=TRUE -DNFC_RW_ONLY=TRUE
LOCAL_CPPFLAGS := $(LOCAL_CFLAGS)
include $(BUILD_SHARED_LIBRARY)
endif #BOARD_HAVE_BCM2079X

######################################
include $(call all-makefiles-under,$(LOCAL_PATH))
