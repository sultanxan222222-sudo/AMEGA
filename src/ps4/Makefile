APP_TITLE := AMEGA PS4 Receiver
APP_VERSION := 0.2.2
TITLE_ID := AMEG00001
CONTENT_ID := IV0000-AMEG00001_00-AMEGARECEIVER000

TOOLCHAIN ?= $(OO_PS4_TOOLCHAIN)
CXX := clang++
LD := ld.lld
PKGTOOL := $(TOOLCHAIN)/bin/linux/PkgTool.Core
CREATE_EBOOT := $(TOOLCHAIN)/bin/linux/create-eboot
SDK_DATA := $(TOOLCHAIN)/bin/data

CXXFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 \
  -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
  -isystem $(TOOLCHAIN)/include/c++/v1
LIBS := -lc -lkernel -lc++ -lSceUserService -lSceVideoOut -lSceAudioOut \
  -lScePad -lSceSysmodule -lSceFreeType -lSDL2

.PHONY: all clean check-toolchain
all: check-toolchain AMEGA-PS4.pkg

check-toolchain:
	@test -n "$(TOOLCHAIN)" || (echo "OO_PS4_TOOLCHAIN is not set"; exit 1)
	@test -x "$(CREATE_EBOOT)" || (echo "Missing create-eboot in $(TOOLCHAIN)/bin/linux"; exit 1)
	@test -x "$(PKGTOOL)" || (echo "Missing PkgTool.Core in $(TOOLCHAIN)/bin/linux"; exit 1)
	@test -f "$(SDK_DATA)/template.gp4" || (echo "Missing GP4 template in $(SDK_DATA)"; exit 1)
	@test -f "$(SDK_DATA)/modules/libc.prx" || (echo "Missing default libc.prx in SDK data"; exit 1)

build:
	mkdir -p build

build/amega.o: receiver/main.cpp receiver/font_ru.h | build
	$(CXX) $(CXXFLAGS) -c receiver/main.cpp -o $@

build/amega.elf: build/amega.o
	$(LD) -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
	  -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o $< -o $@

eboot.bin: build/amega.elf
	$(CREATE_EBOOT) -in=build/amega.elf -out=build/amega.oelf --paid=0x3800000000000011
	@test -f eboot.bin || (echo "create-eboot did not produce eboot.bin"; exit 1)

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

sce_module/libc.prx: | sce_module
	cp $(SDK_DATA)/modules/libc.prx $@

sce_module/libSceFios2.prx: | sce_module
	cp $(SDK_DATA)/modules/libSceFios2.prx $@

sce_module:
	mkdir -p $@

sce_sys/about/right.sprx:
	mkdir -p sce_sys/about
	cp $(SDK_DATA)/right.sprx $@

pkg.gp4:
	@test -f "$@" || (echo "Missing generated GP4 manifest: run tools/create_gp4.py before building"; exit 1)

AMEGA-PS4.pkg: pkg.gp4 eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx sce_sys/about/right.sprx
	$(PKGTOOL) pkg_build "$<" .
	cp "$(CONTENT_ID).pkg" "$@"

clean:
	rm -rf build eboot.bin pkg.gp4 AMEGA-PS4.pkg "$(CONTENT_ID).pkg" sce_sys/param.sfo sce_sys/about sce_module
