APP_TITLE := AMEGA PS4 Receiver
APP_VERSION := 0.1.0
TITLE_ID := AMEG00001
CONTENT_ID := IV0000-AMEG00001_00-AMEGARECEIVER000

TOOLCHAIN ?= $(OO_PS4_TOOLCHAIN)
CXX := clang++
LD := ld.lld
PKGTOOL := $(TOOLCHAIN)/bin/linux/PkgTool.Core
CREATE_FSELF := $(TOOLCHAIN)/bin/linux/create-fself
CREATE_GP4 := $(TOOLCHAIN)/bin/linux/create-gp4

CXXFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 \
  -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
  -isystem $(TOOLCHAIN)/include/c++/v1
LIBS := -lc -lkernel -lc++ -lSceUserService -lSceVideoOut -lSceAudioOut \
  -lScePad -lSceSysmodule -lSDL2

.PHONY: all clean check-toolchain
all: check-toolchain AMEGA-PS4.pkg

check-toolchain:
	@test -n "$(TOOLCHAIN)" || (echo "OO_PS4_TOOLCHAIN is not set"; exit 1)
	@test -x "$(CREATE_FSELF)" || (echo "Missing create-fself in $(TOOLCHAIN)/bin/linux"; exit 1)
	@test -x "$(CREATE_GP4)" || (echo "Missing create-gp4 in $(TOOLCHAIN)/bin/linux"; exit 1)
	@test -x "$(PKGTOOL)" || (echo "Missing PkgTool.Core in $(TOOLCHAIN)/bin/linux"; exit 1)

build:
	mkdir -p build

build/amega.o: receiver/main.cpp receiver/font_ru.h | build
	$(CXX) $(CXXFLAGS) -c receiver/main.cpp -o $@

build/amega.elf: build/amega.o
	$(LD) -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
	  -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o $< -o $@

eboot.bin: build/amega.elf
	$(CREATE_FSELF) -in=build/amega.elf -out=build/amega.oelf --eboot "eboot.bin" --paid 0x3800000000000011

sce_sys/icon0.png: ../../tools/generate_icon.py
	mkdir -p sce_sys
	python3 ../../tools/generate_icon.py $@

sce_sys/param.sfo: Makefile
	mkdir -p sce_sys
	$(PKGTOOL) sfo_new $@
	$(PKGTOOL) sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(PKGTOOL) sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(APP_VERSION)'
	$(PKGTOOL) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(PKGTOOL) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(PKGTOOL) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(APP_TITLE)'
	$(PKGTOOL) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(PKGTOOL) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(APP_VERSION)'

AMEGA-PS4.pkg: eboot.bin sce_sys/param.sfo
	$(CREATE_GP4) -out pkg.gp4 --content-id=$(CONTENT_ID) --files "eboot.bin sce_sys/param.sfo sce_sys/icon0.png"
	$(PKGTOOL) pkg_build pkg.gp4 .
	cp $(CONTENT_ID).pkg $@

clean:
	rm -rf build eboot.bin pkg.gp4 AMEGA-PS4.pkg $(CONTENT_ID).pkg sce_sys/param.sfo sce_sys/icon0.png
