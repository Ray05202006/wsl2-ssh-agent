# wsl2-ssh-agent: C daemon (Linux) + native helper (Windows).
#
#   make                 build ./wsl2-ssh-agent with the helper embedded
#   make test            functional tests (needs WSL interop + Windows ssh-agent)
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

test: wsl2-ssh-agent build/agentbench
	tests/run.sh ./wsl2-ssh-agent

install: wsl2-ssh-agent
	install -Dm755 wsl2-ssh-agent $(DESTDIR)$(PREFIX)/bin/wsl2-ssh-agent
	install -Dm644 extras/systemd/user/wsl2-ssh-agent.service \
	  $(DESTDIR)$(PREFIX)/lib/systemd/user/wsl2-ssh-agent.service

test-daemon: wsl2-ssh-agent
	tests/daemon.sh ./wsl2-ssh-agent

# memory-safety checks: ASan+UBSan daemon under the daemon test suite (plus the
# end-to-end suite when run in WSL2), randomized framing tests of the helper,
# and gcc's static analyzer on the daemon.
SANFLAGS = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

build/san/wsl2-ssh-agent: src/wsl2-ssh-agent.c $(HELPER_EXE)
	mkdir -p build/san
	$(CC) $(SANFLAGS) $(WARN) -DHELPER_EXE='"$(abspath $(HELPER_EXE))"' -o $@ $<

sanitize: build/san/wsl2-ssh-agent
	ASAN_OPTIONS=exitcode=99 tests/daemon.sh build/san/wsl2-ssh-agent
	@if [ -n "$$WSL_DISTRO_NAME" ]; then tests/run.sh build/san/wsl2-ssh-agent && PRESPAWN=1 tests/run.sh build/san/wsl2-ssh-agent; fi

fuzz:
	tests/fuzz/run.sh $(or $(N),20000)

valgrind: $(HELPER_EXE)
	HELPER_EXE=$(HELPER_EXE) tests/valgrind.sh $(or $(N),300)

analyze:
	$(CC) -fanalyzer -O1 $(WARN) -Werror -DHELPER_EXE='"$(abspath $(HELPER_EXE))"' -c -o /dev/null src/wsl2-ssh-agent.c

clean-linux:
	rm -f wsl2-ssh-agent

clean: clean-linux
	rm -rf build

.PHONY: all test test-daemon sanitize fuzz valgrind analyze install clean clean-linux
