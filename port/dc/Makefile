# Compilation Dreamcast (KOS). À lancer depuis WSL après "source environ.sh",
# ou par le script build_dc.sh à la racine du dépôt.
SCUMMVM ?= ../../scummvm
ROOT := ../..
TARGET := bd.elf

ENGINE_SRC := $(filter-out %/sound.cpp %/vabsound.cpp %/midimusicplayer.cpp %/strplayer.cpp, \
	$(wildcard $(ROOT)/engine/dragons/*.cpp))
SVM_LIST := common/str.cpp common/ustr.cpp common/str-base.cpp common/stream.cpp \
	common/util.cpp common/memorypool.cpp common/hashmap.cpp common/path.cpp common/language.cpp \
	common/platform.cpp common/error.cpp common/str-enc.cpp common/punycode.cpp common/memory.cpp \
	common/encodings/singlebyte.cpp \
	graphics/surface.cpp graphics/pixelformat.cpp graphics/primitives.cpp graphics/palette.cpp \
	graphics/transform_tools.cpp graphics/blit/blit.cpp graphics/blit/blit-alpha.cpp \
	graphics/blit/blit-fast.cpp graphics/blit/blit-generic.cpp graphics/blit/blit-scale.cpp
PORT_SRC := $(ROOT)/port/common/engine_impl.cpp $(ROOT)/port/common/strplayer_port.cpp $(ROOT)/port/common/strdec.cpp $(ROOT)/port/common/subtitles.cpp $(ROOT)/port/common/inputmap.cpp $(ROOT)/port/common/padart.cpp $(ROOT)/port/snd/snd.cpp $(ROOT)/port/snd/sound_manager.cpp $(ROOT)/port/common/gpu_core.cpp $(ROOT)/port/common/hd_pack.cpp $(ROOT)/port/common/inflate.cpp $(ROOT)/port/common/disc.cpp $(ROOT)/port/common/prefetch.cpp main_dc.cpp audio_dc.cpp

OBJS := $(patsubst %.cpp,obj/%.o,$(notdir $(ENGINE_SRC) $(PORT_SRC))) \
	$(patsubst %.cpp,obj/svm/%.o,$(SVM_LIST))

KOS_CPPFLAGS += -DHAVE_CONFIG_H -I$(ROOT)/port/common -I$(ROOT)/port/snd -I$(ROOT)/port/compat -I$(ROOT)/engine -I$(SCUMMVM)
ifdef GPU
KOS_CPPFLAGS += -DBD_GPU
endif
ifdef LOG
KOS_CPPFLAGS += -DBD_LOG
endif
ifdef NOLOG
KOS_CPPFLAGS += -DBD_NOLOG
endif
ifdef HW
KOS_CPPFLAGS += -DBD_HWAUDIO
endif
BD_BUILD ?= inconnue
KOS_CPPFLAGS += -DBD_BUILD=\"$(BD_BUILD)\"
KOS_CFLAGS += -O2 -w -MMD -MP

vpath %.cpp $(ROOT)/engine/dragons $(ROOT)/port/common $(ROOT)/port/snd .

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

$(TARGET): $(OBJS)
	kos-c++ -o $@ $(OBJS)

obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(KOS_CFLAGS) $(KOS_CPPFLAGS) -std=gnu++17 -c $< -o $@

obj/svm/%.o: $(SCUMMVM)/%.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(KOS_CFLAGS) $(KOS_CPPFLAGS) -std=gnu++17 -c $< -o $@

clean:
	rm -rf obj $(TARGET)

-include $(OBJS:.o=.d)
