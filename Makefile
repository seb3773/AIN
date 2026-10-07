CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Isrc
BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/ain
PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all clean update-linux-stub update-dos-stub install uninstall deb test help debug

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): src/ain.c src/ain_sfx_stub.h src/ain_sfx_linux_stub.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ src/ain.c

debug: CFLAGS = -g -O0 -Wall -Wextra -Isrc
debug: clean $(TARGET)

# Dev target: rebuild embedded Linux SFX stub header from src/ain_sfx_linux_stub.c
update-linux-stub: src/ain_sfx_linux_stub.c $(TARGET)
	$(CC) -Os -Wall -Wextra -s -ffunction-sections -fdata-sections -Wl,--gc-sections -o .tmp_lin_stub src/ain_sfx_linux_stub.c
	./$(TARGET) a -m1 .tmp_lin_stub.ain .tmp_lin_stub
	python3 -c "import struct; d=open('.tmp_lin_stub.ain','rb').read(); ip=struct.unpack('<I',d[14:18])[0]; st=d[24:ip]; sz=len(open('.tmp_lin_stub','rb').read()); open('src/ain_sfx_linux_stub.h','w').write('/* Auto-generated: AIN M1 compressed Linux SFX stub */\n#ifndef AIN_SFX_LINUX_STUB_H\n#define AIN_SFX_LINUX_STUB_H\n#include <stdint.h>\n#include <stddef.h>\n#define AIN_COMPRESSED_LINUX_SFX_STUB_SIZE %du\n#define AIN_ORIGINAL_LINUX_SFX_STUB_SIZE %du\nstatic const uint8_t ain_compressed_linux_sfx_stub[%d] = {' % (len(st), sz, len(st)) + ','.join('0x%02x' % b for b in st) + '};\n#endif\n')"
	rm -f .tmp_lin_stub .tmp_lin_stub.ain

# Dev target: rebuild embedded DOS SFX stub header from historical_dos/AINEXT.EXE
update-dos-stub: historical_dos/AINEXT.EXE $(TARGET)
	./$(TARGET) a -m1 .tmp_dos_stub.ain historical_dos/AINEXT.EXE
	python3 -c "import struct; d=open('.tmp_dos_stub.ain','rb').read(); ip=struct.unpack('<I',d[14:18])[0]; st=d[24:ip]; sz=len(open('historical_dos/AINEXT.EXE','rb').read()); open('src/ain_sfx_stub.h','w').write('/* Auto-generated: AIN M1 compressed DOS SFX stub */\n#ifndef AIN_SFX_STUB_H\n#define AIN_SFX_STUB_H\n#include <stdint.h>\n#include <stddef.h>\n#define AIN_COMPRESSED_SFX_STUB_SIZE %du\n#define AIN_ORIGINAL_SFX_STUB_SIZE %du\nstatic const uint8_t ain_compressed_sfx_stub[%d] = {' % (len(st), sz, len(st)) + ','.join('0x%02x' % b for b in st) + '};\n#endif\n')"
	rm -f .tmp_dos_stub.ain

test: $(TARGET)
	@echo "Running tests against reference corpus (AIN 2.32)..."
	./$(TARGET) l packed_corpus/CORPACK.AIN
	./$(TARGET) t packed_corpus/CORPACK.AIN
	./$(TARGET) l packed_corpus/NESTED.AIN
	./$(TARGET) t packed_corpus/NESTED.AIN
	@echo "Running backward compatibility tests (AIN 2.22 & 2.2)..."
	./tests/verify_ain222.sh
	./tests/verify_ain22.sh
	@echo "All tests passed successfully."

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/ain

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/ain

deb:
	@chmod +x build_deb.sh
	./build_deb.sh

clean:
	rm -rf $(BUILD_DIR) *.o .tmp_* *.deb

help:
	@echo "AIN 2.32 Archiver build targets:"
	@echo "  make            - Build production binary (build/ain)"
	@echo "  make debug      - Build debug binary with symbols"
	@echo "  make test       - Run verification against reference archives"
	@echo "  make deb        - Build standalone Debian .deb package"
	@echo "  make install    - Install binary to PREFIX (default: /usr/local)"
	@echo "  make clean      - Clean build artifacts"
