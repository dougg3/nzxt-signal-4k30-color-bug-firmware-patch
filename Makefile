# Cross-compile mcu_flash.exe for 64-bit Windows (the SDK DLL is x86-64).
CC      = x86_64-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -Wextra -municode
LDFLAGS = -static
LDLIBS  = -lbcrypt

TARGET  = mcu_flash.exe

all: $(TARGET)

$(TARGET): mcu_flash.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
