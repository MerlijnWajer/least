.PHONY: default all

CFLAGS += -ansi -Werror -Wall -Wextra -ggdb
CFLAGS += -I mupdf/fitz -I mupdf/pdf #-Ixps -Icbz -Iscripts
LIBS += -lfreetype -ljbig2dec -ljpeg -lopenjpeg -lz -lm -lGL -lSDL -lGLU -lSDL_image

default: all
	$(CC) least.c $(CFLAGS) $(LIBS) -o least mupdf/build/debug/libfitz.a

