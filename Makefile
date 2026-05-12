CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lusb-1.0

all: ncs2_boot ncs2_infer ncs2_mkblob

ncs2_boot: src/ncs2_boot.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

ncs2_infer: src/ncs2_infer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

ncs2_mkblob: src/ncs2_mkblob.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ncs2_boot ncs2_infer ncs2_mkblob

install: ncs2_boot ncs2_infer
	install -m 755 ncs2_boot ncs2_infer /usr/local/bin/

.PHONY: all clean install
