CXX ?= g++
AR ?= ar
CXXFLAGS ?= -O2 -std=c++14

all: libpoppler_decrypt.a

poppler_decrypt.o: poppler_decrypt.cpp poppler_decrypt.h
	$(CXX) $(CXXFLAGS) -c poppler_decrypt.cpp -o poppler_decrypt.o

libpoppler_decrypt.a: poppler_decrypt.o
	$(AR) rcs $@ $^

clean:
	rm -f poppler_decrypt.o libpoppler_decrypt.a

.PHONY: all clean
