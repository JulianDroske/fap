DEPS := tinyradix.h fatrack.h pstrack.h tinycli.h db.c pm.c
OBJS := tinyradix.o fatrack.o pstrack.o tinycli.o main.o
OUT ?= fap

LIBS += -lsqlite3

all: $(OUT)

%.o: %.c $(DEPS) Makefile
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT): $(OBJS) Makefile
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)
