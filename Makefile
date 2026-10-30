CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
CPPFLAGS += -D_GNU_SOURCE
LDLIBS  += -lpthread -lm

# Prefer system dev packages (libcurl4-openssl-dev, libncurses-dev).
# Without them, `make deps` unpacks their headers into third_party/ (no root
# needed) and we link straight against the installed runtime libraries.
ifeq ($(shell pkg-config --exists libcurl ncursesw 2>/dev/null && echo yes),yes)
  CPPFLAGS += $(shell pkg-config --cflags libcurl ncursesw)
  LDLIBS   += $(shell pkg-config --libs libcurl ncursesw)
else
  MULTIARCH := $(shell $(CC) -print-multiarch)
  LOCAL    := third_party/root/usr
  CPPFLAGS += -I$(LOCAL)/include -I$(LOCAL)/include/$(MULTIARCH)
  LDLIBS   += -l:libcurl.so.4 -l:libncursesw.so.6 -l:libtinfo.so.6
endif

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)

hft-sim: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c src/hft.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

deps:
	mkdir -p third_party/debs
	cd third_party/debs && apt-get download libcurl4-openssl-dev libncurses-dev
	for d in third_party/debs/*.deb; do dpkg-deb -x $$d third_party/root; done

clean:
	rm -rf build hft-sim

.PHONY: deps clean
