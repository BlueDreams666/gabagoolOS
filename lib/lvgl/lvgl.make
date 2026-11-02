# LVGL graphics library for the SloppyOS interface (MIT licence, see LICENCE.txt)
LVGLLIB_DIR := $(ROOTDIR)/lib/lvgl
LVGLLIB_SRC := $(shell find $(LVGLLIB_DIR)/src -name '*.c' -not -path '*/drivers/*' -not -path '*/libs/thorvg/*' | sort)
LVGLLIB_OBJ := $(call c2obj, $(LVGLLIB_SRC))
LVGLLIB := $(BUILDDIR)/lib/liblvgl.a
LVGL_CFLAGS := -DLV_CONF_INCLUDE_SIMPLE -I$(LVGLLIB_DIR) -I$(ROOTDIR)/apps/sloppy

INCLUDES += -I$(LVGLLIB_DIR) -I$(ROOTDIR)/apps/sloppy
DEFINES += -DLV_CONF_INCLUDE_SIMPLE
CORE_LIBS += $(LVGLLIB)

$(BUILDDIR)/lib/lvgl/%.o: $(LVGLLIB_DIR)/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(GCCOPTS) $(EXTRA_DEFINES) -w -O2 $(LVGL_CFLAGS) -c $< -o $@

$(LVGLLIB_OBJ): $(ROOTDIR)/apps/sloppy/lv_conf.h

$(LVGLLIB): $(LVGLLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null
