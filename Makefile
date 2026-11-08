# wsl2-ssh-agent: C daemon (Linux) + native helper (Windows).
#
#   make                 build ./wsl2-ssh-agent with the helper embedded
#   make install         install to $(DESTDIR)$(PREFIX)/bin
#
# The Windows helper is built with one of (first found):
#   WINCC=x86_64-w64-mingw32-gcc   (Arch: pacman -S mingw-w64-gcc)
#   MSVC on the Windows host, driven through WSL interop (windows/msvc.sh)
# or point HELPER_EXE at a prebuilt wsl2-ssh-agent-helper.exe.

PREFIX  ?= /usr/local
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CC      ?= cc
CFLAGS  ?= -Os -pipe
WARN     = -Wall -Wextra -Wno-unused-parameter
HARDEN   = -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE
LDFLAGS ?= -pie -Wl,-z,relro,-z,now -Wl,--gc-sections -Wl,--as-needed -s
SIZE     = -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables

WINCC   ?= $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null)
HELPER_EXE ?= build/wsl2-ssh-agent-helper.exe

all: wsl2-ssh-agent

build:
	mkdir -p build

ifeq ($(origin HELPER_EXE),file)
$(HELPER_EXE): windows/helper.c | build
ifneq ($(WINCC),)
	$(WINCC) -Os -s -nostdlib -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables \
	  -mno-stack-arg-probe -Wl,-e,start -Wl,--subsystem,console -Wl,--gc-sections \
	  -o $@ $< -lkernel32
else
	windows/msvc.sh $< $@
endif
endif

wsl2-ssh-agent: src/wsl2-ssh-agent.c $(HELPER_EXE)
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(SIZE) -DVERSION='"$(VERSION)"' \
	  -DHELPER_EXE='"$(abspath $(HELPER_EXE))"' -o $@ $< $(LDFLAGS)

build/agentbench: bench/agentbench.c | build
	$(CC) -O2 -pthread -o $@ $<

install: wsl2-ssh-agent
	install -Dm755 wsl2-ssh-agent $(DESTDIR)$(PREFIX)/bin/wsl2-ssh-agent
	install -Dm644 extras/systemd/user/wsl2-ssh-agent.service \
	  $(DESTDIR)$(PREFIX)/lib/systemd/user/wsl2-ssh-agent.service

clean-linux:
	rm -f wsl2-ssh-agent

clean: clean-linux
	rm -rf build

.PHONY: all install clean clean-linux
