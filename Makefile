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
                src/crypto/sha512.cpp \
                src/crypto/ed25519.cpp \
                src/crypto/bpcert.cpp \
                src/crypto/trusted_root_store.cpp \
                src/crypto/x25519.cpp src/crypto/hkdf.cpp \
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
                src/realtime/realtime_watcher.cpp \
                src/realtime/realtime_debouncer.cpp \
                src/realtime/realtime_backup_service.cpp \
                src/cli/terminal_secret.cpp \
                src/cli/realtime_commands.cpp \
                src/cli/cli_commands.cpp

# ---- 远程备份服务端（PR #20）----
#
# backup-server 是独立进程，只做"存储后端 + 传输边界"：协议、认证、元数据、
# 流式落盘。它**不链接** BackupEngine / Filter / MyPack / USTAR / 压缩 /
# 加密 / 增量链——那些属于本地备份核心，服务端不重新实现第二套。
# 因此它的源文件列表是显式的，而不是复用 CORE_SOURCES。
#
# 桌面端（backupctl / 两个 GUI）不链接 SQLite：只有服务端需要元数据库。
SERVER_TARGET := $(BUILD_DIR)/backup-server
# 服务端独有的源码**不放在 src/ 下**：src/ 是桌面核心，若干既有测试脚本会把
# src/**/*.cpp 整个编译并链接一遍，把需要 SQLite、还带自己的 main() 的服务端
# 源文件混进去会让它们全部失败。协议与客户端（backupctl/GUI 也要用）留在
# src/network/，服务端实现放 server/。
SERVER_CORE_SOURCES := src/network/network_protocol.cpp \
                       server/remote_auth.cpp \
                       server/remote_metadata_store.cpp \
                       server/remote_maintenance.cpp \
                       server/remote_server.cpp \
                       src/platform/file_lock.cpp \
                       src/crypto/sha256.cpp \
                       src/crypto/sha512.cpp \
                       src/crypto/ed25519.cpp \
                       src/crypto/bpcert.cpp \
                       src/crypto/trusted_root_store.cpp \
                       src/crypto/hmac.cpp \
                       src/crypto/pbkdf2.cpp \
                       src/crypto/random.cpp \
                       src/crypto/aes.cpp \
                       src/crypto/x25519.cpp \
                       src/crypto/hkdf.cpp \
                       src/network/secure_transport.cpp
SERVER_SOURCES := server/main.cpp $(SERVER_CORE_SOURCES)
# 服务端的目标文件放在 $(BUILD_DIR)/server/ 下，**不要**落在 $(BUILD_DIR)/src/。
# 既有的测试脚本用 "find build/src -name '*.o'" 收集核心对象来链接单元测试，
# 把服务端的 main.o 与需要 SQLite 的目标文件混进去会让它们全部链接失败。
SERVER_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/server/%.o,$(SERVER_SOURCES))

# SQLite 头文件：优先用系统装的 libsqlite3-dev，否则用仓库里固定的官方头。
# 链接一律直接指向系统运行库 libsqlite3.so.0，不依赖 -lsqlite3 的开发符号
# 链接，因此在只装了运行时库的机器上也能构建。
SQLITE_HEADER := $(firstword $(wildcard /usr/include/sqlite3.h) \
                            $(wildcard third_party/sqlite/include/sqlite3.h))
SQLITE_INCLUDE_DIR := $(dir $(SQLITE_HEADER))
SQLITE_LIBRARY := $(firstword $(wildcard /usr/lib/x86_64-linux-gnu/libsqlite3.so) \
                              $(wildcard /usr/lib/x86_64-linux-gnu/libsqlite3.so.0) \
                              $(wildcard /usr/lib64/libsqlite3.so) \
                              $(wildcard /usr/lib/libsqlite3.so))

FILESYSTEM_SOURCES := src/filesystem/file_system.cpp

