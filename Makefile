P=watch-tags
OBJECTS=$(P).o
CFLAGS = `pkg-config --cflags ramutils` -g -Wall -std=c11 -pedantic -O3
LDLIBS = `pkg-config --libs ramutils` -pthread

$(P): $(OBJECTS)

clean:
	rm -f $(OBJECTS) $(P)

.PHONY: clean
