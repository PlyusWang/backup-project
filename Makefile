CXX := g++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic

BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/backupctl

APP_SOURCES := app/backupctl.cpp
CORE_SOURCES := src/core/backup_engine.cpp
FILESYSTEM_SOURCES := src/filesystem/file_system.cpp
SOURCES := $(APP_SOURCES) $(CORE_SOURCES) $(FILESYSTEM_SOURCES)
OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
DEPENDS := $(OBJECTS:.o=.d)

.PHONY: all debug sanitize test clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $(TARGET)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Debug build with symbols, kept in a separate directory so it never
# clobbers the release binary.
debug:
	@$(MAKE) BUILD_DIR=build-debug CXXFLAGS="$(CXXFLAGS) -g" all

# AddressSanitizer + UndefinedBehaviorSanitizer build.
sanitize:
	@$(MAKE) BUILD_DIR=build-sanitize CXXFLAGS="$(CXXFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" all

test: all
	@bash scripts/test.sh

clean:
	@rm -rf $(BUILD_DIR) build-debug build-sanitize

-include $(DEPENDS)
