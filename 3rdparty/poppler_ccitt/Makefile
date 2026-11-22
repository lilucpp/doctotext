CXX ?= g++
AR ?= ar
CXXFLAGS ?= -O2 -std=c++14

all: libpoppler_ccitt.a

poppler_ccitt.o: poppler_ccitt.cpp poppler_ccitt.h Stream-CCITT.h
	$(CXX) $(CXXFLAGS) -c poppler_ccitt.cpp -o poppler_ccitt.o

libpoppler_ccitt.a: poppler_ccitt.o
	$(AR) rcs $@ $^

clean:
	rm -f poppler_ccitt.o libpoppler_ccitt.a

.PHONY: all clean
