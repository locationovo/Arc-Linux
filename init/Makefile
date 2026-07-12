CC = gcc
CFLAGS = -static -Os -s -Wall -Wextra
TARGET = init
SRC = init.c
OBJ = init.o

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJ)

install: $(TARGET)
	install -D $(TARGET) $(DESTDIR)/init
