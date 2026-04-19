PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
LDLIBS ?=

WARN_CFLAGS = -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith \
  -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings

BASE_CPPFLAGS = -D_POSIX_C_SOURCE=200809L -I$(CURDIR)/src
BASE_CFLAGS = -std=c99 $(WARN_CFLAGS)

ifeq ($(SANITIZE),1)
BASE_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

SERVER_SRCS = \
  src/main.c \
  src/nd_backend.c \
  src/nd_backend_folder.c \
  src/nd_backend_image.c \
  src/nd_bpb.c \
  src/nd_common.c \
  src/nd_fat.c \
  src/nd_fat_sync.c \
  src/nd_journal.c \
  src/nd_protocol.c \
  src/nd_server.c

SERVER_OBJS = $(SERVER_SRCS:.c=.o)

.PHONY: all clean elks-check install

all: cnetdrive

cnetdrive: $(SERVER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(SERVER_OBJS) $(LDLIBS)

elks-check:
	$(MAKE) clean
	$(MAKE) CPPFLAGS="$(CPPFLAGS) -DELKS" cnetdrive

install: cnetdrive
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 cnetdrive $(DESTDIR)$(BINDIR)/cnetdrive

clean:
	rm -f cnetdrive $(SERVER_OBJS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(BASE_CPPFLAGS) $(BASE_CFLAGS) $(CFLAGS) -c -o $@ $<