# ---- 远程备份客户端（PR #20）----
#
# CLI 与 Modern GUI 共用同一个 RemoteArchiveClient：桌面端只链接协议编解码与
# 客户端，**不链接 SQLite**（元数据库只属于服务端进程）。
CORE_SOURCES += src/network/network_protocol.cpp \
                src/network/secure_transport.cpp \
                src/network/remote_backup_client.cpp \
                src/network/snapshot_bundle.cpp \
                src/network/remote_incremental.cpp \
                src/cli/remote_commands.cpp
SOURCES := $(APP_SOURCES) $(CORE_SOURCES) $(FILESYSTEM_SOURCES)
OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
DEPENDS := $(OBJECTS:.o=.d)
# 注意：这一条必须放在上面的 := 赋值**之后**。DEPENDS 是立即赋值，
# 放在前面会被整体覆盖，服务端的目标文件就再也不追踪
# include/remote_server.h，改了头文件也只重建一半目标文件——
# 两个目标文件对同一个结构体的大小理解不一致，
# 后果是构造对象时越界写坏调用者的栈 canary。
DEPENDS += $(SERVER_OBJECTS:.o=.d)

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

.PHONY: all debug sanitize test test-fixtures server remote-sequence gui gui-modern gui-all cert-tool clean

# 产品构建：三个产品产物（CLI + 服务端 + 测试夹具除外）。
# archive-cli 是测试夹具，见上面的说明。
all: $(TARGET) $(SERVER_TARGET)

# ---- ECS 本地管理工具（PR #20 closure）----
#
# backup-server-admin 是**只能在服务器本机运行**的管理工具：管理员先 SSH 进
# ECS，再在 ECS 上执行它。它不监听任何端口（源码里没有 socket() / bind() /
# listen()）、不说 BPNET1、不链接 Qt，也不在任何 GUI / CLI 的调用路径上。
#
# 它和服务端共用同一份 RemoteMaintenance 与 RemoteMetadataStore：管理工具的
# 删除动作与服务端的 DELETE / DELETE_ACCOUNT 走的是同一批函数，不存在
# "管理工具另有一套删除逻辑"这种分叉。
ADMIN_TARGET := $(BUILD_DIR)/backup-server-admin
ADMIN_SOURCES := server/admin_main.cpp $(SERVER_CORE_SOURCES)
ADMIN_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/server/%.o,$(ADMIN_SOURCES))
DEPENDS += $(ADMIN_OBJECTS:.o=.d)

# ---- 服务端传输身份密钥工具（PR #21）----
#
# backup-server-keygen 生成 BPSEC1 的服务端长期身份密钥（32 字节 X25519 标量，
# 0600 文件），并打印公钥与指纹供客户端 pin。它只在服务器本机运行，不监听端口。
# 它和服务端共用同一份 secure_transport.cpp，不存在"密钥工具另写一套编码"。
KEYGEN_TARGET := $(BUILD_DIR)/backup-server-keygen
KEYGEN_SOURCES := server/keygen_main.cpp $(SERVER_CORE_SOURCES)
KEYGEN_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/server/%.o,$(KEYGEN_SOURCES))
DEPENDS += $(KEYGEN_OBJECTS:.o=.d)

$(KEYGEN_TARGET): $(KEYGEN_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(KEYGEN_OBJECTS) $(SQLITE_LIBRARY) -pthread -o $@

# ---- 离线根与服务器身份证书工具（PR #23）----
#
# backup-cert-tool 管离线根私钥与服务器身份证书：root-init / root-info /
# issue-server / verify-server / inspect-server。它和客户端、服务端共用同一份
# bpcert.cpp + trusted_root_store.cpp —— 不存在"工具另写一套证书解析"。
# 私钥只能从 --root-key <文件路径> 读：工具里没有任何命令行十六进制入口。
CERT_TOOL_TARGET := $(BUILD_DIR)/backup-cert-tool
CERT_TOOL_SOURCES := tools/cert_tool_main.cpp $(SERVER_CORE_SOURCES)
CERT_TOOL_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/server/%.o,$(CERT_TOOL_SOURCES))
DEPENDS += $(CERT_TOOL_OBJECTS:.o=.d)

