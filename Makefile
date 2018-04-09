.PHONY: all clean
default: all

CFLAGS += -ansi -Werror -Wall -Wextra
#CFLAGS += -I mupdf/fitz -I mupdf/pdf #-Ixps -Icbz -Iscripts
CFLAGS += -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600
LIBS += -lfreetype -ljbig2dec -ljpeg -lopenjp2 -lz -lm -lGL -lSDL

all: debug

debug: CFLAGS += -ggdb
debug: least

release: CFLAGS += -O2
release: least

LEAST_OS=least.o

least: $(LEAST_OS)
	$(CC) least.c $(CFLAGS) $(LIBS) -o least -lmupdf

clean:
	rm least
