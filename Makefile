.PHONY: default all

CFLAGS += -I mupdf/fitz -I mupdf/pdf #-Ixps -Icbz -Iscripts
LIBS += -lfreetype -ljbig2dec -ljpeg -lopenjpeg -lz -lm

default: all
	$(CC) least.c $(CFLAGS) $(LIBS) -o least mupdf/build/debug/libfitz.a

