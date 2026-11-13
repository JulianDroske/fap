
DEPS := lib/tinyradix.h lib/fatrack.h lib/pstrack.h lib/tinycli.h
DEPS += db.h pm.h

OBJS := lib/tinyradix.o lib/fatrack.o lib/pstrack.o lib/tinycli.o
OBJS += db.o pm.o main.o

OUT ?= fap

CC ?= cc
CFLAGS += -std=c99
LIBS += -lsqlite3

CTH := tool/c2h.sh

all: $(OUT)

db.h: db.c $(CTH) Makefile
	$(CTH) db.c db.h _DB_H_ stdint.h sqlite3.h

%.o: %.c $(DEPS) Makefile
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT): $(OBJS) Makefile
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

clean:
	@rm -f $(OBJS) $(OUT)

.PHONY: all clean

