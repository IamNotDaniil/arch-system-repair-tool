CC = gcc
CFLAGS = -O2 -Wall -D_GNU_SOURCE
LDFLAGS = -lrt
TARGET = arch-repair
SOURCE = arch-repair.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)
	@echo "Build complete. Run with: sudo ./$(TARGET) --force"

clean:
	rm -f $(TARGET)

install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/
	sudo chmod 755 /usr/local/bin/$(TARGET)

.PHONY: all clean install
