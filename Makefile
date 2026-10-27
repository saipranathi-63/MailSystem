# Makefile for SimpleMail System
# Produces: smserver, smspooler, smclient

CXX ?= g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2

# Detect OS
ifeq ($(OS),Windows_NT)
    LDFLAGS = -lws2_32
    RM = del /Q /F
    EXT = .exe
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        LDFLAGS = -pthread
    else
        LDFLAGS = -pthread -lrt
    endif
    RM = rm -rf
    EXT =
endif

TARGETS = smserver$(EXT) smspooler$(EXT) smclient$(EXT)

all: $(TARGETS)

smspooler$(EXT): smspooler.cpp smcommon.h
	$(CXX) $(CXXFLAGS) smspooler.cpp -o smspooler$(EXT) $(LDFLAGS)

smserver$(EXT): smserver.cpp smcommon.h
	$(CXX) $(CXXFLAGS) smserver.cpp -o smserver$(EXT) $(LDFLAGS)

smclient$(EXT): smclient.cpp smcommon.h
	$(CXX) $(CXXFLAGS) smclient.cpp -o smclient$(EXT) $(LDFLAGS)

clean:
	$(RM) $(TARGETS) *.o *.exe
