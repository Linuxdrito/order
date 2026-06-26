CXX = g++
CXXFLAGS = -std=c++20 -O2 -march=native -mtune=native -fomit-frame-pointer -pipe -DNDEBUG -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections
LDFLAGS = -s -Wl,--gc-sections

all: order

order: main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f order

install: order
	cp order /usr/local/bin/order

uninstall:
	rm -f /usr/local/bin/order

.PHONY: all clean install uninstall
