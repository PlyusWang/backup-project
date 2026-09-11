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

.PHONY: all debug sanitize test gui clean

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

# ---- 桌面 GUI（Qt 6 Widgets + Qt Concurrent）----
# GUI 做成独立目标：默认的 make 依旧只构建 CLI，
# 这样没装 Qt 开发包的机器照样能编译、测试核心。
QT_PACKAGES := Qt6Widgets Qt6Concurrent
# Qt 的 include 目录转成 -isystem：Qt 自己的头文件不参与本项目的警告统计，
# 我们自己的代码仍然保持 -Wall -Wextra -Wpedantic 全开。
QT_CFLAGS := $(patsubst -I%,-isystem %,$(shell pkg-config --cflags $(QT_PACKAGES) 2>/dev/null))
QT_LIBS := $(shell pkg-config --libs $(QT_PACKAGES) 2>/dev/null)

GUI_TARGET := $(BUILD_DIR)/backup-gui
GUI_SOURCES := $(wildcard ui/desktop/*.cpp)
GUI_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(GUI_SOURCES))
CORE_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CORE_SOURCES) $(FILESYSTEM_SOURCES))
DEPENDS += $(GUI_OBJECTS:.o=.d)

gui: check-qt $(GUI_TARGET)

# 缺 Qt 时给一句能照做的提示，而不是让 g++ 抛出一屏找不到头文件的错误。
check-qt:
	@pkg-config --exists $(QT_PACKAGES) || { \
		echo "未找到 Qt 6 开发包（需要 $(QT_PACKAGES)）。"; \
		echo "Ubuntu 上可执行: sudo apt-get install -y qt6-base-dev qt6-base-dev-tools"; \
		exit 1; \
	}

$(GUI_TARGET): $(GUI_OBJECTS) $(CORE_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(GUI_OBJECTS) $(CORE_OBJECTS) $(QT_LIBS) -o $@

# GUI 单独一条模式规则：Qt 需要 -fPIC 和 Qt 的 include 路径；
# 这条规则的 stem 更短，GNU Make 会优先选它，CLI 的规则不受影响。
$(BUILD_DIR)/ui/desktop/%.o: ui/desktop/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC $(QT_CFLAGS) -MMD -MP -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR) build-debug build-sanitize

-include $(DEPENDS)
