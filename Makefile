TARGET          := jmdict_vita
TITLE_ID        := JMDV00001
APP_NAME        := JMdict Vita
APP_VERSION     := 01.00
VITASDK         ?= /home/shoui/vitasdk
PREFIX          := $(VITASDK)/bin/arm-vita-eabi
CXX             := $(PREFIX)-g++
OBJCOPY         := $(PREFIX)-objcopy
VITA_ELF_CREATE := $(VITASDK)/bin/vita-elf-create
VITA_MAKE_FSELF := $(VITASDK)/bin/vita-make-fself
VITA_MKSFOEX    := $(VITASDK)/bin/vita-mksfoex
VITA_PACK_VPK   := $(VITASDK)/bin/vita-pack-vpk
PSP2CXML        ?= psp2cxml-tool
CURL            ?= curl

BUILD_DIR       := build
SOURCE_DIR      := $(BUILD_DIR)/sources
JMDICT_SOURCE   := $(SOURCE_DIR)/JMdict_e_examp.gz
JMNEDICT_SOURCE := $(SOURCE_DIR)/JMnedict.xml.gz
DICT_BIN        := $(BUILD_DIR)/jmdict_vita.bin
RCO             := $(BUILD_DIR)/jmdict_vita.rco
ICON            := assets/sce_sys/icon0.png
ELF             := $(BUILD_DIR)/$(TARGET).elf
STRIPPED_ELF    := $(BUILD_DIR)/$(TARGET).stripped.elf
VELF            := $(BUILD_DIR)/$(TARGET).velf
EBOOT           := $(BUILD_DIR)/eboot.bin
PARAM_SFO       := $(BUILD_DIR)/param.sfo
VPK             := $(BUILD_DIR)/$(TARGET).vpk
SOURCES         := src/main.cpp src/dictionary.cpp src/zlib_paf_shim.cpp
OBJECTS         := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

CPPFLAGS := -Iinclude -D__VITA__ -D__declspec\(x\)=
CXXFLAGS := -std=gnu++11 -Os -g -Wall -Wextra -Wno-unused-parameter \
            -fno-rtti -fno-exceptions -fno-builtin -fshort-wchar \
            -ffunction-sections -fdata-sections
LDFLAGS  := -Wl,-q,-z,nocopyreloc -nostartfiles -nostdlib
LIBS     := -lScePaf_stub_weak -lSceAppMgr_stub -lSceKernelThreadMgr_stub \
            -lSceSysmodule_stub -lSceIofilemgr_stub -lSceLibKernel_stub \
            -lSceProcessmgr_stub -lz -lgcc

.PHONY: all app dictionary rco vpk test clean

all: app

app: $(EBOOT) $(PARAM_SFO) $(RCO)

dictionary: $(DICT_BIN)

rco: $(RCO)

vpk: $(VPK)

$(BUILD_DIR):
	mkdir -p $@

$(SOURCE_DIR): | $(BUILD_DIR)
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(ELF): $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LIBS) -o $@

$(STRIPPED_ELF): $(ELF)
	$(OBJCOPY) --strip-debug $< $@

$(VELF): $(STRIPPED_ELF) exports.yml
	$(VITA_ELF_CREATE) -e exports.yml $< $@

# Requested system-application FSELF: attribute 0x0E, 0x10000 KiB ceiling.
$(EBOOT): $(VELF)
	$(VITA_MAKE_FSELF) -at 0x0E -m 0x10000 $< $@

$(PARAM_SFO): | $(BUILD_DIR)
	$(VITA_MKSFOEX) -s TITLE_ID=$(TITLE_ID) -s APP_VER=$(APP_VERSION) "$(APP_NAME)" $@

$(RCO): ui/jmdict_vita.xml | $(BUILD_DIR)
	$(PSP2CXML) $<
	test -f ui/jmdict_vita.rco
	mv ui/jmdict_vita.rco $@

$(JMDICT_SOURCE): | $(SOURCE_DIR)
	$(CURL) --fail --location --retry 3 \
	  --output $@.tmp https://www.edrdg.org/pub/Nihongo/JMdict_e_examp.gz
	gzip -t $@.tmp
	mv $@.tmp $@

$(JMNEDICT_SOURCE): | $(SOURCE_DIR)
	$(CURL) --fail --location --retry 3 \
	  --output $@.tmp https://www.edrdg.org/pub/Nihongo/JMnedict.xml.gz
	gzip -t $@.tmp
	mv $@.tmp $@

$(DICT_BIN): tools/convert_edrdg.py \
             $(JMDICT_SOURCE) $(JMNEDICT_SOURCE) | $(BUILD_DIR)
	python3 tools/convert_edrdg.py \
	  --jmdict $(JMDICT_SOURCE) \
	  --jmnedict $(JMNEDICT_SOURCE) \
	  --output $@

$(VPK): $(EBOOT) $(PARAM_SFO) $(RCO) $(DICT_BIN) $(ICON) licenses/EDRDG.txt
	$(VITA_PACK_VPK) -s $(PARAM_SFO) -b $(EBOOT) \
	  -a $(ICON)=sce_sys/icon0.png \
	  -a $(RCO)=jmdict_vita.rco \
	  -a $(DICT_BIN)=dictionary/jmdict_vita.bin \
	  -a licenses/EDRDG.txt=licenses/EDRDG.txt \
	  $@

test:
	python3 -m unittest discover -s tests -v

clean:
	rm -rf $(BUILD_DIR) ui/jmdict_vita.rco
