CC ?= gcc
AR ?= ar
CFLAGS ?= -O2

SRCS = jbig2.c jbig2_arith.c jbig2_arith_int.c jbig2_arith_iaid.c \
       jbig2_huffman.c jbig2_segment.c jbig2_page.c \
       jbig2_symbol_dict.c jbig2_text.c jbig2_generic.c \
       jbig2_refinement.c jbig2_mmr.c jbig2_halftone.c \
       jbig2_image.c jbig2_metadata.c
OBJS = $(SRCS:.c=.o)

all: libjbig2dec.a

%.o: %.c
	$(CC) $(CFLAGS) -DHAVE_STDINT_H -w -c $< -o $@

libjbig2dec.a: $(OBJS)
	$(AR) rcs $@ $^

clean:
	rm -f $(OBJS) libjbig2dec.a

.PHONY: all clean
