CXX ?= g++
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wpedantic
OUT ?= ../build/bin
.PHONY: all test clean
all: $(OUT)/dho804-usb-native $(OUT)/test_transport
$(OUT):
	mkdir -p $@
$(OUT)/dho804-usb-native: bridge.cpp transport.hpp | $(OUT)
	$(CXX) $(CXXFLAGS) -pthread bridge.cpp -o $@ $(LDFLAGS)
$(OUT)/test_transport: test_transport.cpp transport.hpp | $(OUT)
	$(CXX) $(CXXFLAGS) -pthread test_transport.cpp -o $@ $(LDFLAGS)
test: all
	$(OUT)/test_transport
	$(OUT)/dho804-usb-native --help
clean:
	rm -f $(OUT)/dho804-usb-native $(OUT)/test_transport
.PHONY: asan tsan
asan: | $(OUT)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer test_transport.cpp -o $(OUT)/test_transport_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(OUT)/test_transport_asan
tsan: | $(OUT)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -pthread -fsanitize=thread -fno-omit-frame-pointer test_transport.cpp -o $(OUT)/test_transport_tsan
	$(OUT)/test_transport_tsan
