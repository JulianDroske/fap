DEPS := tinyradix.c fatrack.c pstrack.c tinycli.c main.c db.c pm.c
OBJS := main.o
OUT ?= fap

LIBS += -lsqlite3

all: $(OUT)

%.o: %.c $(DEPS) Makefile
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT): $(OBJS) Makefile
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)