$(CERT_TOOL_TARGET): $(CERT_TOOL_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CERT_TOOL_OBJECTS) $(SQLITE_LIBRARY) -pthread -o $@

# 证书工具是运维/离线工具，不进 all 与 server 的默认目标，单独显式构建。
cert-tool: $(CERT_TOOL_TARGET)

# 单独构建服务端（部署脚本用）。管理工具、密钥工具与服务端同属"服务器侧
# 交付物"，所以同一条目标一起构建。
server: $(SERVER_TARGET) $(ADMIN_TARGET) $(KEYGEN_TARGET)

$(SERVER_TARGET): $(SERVER_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SERVER_OBJECTS) $(SQLITE_LIBRARY) -pthread -o $@

$(ADMIN_TARGET): $(ADMIN_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(ADMIN_OBJECTS) $(SQLITE_LIBRARY) -pthread -o $@

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

# ---- ECS 真机序列驱动器（测试工具，不是产品功能）----
#
# tests/tools/remote_sequence.cpp 把人工验收那一串动作（注册 / 错误口令 ×4 /
# LIST ×10 / 空闲 / 上传 / 下载 / 删除快照 / 注销）跑在**一条真实连接**上，
# 供 scripts/aliyun_sequence_e2e.sh 在 ECS 真机复验。和 archive-cli 一样，
# 它不在默认构建里，需要时显式构建：
#
#   make remote-sequence
REMOTE_SEQUENCE_TARGET := $(BUILD_DIR)/remote-sequence
REMOTE_SEQUENCE_SOURCES := tests/tools/remote_sequence.cpp
REMOTE_SEQUENCE_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(REMOTE_SEQUENCE_SOURCES))
# CORE_OBJECTS 在下面才定义（GUI 那一段），这里显式算一份同样的集合。
REMOTE_SEQUENCE_CORE_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CORE_SOURCES) $(FILESYSTEM_SOURCES))
DEPENDS += $(REMOTE_SEQUENCE_OBJECTS:.o=.d)

remote-sequence: $(REMOTE_SEQUENCE_TARGET)

$(REMOTE_SEQUENCE_TARGET): $(REMOTE_SEQUENCE_OBJECTS) $(REMOTE_SEQUENCE_CORE_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(REMOTE_SEQUENCE_OBJECTS) $(REMOTE_SEQUENCE_CORE_OBJECTS) -o $@

# 服务端源码单独一条模式规则：只有它们需要 SQLite 的头文件路径，
# 并且统一落在 $(BUILD_DIR)/server/ 下（见上面 SERVER_OBJECTS 的说明）。
$(BUILD_DIR)/server/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -I$(SQLITE_INCLUDE_DIR) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Debug build with symbols, kept in a separate directory so it never
# clobbers the release binary.
debug:
	@$(MAKE) BUILD_DIR=build-debug CXXFLAGS="$(CXXFLAGS) -g" all

# AddressSanitizer + UndefinedBehaviorSanitizer build.
# sanitize 是测试用的构建，所以顺带把测试夹具与**服务器侧交付物**也建出来：
# 脚本里的 sanitizer 维度要用 backup-server-keygen 生成传输身份密钥，
# 只建 all 的话它不存在（PR #21 就是这么暴露出来的）。产品构建（all）里没有这一条。
sanitize:
	@$(MAKE) BUILD_DIR=build-sanitize CXXFLAGS="$(CXXFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" all test-fixtures server

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
# PR #22 起多了 Qt6Network：SSH 安全通道的就绪判定与活性探测用 QTcpSocket /
# QTcpServer（"进程还活着"不等于"本地端口连得上"）。Qt6Network 与 Qt6Core
# 同属 qt6-base-dev，没有引入新的系统依赖。
QT_QML_PACKAGES := Qt6Quick Qt6Qml Qt6QuickControls2 Qt6Concurrent Qt6Network
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
		echo "Ubuntu 上可执行: sudo apt-get install -y qt6-base-dev qt6-declarative-dev qt6-declarative-dev-tools qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-dialogs qml6-module-qtquick-shapes"; \
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
