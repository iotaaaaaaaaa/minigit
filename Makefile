CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDLIBS   := -lz

LIB_SRC := src/util.cpp src/repo.cpp src/diff.cpp
HEADERS := $(wildcard src/*.h)

.PHONY: all test clean

all: bin/minigit

bin/minigit: $(LIB_SRC) src/main.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(LIB_SRC) src/main.cpp -o $@ $(LDLIBS)

bin/unit_tests: $(LIB_SRC) tests/unit.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -Isrc $(LIB_SRC) tests/unit.cpp -o $@ $(LDLIBS)

# Unit tests, then interoperability tests against the real git binary.
test: bin/minigit bin/unit_tests
	./bin/unit_tests
	./tests/interop.sh

clean:
	rm -rf bin
