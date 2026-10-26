# camwall-planes. install.sh compiles the same sources in one gcc call outside the
# checkout; this Makefile is for building by hand: make, make clean.

PKGS    = libdrm libavformat libavcodec libavutil
CFLAGS ?= -O2 -Wall
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lpthread
OBJS    = main.o stream.o display.o

camwall-planes: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): common.h stream.h display.h

clean:
	rm -f camwall-planes $(OBJS)

.PHONY: clean
