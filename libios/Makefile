include $(DEVKITARM)/base_rules
#---------------------------------------------------------------------------------
# Clear the implicit built in rules
#---------------------------------------------------------------------------------
.SUFFIXES:

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------
TARGET		:=	$(notdir $(CURDIR))
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data  
INCLUDES	:=	../include

#---------------------------------------------------------------------------------
# the prefix on the compiler executables
#---------------------------------------------------------------------------------
PREFIX		:= $(DEVKITARM)/bin/arm-none-eabi-
ifneq (,$(findstring NT,$(UNAME)))
	CC			:= $(PREFIX)gcc.exe
	CXX			:= $(PREFIX)g++.exe
	AR			:= $(PREFIX)ar.exe
	OBJCOPY		:= $(PREFIX)objcopy.exe
	LD			:= $(PREFIX)g++.exe
	AS			:= $(PREFIX)gcc.exe
endif
CC			?= $(PREFIX)gcc
CXX			?= $(PREFIX)g++
AR			?= $(PREFIX)ar
OBJCOPY		?= $(PREFIX)objcopy
LD			?= $(PREFIX)g++
AS			?= $(PREFIX)gcc

#---------------------------------------------------------------------------------
# linker script
#---------------------------------------------------------------------------------
ifeq ($(BUILDING),$(emptystring))

export ROOT	:= $(CURDIR)

all:
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@$(MAKE) -C $(BUILD) -f $(CURDIR)/Makefile BUILDING=all
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).a
else

TARGET := $(notdir $(ROOT))
#----------------------------------------------------
# MS Visual Studio Style Fix:
#----------------------------------------------------
STYLEFIX	= 2>&1 | sed -e 's/\([a-zA-Z\.]\+\):\([0-9]\+\):\([0-9]\+:\)\?\(.\+\)/\1(\2):\4/' -e 's/undefined/error: undefined/'

#---------------------------------------------------------------------------------
# automatically build a list of object files for our project
#---------------------------------------------------------------------------------
OUTPUT		:=  $(ROOT)/$(TARGET)
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(ROOT)/$(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(ROOT)/$(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(ROOT)/$(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(ROOT)/$(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(ROOT)/$(dir)/*.*)))

OFILES		:=	$(addsuffix _bin.o,$(BINFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)
					
DEPENDS		:= $(OFILES:.o=.d)

VPATH		=  $(foreach dir,$(SOURCES),$(ROOT)/$(dir))

#---------------------------------------------------------------------------------
# build a list of library paths
#---------------------------------------------------------------------------------
ARCH	=	-march=armv5te -mtune=arm926ej-s

CFLAGS	=	-g $(ARCH) $(INCLUDE) -fno-strict-aliasing -Wall -O2 -fomit-frame-pointer -ffast-math -fverbose-asm \
			-Wpointer-arith -Winline -Wundef -g -ffunction-sections -fdata-sections -fno-exceptions

CPPFLAGS	=	-g $(ARCH) $(INCLUDE) -fno-strict-aliasing -Wall -O2 -fomit-frame-pointer -ffast-math -fverbose-asm \
			-Wpointer-arith -Winline -Wundef -g -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fpermissive -Wfatal-error


AFLAGS	=	-g $(ARCH)

$(OUTPUT).a: $(OFILES)
	@echo linking $(notdir $@)
	@$(AR) rvs $@ $(OFILES) $(STYLEFIX)

-include $(DEPENDS)
	
define bin2o
	@echo  -e "\t.section .rodata\n\t.align 4\n\t.global $(*)\n\t.global $(*)_end\n$(*):\n\t.incbin \"$(subst /,\\\\\\\\,$(shell echo $< | sed 's=/==;s=/=:/='))\"\n$(*)_end:\n" > $@.s
	@$(CC)  $(ASFLAGS) $(AFLAGS) -c $@.s -o $@
	@rm -rf $@.s
endef

endif
