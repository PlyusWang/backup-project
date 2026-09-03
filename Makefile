CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic

BUILD_DIR := build
TARGET := $(BUILD_DIR)/backupctl
SOURCE := app/backupctl.cpp

.PHONY: all debug test clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SOURCE) -o $(TARGET)

debug:
	@$(MAKE) CXXFLAGS="$(CXXFLAGS) -g"

test: all
	@bash scripts/test.sh

clean:
	@rm -rf $(BUILD_DIR)