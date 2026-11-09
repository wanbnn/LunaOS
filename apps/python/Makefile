# Build the MicroPython core and LunaOS port as one freestanding archive.
ROOT := ../..
BUILD := $(ROOT)/build/micropython
MPY := $(ROOT)/third_party/micropython
include $(MPY)/py/mkenv.mk

export MICROPY_GIT_TAG := v1.29.0
MICROPY_ROM_TEXT_COMPRESSION := 0
include $(TOP)/py/py.mk

CFLAGS += $(LUNA_FLAGS) -I. -I$(TOP) -I$(BUILD) -fwrapv -DNDEBUG
SRC_C := port.c sdk.c shared/runtime/gchelper_generic.c
SRC_QSTR += port.c sdk.c
QSTR_GLOBAL_DEPENDENCIES += mpconfigport.h
OBJ := $(PY_CORE_O) $(addprefix $(BUILD)/,$(SRC_C:.c=.o))

all: $(BUILD)/libmicropython.a


$(OBJ): Makefile $(ROOT)/Makefile
include $(TOP)/py/mkrules.mk
