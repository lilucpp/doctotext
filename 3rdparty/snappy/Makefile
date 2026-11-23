CXX ?= g++
AR ?= ar
CXXFLAGS ?= -O2 -std=c++14

SRCS = snappy.cc snappy-sinksource.cc snappy-stubs-internal.cc
OBJS = $(SRCS:.cc=.o)

all: libsnappy.a

%.o: %.cc snappy.h snappy-internal.h snappy-sinksource.h snappy-stubs-internal.h snappy-stubs-public.h
	$(CXX) $(CXXFLAGS) -I. -w -c $< -o $@

libsnappy.a: $(OBJS)
	$(AR) rcs $@ $^

clean:
	rm -f $(OBJS) libsnappy.a

.PHONY: all clean
