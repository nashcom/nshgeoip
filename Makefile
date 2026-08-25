# nshgeoip - small local GeoIP lookup daemon
#
# Requires: g++ with C++17, libmaxminddb (dev headers + library), pthread.
# Linux only.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -pthread
LDFLAGS  ?= -pthread
LDLIBS   := -lmaxminddb

PREFIX      ?= /usr/local
SBINDIR     := $(PREFIX)/sbin
CONF_DIR    := /etc/nshgeoip

SRC_DIR  := src
BIN      := nshgeoip

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

TEST_BIN     := tests/test_nshgeoip
TEST_SRCS    := tests/test_nshgeoip.cpp \
                 $(SRC_DIR)/ip_addr.cpp \
                 $(SRC_DIR)/text_util.cpp \
                 $(SRC_DIR)/config.cpp \
                 $(SRC_DIR)/http.cpp \
                 $(SRC_DIR)/metrics.cpp

.PHONY: all clean test install

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_SRCS)

clean:
	rm -f $(OBJS) $(DEPS) $(BIN) $(TEST_BIN)

# Installs the binary and a starter config only -- does NOT create the
# nshgeoip user/group, install/enable the systemd service, or touch an
# existing config file. Use ./nshgeoipctl.sh install instead for all of
# that in one step; this target exists for a bare binary-only install.
install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(SBINDIR)/$(BIN)
	install -m 0755 -d $(DESTDIR)$(CONF_DIR)
	install -m 0644 etc/nshgeoip.conf.example \
		$(DESTDIR)$(CONF_DIR)/nshgeoip.conf.example
	@echo
	@echo "Installed $(BIN) to $(DESTDIR)$(SBINDIR)/$(BIN)"
	@echo "Run ./nshgeoipctl.sh install for the user/group, real config,"
	@echo "and systemd service -- or see README.md to do it manually."
