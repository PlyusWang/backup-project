CXX := g++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic

BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/backupctl

APP_SOURCES := app/backupctl.cpp
CORE_SOURCES := src/core/archive_entry.cpp src/core/archive_pipeline.cpp \
                src/core/backup_engine.cpp src/core/backup_preview.cpp \
                src/core/file_io.cpp \
                src/core/source_digest.cpp \
                src/core/incremental_delta.cpp \
                src/core/incremental_restore.cpp \
                src/core/incremental_backup.cpp \
                src/core/tree_scanner.cpp src/core/source_tree_walker.cpp \
                src/core/user_directory.cpp src/archive/archive.cpp \
                src/archive/archive_path.cpp src/archive/container_format.cpp \
                src/archive/mypack_v2.cpp src/archive/pack_stream.cpp \
                src/archive/ustar.cpp src/catalog/backup_catalog.cpp \
                src/config/config_manager.cpp src/compression/codec_io.cpp src/compression/huffman.cpp \
                src/compression/lzss.cpp src/crypto/aes.cpp \
                src/crypto/des.cpp src/crypto/hmac.cpp src/crypto/pbkdf2.cpp \
                src/crypto/random.cpp src/crypto/sha256.cpp \
                src/filter/filter.cpp src/filter/filter_rule_builder.cpp


# PR #17：定时备份 + 变化检测 + retention 的共享核心。
# 这些源文件都是 Qt 无关的纯 C++17，CLI 与 Modern GUI 共用同一份，
# 所以它们属于 CORE_SOURCES，而不是某个前端的目标。
CORE_SOURCES += src/core/backup_mode.cpp \
                src/core/backup_option_keys.cpp \
                src/core/simple_json.cpp \
                src/platform/app_paths.cpp \
                src/platform/file_lock.cpp \
                src/platform/application_instance_lock.cpp \
                src/scheduler/source_manifest.cpp \
                src/scheduler/schedule_store.cpp \
                src/scheduler/scheduled_backup_service.cpp \
                src/scheduler/scheduler_lock.cpp \
                src/realtime/realtime_store.cpp \
                src/cli/terminal_secret.cpp \
                src/cli/cli_commands.cpp

FILESYSTEM_SOURCES := src/filesystem/file_system.cpp
SOURCES := $(APP_SOURCES) $(CORE_SOURCES) $(FILESYSTEM_SOURCES)
OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
DEPENDS := $(OBJECTS:.o=.d)

# ---- 归档格式的测试夹具（不是产品命令）----
#
# 产品 CLI 与 Modern GUI 一样是 repository-driven：归档名由 BackupCatalog 在
# 配置好的仓库里生成，调用方不能指定任意路径。但归档格式本身（v0.1 legacy /
# v2 container）的端到端回归需要"写到指定路径、再从这个路径恢复"，所以那部分
# 能力搬到了这个独立可执行文件里。
#
# 它**不在默认构建目标里**：普通的 make / make all / make gui-all 是产品构建，
# 不该产出一个用户看不懂、也不该去用的可执行文件。需要它的测试自己显式构建：
#
#   make test-fixtures                     # 普通构建目录
#   make sanitize                          # 顺带构建 build-sanitize 的那一份
#
# 它不出现在 backupctl --help 里，不参与 GUI/CLI parity，也不是用户功能，
# 也不拿产品单实例锁（它不是产品前端）。
FIXTURE_TARGET := $(BUILD_DIR)/archive-cli
FIXTURE_SOURCES := tests/tools/archive_cli.cpp
FIXTURE_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(FIXTURE_SOURCES))
# CORE_OBJECTS 在下面才定义（GUI 那一段），这里显式算一份同样的集合：
# 目标的前置条件在解析这条规则时就要展开，用后面的变量会拿到空值。
FIXTURE_CORE_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CORE_SOURCES) $(FILESYSTEM_SOURCES))
DEPENDS += $(FIXTURE_OBJECTS:.o=.d)

.PHONY: all debug sanitize test test-fixtures gui gui-modern gui-all clean

# 产品构建：只有产品前端。archive-cli 是测试夹具，见上面的说明。
all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $(TARGET)

# 显式构建测试夹具。测试脚本用它，产品构建不用它。
test-fixtures: $(FIXTURE_TARGET)

