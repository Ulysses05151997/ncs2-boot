CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lusb-1.0

TARGET  = ncs2_boot
SRC     = src/ncs2_boot.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean install
