#
# A simple Makefile
#

CFLAGS = -Wall -O2 -g

TARGETS = QemuHDADump ExtractHDADump frameDumpFormatted

default: $(TARGETS)

QemuHDADump: QemuHDADump.c
	$(CC) $(CFLAGS) -o $@ $<

ExtractHDADump: ExtractHDADump.c
	$(CC) $(CFLAGS) -o $@ $<

frameDumpFormatted: frameDumpFormatted.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	find . \( -name \*~ -o -name \*.o \) -delete
	rm -f $(TARGETS)

realclean: clean

# EOF