# 与 backupctl 共享同一份 CORE_SOURCES：夹具调用的就是产品用的引擎与读写器，
# 不存在"测试用另一套实现"。
$(FIXTURE_TARGET): $(FIXTURE_OBJECTS) $(FIXTURE_CORE_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(FIXTURE_OBJECTS) $(CORE_OBJECTS) -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Debug build with symbols, kept in a separate directory so it never
# clobbers the release binary.
debug:
	@$(MAKE) BUILD_DIR=build-debug CXXFLAGS="$(CXXFLAGS) -g" all

# AddressSanitizer + UndefinedBehaviorSanitizer build.
# sanitize 是测试用的构建，所以顺带把测试夹具也建出来（脚本里的 sanitizer
# 维度要跑它）。产品构建（all）里没有这一条。
sanitize:
	@$(MAKE) BUILD_DIR=build-sanitize CXXFLAGS="$(CXXFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" all test-fixtures

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

# ---- 现代 GUI（Qt Quick / QML + Qt Concurrent）----
# 和 Widgets GUI 完全并行：make gui 与 make gui-modern 各自构建各自的产物，
# 默认的 make 依旧只构建 CLI，因此没有 Qt Quick 的环境也能编译核心。
QT_QML_PACKAGES := Qt6Quick Qt6Qml Qt6QuickControls2 Qt6Concurrent
QT_QML_CFLAGS := $(patsubst -I%,-isystem %,$(shell pkg-config --cflags $(QT_QML_PACKAGES) 2>/dev/null))
QT_QML_LIBS := $(shell pkg-config --libs $(QT_QML_PACKAGES) 2>/dev/null)

# moc / rcc 通过 qtpaths6 动态定位，不写死 /usr/lib/qt6/libexec 这类机器相关路径。
QT_TOOLS_DIR := $(shell qtpaths6 --query QT_INSTALL_LIBEXECS 2>/dev/null)
MOC := $(firstword $(wildcard $(QT_TOOLS_DIR)/moc) $(shell command -v moc 2>/dev/null))
RCC := $(firstword $(wildcard $(QT_TOOLS_DIR)/rcc) $(shell command -v rcc 2>/dev/null))

MODERN_TARGET := $(BUILD_DIR)/backup-gui-modern
MODERN_SOURCES := $(wildcard ui/modern/*.cpp)
# 带 Q_OBJECT 的头文件都要先过 moc：目前是 BackupController 和 AppTheme。
MODERN_HEADERS := $(wildcard ui/modern/*.h)
MODERN_MOC_SOURCES := $(patsubst ui/modern/%.h,$(BUILD_DIR)/ui/modern/moc_%.cpp,$(MODERN_HEADERS))
MODERN_QRC_SOURCE := $(BUILD_DIR)/ui/modern/qrc_resources.cpp
MODERN_GENERATED_OBJECTS := $(MODERN_MOC_SOURCES:.cpp=.o) $(MODERN_QRC_SOURCE:.cpp=.o)
MODERN_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(MODERN_SOURCES)) $(MODERN_GENERATED_OBJECTS)
DEPENDS += $(MODERN_OBJECTS:.o=.d)

gui-modern: check-qt-qml $(MODERN_TARGET)

# 缺依赖时给一句能照做的提示，而不是让 g++ 抛出一屏找不到头文件的错误。
check-qt-qml:
	@pkg-config --exists $(QT_QML_PACKAGES) || { \
		echo "未找到 Qt Quick / QML 开发包（需要 $(QT_QML_PACKAGES)）。"; \
		echo "Ubuntu 上可执行: sudo apt-get install -y qt6-declarative-dev qt6-declarative-dev-tools qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-dialogs qml6-module-qtquick-shapes"; \
		exit 1; \
	}
	@test -x "$(MOC)" || { echo "找不到 moc，请确认 qt6-base-dev-tools 已安装。"; exit 1; }
	@test -x "$(RCC)" || { echo "找不到 rcc，请确认 qt6-base-dev-tools 已安装。"; exit 1; }

$(MODERN_TARGET): $(MODERN_OBJECTS) $(CORE_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MODERN_OBJECTS) $(CORE_OBJECTS) $(QT_QML_LIBS) -o $@

$(BUILD_DIR)/ui/modern/%.o: ui/modern/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -Iui/modern $(CXXFLAGS) -fPIC $(QT_QML_CFLAGS) -MMD -MP -c $< -o $@

# Q_OBJECT 头文件的元对象代码由 moc 生成，再按普通 C++ 编译。
$(BUILD_DIR)/ui/modern/moc_%.cpp: ui/modern/%.h
	@mkdir -p $(dir $@)
	$(MOC) $< -o $@

# QML 与图标全部编进可执行文件，这样从任意工作目录启动都能找到资源。
$(MODERN_QRC_SOURCE): ui/modern/resources.qrc $(wildcard ui/modern/qml/*.qml ui/modern/qml/*/*.qml)
	@mkdir -p $(dir $@)
	$(RCC) --name modern_resources $< -o $@

$(BUILD_DIR)/ui/modern/moc_%.o: $(BUILD_DIR)/ui/modern/moc_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -Iui/modern $(CXXFLAGS) -fPIC $(QT_QML_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/ui/modern/qrc_resources.o: $(MODERN_QRC_SOURCE)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -Iui/modern $(CXXFLAGS) -fPIC $(QT_QML_CFLAGS) -MMD -MP -c $< -o $@

# 一次构建两套 GUI；不是默认目标，避免把 Qt Quick 变成 make 的依赖。
gui-all: gui gui-modern

clean:
	@rm -rf $(BUILD_DIR) build-debug build-sanitize

-include $(DEPENDS)
