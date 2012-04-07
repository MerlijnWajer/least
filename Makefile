.PHONY: all clean
.DEFAULT: all

CFLAGS += -ansi -Werror -Wall -Wextra -ggdb
CFLAGS += -I mupdf/fitz -I mupdf/pdf #-Ixps -Icbz -Iscripts
CFLAGS += -D_BSD_SOURCE -D_XOPEN_SOURCE=600 -O2
LIBS += -lfreetype -ljbig2dec -ljpeg -lopenjpeg -lz -lm -lGL -lSDL -lGLU -pthread


all: least

least: least.c
	$(CC) least.c $(CFLAGS) $(LIBS) -o least mupdf/build/debug/libfitz.a

clean:
	rm least
