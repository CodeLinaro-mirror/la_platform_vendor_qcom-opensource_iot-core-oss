ifeq ($(call is-board-platform-in-list,kona lahaina bengal),true)

#Compiling only for codec2 enabled SPs
ifneq ($(TARGET_KERNEL_VERSION), 4.19)

PRODUCT_PACKAGES += libqtic2module

endif
endif
