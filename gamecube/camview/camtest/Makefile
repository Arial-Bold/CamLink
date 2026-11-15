#---------------------------------------------------------------------------------
# camtest - the GameCube side of the adapter test. Builds camtest.dol.
# Requires devkitPPC + libogc2 (or use the GitHub build).
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "DEVKITPPC is not set. Use the GitHub build")
endif

include $(DEVKITPRO)/libogc2/gamecube_rules

TARGET		:=	camtest
BUILD		:=	build
SOURCES		:=	.
INCLUDES	:=	. ../../shared

CFLAGS	=	-g -O2 -Wall -std=gnu11 $(MACHDEP) $(INCLUDE)
LDFLAGS	=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS	:=	-logc -lm
LIBDIRS	:=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) -I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC)
export LIBPATHS	:=	-L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

else

DEPENDS	:=	$(OFILES:.o=.d)
$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)

endif
