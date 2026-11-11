# Gentoo Power Control: optimzrd (root daemon), optimzr (CLI), optimzr-gui (GTK4)
CC ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -std=gnu17 -Wall -Wextra -Wno-format-truncation -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fPIE
LDFLAGS += -pie -Wl,-z,relro,-z,now
B = build

all: $(B)/optimzrd $(B)/optimzr $(B)/optimzr-gui

$(B):
	mkdir -p $(B)

$(B)/optimzrd: src/optimzrd.c src/profiles.h src/proto.h | $(B)
	$(CC) $(CFLAGS) -o $@ src/optimzrd.c $(LDFLAGS)

$(B)/optimzr: src/optimzr.c src/proto.c src/proto.h | $(B)
	$(CC) $(CFLAGS) -o $@ src/optimzr.c src/proto.c $(LDFLAGS)

$(B)/optimzr-gui: src/gui.c src/proto.c src/proto.h | $(B)
	$(CC) $(CFLAGS) $$(pkg-config --cflags gtk4) -o $@ src/gui.c src/proto.c $(LDFLAGS) $$(pkg-config --libs gtk4) -lm

clean:
	rm -f $(B)/optimzrd $(B)/optimzr $(B)/optimzr-gui

.PHONY: all clean
