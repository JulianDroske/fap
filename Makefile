
DEPS := lib/tinyradix.h lib/fatrack.h lib/pstrack.h lib/tinycli.h
DEPS += pm.h main/db.c

OBJS := lib/tinyradix.o lib/fatrack.o lib/pstrack.o lib/tinycli.o
OBJS += pm.o main.o

OUT ?= fap

CC ?= cc
CFLAGS += -std=c99
LIBS += -lsqlite3

CTH := tool/c2h.sh

all: $(OUT)

%.o: %.c $(DEPS) Makefile
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT): $(OBJS) Makefile
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

clean:
	@rm -f $(OBJS) $(OUT)

.PHONY: all clean

