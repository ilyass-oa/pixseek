BUILD ?= build
PYTHON ?= python3
CPPFLAGS += -Iinclude -Iimg-dist -MMD -MP
CFLAGS += -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic
CXXFLAGS += -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -pthread
LDLIBS += -lm -pthread

ifeq ($(SANITIZE),1)
CFLAGS += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
CXXFLAGS += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

LIBOBJ = $(addprefix $(BUILD)/img-dist/,bmp.o pHash.o verbose.o)
SERVEROBJ = $(BUILD)/src/server.o $(BUILD)/src/catalog.o
CLIENTOBJ = $(BUILD)/src/client.o
TESTOBJ = $(BUILD)/tests/bmp_test.o

.PHONY: all test clean
all: $(BUILD)/pixseek-server $(BUILD)/pixseek-client

$(BUILD)/pixseek-server: $(SERVEROBJ) $(LIBOBJ)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/pixseek-client: $(CLIENTOBJ)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/bmp-tests: $(TESTOBJ) $(LIBOBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: all $(BUILD)/bmp-tests
	$(BUILD)/bmp-tests
	PIXSEEK_BUILD=$(abspath $(BUILD)) $(PYTHON) -m unittest discover -s tests -v

clean:
	rm -rf -- $(BUILD)

-include $(SERVEROBJ:.o=.d) $(CLIENTOBJ:.o=.d) $(TESTOBJ:.o=.d) $(LIBOBJ:.o=.d)
