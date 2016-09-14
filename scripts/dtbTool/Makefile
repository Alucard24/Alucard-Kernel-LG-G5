OBJ_FILES := dtbtool.o
CFLAGS := -static -O2 -fomit-frame-pointer -Wall

all: dtbTool

dtbTool: $(OBJ_FILES)
	$(CC) $(CFLAGS) -o $@ $^
	strip $@

clean:
	rm -f $(OBJ_FILES)

