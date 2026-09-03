param(
    # 删除当前项目的 .git 并重新初始化。
    # 对于本次“第一次初始化中断”的情况建议使用。
    [switch]$ResetLocalGit,

    # 如果 GitHub 上已经存在 PlyusWang/backup-project，
    # 删除它并重新创建。
    [switch]$RecreateRemote
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ============================================================
# 0. 项目基本配置
# ============================================================

$GitHubOwner = 'PlyusWang'
$RepoName = 'backup-project'
$GitHubRepo = "$GitHubOwner/$RepoName"

# 本脚本必须放在 backup-project 根目录。
$ProjectRoot = $PSScriptRoot

# 原来的项目工程基线文档。
$BaselineSource = 'E:\Documents\1学习\3计算机\课程\软件开发综合实验\work\docs\00_project_baseline.md'
$BaselineDestination = Join-Path $ProjectRoot 'docs\00_project_baseline.md'

Write-Host ''
Write-Host '============================================================' -ForegroundColor Cyan
Write-Host ' Backup Project Repair / Initialization' -ForegroundColor Cyan
Write-Host '============================================================' -ForegroundColor Cyan
Write-Host "Project root : $ProjectRoot"
Write-Host "GitHub repo  : $GitHubRepo"
Write-Host ''

Set-Location $ProjectRoot

# ============================================================
# 1. 辅助函数
# ============================================================

# 写 UTF-8 无 BOM + Linux LF 文件。
# 这样在 Windows 创建的 shell script 拿到 Linux 后不会因为 CRLF 出错。
function Write-Utf8Lf {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string]$Content
    )

    $FullPath = if ([System.IO.Path]::IsPathRooted($Path)) {
        $Path
    }
    else {
        Join-Path $ProjectRoot $Path
    }

    $Parent = Split-Path -Parent $FullPath

    if ($Parent -and -not (Test-Path $Parent)) {
        New-Item -ItemType Directory -Path $Parent -Force | Out-Null
    }

    # PowerShell here-string 内部可能存在 CRLF，
    # 统一转换为 Linux LF。
    $Content = $Content -replace "`r`n", "`n"
    $Content = $Content -replace "`r", "`n"

    $Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

    [System.IO.File]::WriteAllText(
        $FullPath,
        $Content,
        $Utf8NoBom
    )
}

# 只在文件不存在时写入。
# 防止以后再次运行 repair script 时覆盖已经编辑过的文档。
function Write-IfMissing {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string]$Content
    )

    $FullPath = Join-Path $ProjectRoot $Path

    if (-not (Test-Path $FullPath)) {
        Write-Utf8Lf -Path $Path -Content $Content
        Write-Host "[CREATE] $Path"
    }
    else {
        Write-Host "[KEEP]   $Path"
    }
}

function Assert-Command {
    param(
        [Parameter(Mandatory)]
        [string]$Name,

        [Parameter(Mandatory)]
        [string]$InstallHint
    )

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "未找到命令 '$Name'。`n$InstallHint"
    }
}

# ============================================================
# 2. 检查 Git / GitHub CLI
# ============================================================

Assert-Command `
    -Name 'git' `
    -InstallHint '请先安装 Git。'

Assert-Command `
    -Name 'gh' `
    -InstallHint '可以执行：winget install --id GitHub.cli -e'

Write-Host ''
Write-Host '[1/12] Checking GitHub authentication...' -ForegroundColor Yellow

gh auth status

if ($LASTEXITCODE -ne 0) {
    throw @'
GitHub CLI 尚未登录。

请先执行：

gh auth login

然后重新运行本脚本。
'@
}

# ============================================================
# 3. 如果要求，清除中断留下的本地 Git 仓库
# ============================================================

Write-Host ''
Write-Host '[2/12] Checking local Git repository...' -ForegroundColor Yellow

$GitDirectory = Join-Path $ProjectRoot '.git'

if ($ResetLocalGit -and (Test-Path $GitDirectory)) {
    Write-Host 'Removing old .git directory...' -ForegroundColor Yellow
    Remove-Item -LiteralPath $GitDirectory -Recurse -Force
    Write-Host '[OK] Old local Git metadata removed.'
}
elseif (Test-Path $GitDirectory) {
    Write-Host '[KEEP] Existing .git repository.'
}
else {
    Write-Host '[INFO] No existing .git repository.'
}

# ============================================================
# 4. 创建完整目录结构
# ============================================================

Write-Host ''
Write-Host '[3/12] Creating directory structure...' -ForegroundColor Yellow

$Directories = @(
    'docs',
    'docs\uml',
    'docs\ui',
    'docs\gantt',

    'report',
    'report\sections',
    'report\figures',
    'report\tables',
    'report\appendices',

    'include',

    'src',
    'src\core',
    'src\filesystem',
    'src\archive',
    'src\compression',
    'src\crypto',
    'src\scheduler',
    'src\watcher',
    'src\network',

    'app',

    'ui',
    'ui\css',
    'ui\js',

    'tests',
    'tests\unit',
    'tests\integration',
    'tests\system',
    'tests\performance',

    'scripts',

    'build'
)

foreach ($Directory in $Directories) {
    $FullPath = Join-Path $ProjectRoot $Directory
    New-Item -ItemType Directory -Path $FullPath -Force | Out-Null
}

Write-Host '[OK] Directory structure created.'

# ============================================================
# 5. 恢复 00_project_baseline.md
# ============================================================

Write-Host ''
Write-Host '[4/12] Restoring project baseline...' -ForegroundColor Yellow

if (-not (Test-Path -LiteralPath $BaselineSource)) {
    throw "找不到工程基线文档：`n$BaselineSource"
}

Copy-Item `
    -LiteralPath $BaselineSource `
    -Destination $BaselineDestination `
    -Force

Write-Host '[OK] docs/00_project_baseline.md restored.'

# ============================================================
# 6. 创建后续 Markdown 文档
# ============================================================

Write-Host ''
Write-Host '[5/12] Creating documentation skeleton...' -ForegroundColor Yellow

Write-IfMissing 'docs\01_requirements.md' @'
# 需求分析

> 文档编号：01  
> 状态：Draft

本文档用于记录项目的需求分析。

计划主要包含：

- 项目目标；
- 功能需求；
- 非功能需求；
- 用户交互设计；
- 系统用例图；
- 用例描述；
- 项目规划甘特图；
- 需求追踪关系。
'@

Write-IfMissing 'docs\02_architecture.md' @'
# 系统设计

> 文档编号：02  
> 状态：Draft

本文档用于记录系统设计。

计划主要包含：

- 总体架构；
- 构件图；
- 构件及子系统描述；
- 类图；
- 类及对象描述；
- 顺序图；
- 主要用例流程；
- 开发工具；
- 第三方库及依赖说明。
'@

Write-IfMissing 'docs\03_testing.md' @'
# 软件测试

> 文档编号：03  
> 状态：Draft

本文档用于记录软件测试工作。

计划主要包含：

- 测试目标；
- 测试环境；
- 测试方法；
- 测试工具；
- 单元测试；
- 集成测试；
- 黑盒测试；
- 白盒测试；
- 系统测试；
- 性能与压力测试；
- 正式测试用例；
- 测试结果。
'@

Write-IfMissing 'docs\04_release_and_demo.md' @'
# 发布与演示

> 文档编号：04  
> 状态：Draft

本文档用于记录：

- 自动构建；
- 软件发布；
- 部署；
- 演示流程；
- 答辩准备；
- Git 开发证据；
- 版本演进；
- 最终检查清单。
'@

# ============================================================
# 7. README.md
# ============================================================

Write-Host ''
Write-Host '[6/12] Creating project configuration files...' -ForegroundColor Yellow

Write-Utf8Lf 'README.md' @'
# Backup Project

Linux 环境下的数据备份与恢复软件课程项目。

## 技术路线

- 核心后端：C++17
- 主要运行平台：Linux
- 前端：Web
- Web 辅助层：Python
- 构建：GNU Make
- 版本控制：Git / GitHub
- C++ 编程规范：Google C++ Style Guide
- 工程文档：Markdown
- 最终报告：LaTeX → PDF

## 当前目标

项目计划完成不少于 120 分的课程功能。

当前首先完成：

```text
基础 Backup / Restore
```

即建立：

```text
源目录
   │
   ▼
备份
   │
   ▼
备份仓库
   │
   ▼
还原
   │
   ▼
恢复目录
```

的最小闭环。

## 文档

- `docs/00_project_baseline.md`：项目工程基线
- `docs/01_requirements.md`：需求分析
- `docs/02_architecture.md`：系统设计
- `docs/03_testing.md`：软件测试
- `docs/04_release_and_demo.md`：发布与演示

## 开发环境

主要开发环境：

```text
Windows
   │
   │ SSH
   ▼
VMware Ubuntu
   │
   ├── GCC / G++
   ├── GNU Make
   ├── Git
   ├── gdb
   ├── Valgrind
   └── backup-project
```

后续网络备份可连接 Alibaba Cloud ECS。
'@

# ============================================================
# 8. .clang-format
# ============================================================

Write-Utf8Lf '.clang-format' @'
BasedOnStyle: Google
'@

# ============================================================
# 9. .gitignore
# ============================================================

Write-Utf8Lf '.gitignore' @'
# ============================================================
# Build
# ============================================================

/build/

*.o
*.obj
*.a
*.so
*.out

# Executables
backupctl
*.exe

# ============================================================
# Optional CMake artifacts
# ============================================================

/cmake-build-*/
CMakeCache.txt
CMakeFiles/

# ============================================================
# Python
# ============================================================

__pycache__/
*.py[cod]

.venv/
venv/

# ============================================================
# Test temporary output
# ============================================================

/tests/output/
/tests/tmp/

# ============================================================
# Logs
# ============================================================

*.log

# ============================================================
# IDE / Editor
# ============================================================

.vscode/
.idea/

*.swp
*.swo

# ============================================================
# Operating system
# ============================================================

.DS_Store
Thumbs.db

# ============================================================
# LaTeX temporary files
# ============================================================

*.aux
*.bbl
*.bcf
*.blg
*.fdb_latexmk
*.fls
*.run.xml
*.synctex.gz
*.toc
*.xdv

# 最终 PDF 可以提交，因此不全局忽略 PDF。
'@

# ============================================================
# 10. 最小 C++ CLI
# ============================================================

Write-Utf8Lf 'app\backupctl.cpp' @'
#include <iostream>
#include <string>

namespace {

void PrintUsage(const char* program_name) {
  std::cout << "Usage:\n"
            << "  " << program_name
            << " backup <source_directory> <repository>\n"
            << "  " << program_name
            << " restore <repository> <destination>\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    PrintUsage(argv[0]);
    return 0;
  }

  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  std::cerr << "Backup/Restore is not implemented yet.\n";
  return 1;
}
'@

# ============================================================
# 11. Makefile
#
# 使用 __TAB__ 占位，随后替换为真正的 TAB，
# 避免 Markdown / PowerShell 在复制过程中破坏 Makefile。
# ============================================================

$Makefile = @'
CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic

BUILD_DIR := build
TARGET := $(BUILD_DIR)/backupctl
SOURCE := app/backupctl.cpp

.PHONY: all debug test clean

all: $(TARGET)

$(TARGET): $(SOURCE)
__TAB__@mkdir -p $(BUILD_DIR)
__TAB__$(CXX) $(CXXFLAGS) $(SOURCE) -o $(TARGET)

debug:
__TAB__@$(MAKE) CXXFLAGS="$(CXXFLAGS) -g"

test: all
__TAB__@bash scripts/test.sh

clean:
__TAB__@rm -rf $(BUILD_DIR)
'@

$Makefile = $Makefile.Replace('__TAB__', "`t")

Write-Utf8Lf 'Makefile' $Makefile

# ============================================================
# 12. Linux Shell Scripts
# ============================================================

Write-Utf8Lf 'scripts\build.sh' @'
#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

echo "[build] Building backup-project..."

make

echo "[build] Build completed."
'@

Write-Utf8Lf 'scripts\test.sh' @'
#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

echo "[test] Running current smoke test..."

if [[ ! -x build/backupctl ]]; then
    echo "[test] backupctl is missing. Building first..."
    make
fi

./build/backupctl --help

echo "[test] Smoke test passed."
echo "[test] Real Backup / Restore tests will be implemented in Sprint 1."
'@

Write-Utf8Lf 'scripts\lint.sh' @'
#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format is not installed."
    exit 1
fi

mapfile -d '' FILES < <(
    find app src include \
        -type f \
        \( -name "*.cpp" -o -name "*.cc" -o -name "*.h" \) \
        -print0
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No C++ source files found."
    exit 0
fi

clang-format --dry-run --Werror "${FILES[@]}"

echo "clang-format check passed."
'@

Write-Utf8Lf 'scripts\package.sh' @'
#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

echo "Packaging is not implemented yet."
'@

# ============================================================
# 13. Web UI 骨架
# ============================================================

Write-IfMissing 'ui\index.html' @'
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta
    name="viewport"
    content="width=device-width, initial-scale=1.0"
  >
  <title>Backup System</title>
</head>

<body>
  <main>
    <h1>Backup System</h1>
    <p>Web UI placeholder.</p>
  </main>
</body>
</html>
'@

Write-IfMissing 'ui\server.py' @'
#!/usr/bin/env python3

"""Temporary Web bridge entry point.

Core backup functionality must remain implemented in C++.
"""


def main() -> None:
    print("Web bridge placeholder")


if __name__ == "__main__":
    main()
'@

# ============================================================
# 14. LaTeX 最终报告骨架
# ============================================================

Write-IfMissing 'report\main.tex' @'
\documentclass[UTF8]{ctexart}

\usepackage{graphicx}
\usepackage{booktabs}
\usepackage{tabularx}
\usepackage{longtable}
\usepackage{hyperref}

\title{数据备份软件项目报告}
\author{}
\date{}

\begin{document}

\maketitle
\tableofcontents

\input{sections/requirements}
\input{sections/architecture}
\input{sections/testing}

\appendix
\input{appendices/appendix}

\end{document}
'@

Write-IfMissing 'report\sections\requirements.tex' @'
\section{需求分析}

本节最终由项目的 Markdown 需求分析文档整理形成。
'@

Write-IfMissing 'report\sections\architecture.tex' @'
\section{系统设计}

本节最终由项目的 Markdown 系统设计文档整理形成。
'@

Write-IfMissing 'report\sections\testing.tex' @'
\section{软件测试}

本节最终由项目的 Markdown 软件测试文档整理形成。
'@

Write-IfMissing 'report\appendices\appendix.tex' @'
\section{附录}

本节最终加入课程要求的证书、补充材料及相关附件。
'@

# ============================================================
# 15. 为 Git 保留空目录
#
# Git 不跟踪空目录，因此加入 .gitkeep。
# build/ 除外，因为它是编译输出目录，应被 .gitignore 忽略。
# ============================================================

Write-Host ''
Write-Host '[7/12] Adding .gitkeep files...' -ForegroundColor Yellow

$KeepDirectories = @(
    'docs\uml',
    'docs\ui',
    'docs\gantt',

    'report\figures',
    'report\tables',

    'include',

    'src\core',
    'src\filesystem',
    'src\archive',
    'src\compression',
    'src\crypto',
    'src\scheduler',
    'src\watcher',
    'src\network',

    'ui\css',
    'ui\js',

    'tests\unit',
    'tests\integration',
    'tests\system',
    'tests\performance'
)

foreach ($Directory in $KeepDirectories) {
    $KeepFile = Join-Path $ProjectRoot "$Directory\.gitkeep"

    if (-not (Test-Path $KeepFile)) {
        New-Item -ItemType File -Path $KeepFile -Force | Out-Null
    }
}

Write-Host '[OK] Empty Git directories preserved.'

# ============================================================
# 16. 初始化 / 修复 Git
# ============================================================

Write-Host ''
Write-Host '[8/12] Initializing Git...' -ForegroundColor Yellow

if (-not (Test-Path $GitDirectory)) {
    git init
}

git branch -M main

# 检查 Git 身份，否则 git commit 会在最后突然失败。
$GitUserName = git config user.name
$GitUserEmail = git config user.email

if ([string]::IsNullOrWhiteSpace($GitUserName) -or
    [string]::IsNullOrWhiteSpace($GitUserEmail)) {

    Write-Host ''
    Write-Host 'Git 用户身份尚未配置。' -ForegroundColor Red
    Write-Host ''
    Write-Host '请先执行类似：'
    Write-Host ''
    Write-Host 'git config --global user.name "你的名字"'
    Write-Host 'git config --global user.email "你的 GitHub 邮箱"'
    Write-Host ''
    throw 'Git user.name / user.email 未配置。'
}

Write-Host "Git user.name  = $GitUserName"
Write-Host "Git user.email = $GitUserEmail"

# ============================================================
# 17. Git 首次 Commit
# ============================================================

Write-Host ''
Write-Host '[9/12] Creating Git commit...' -ForegroundColor Yellow

git add .

git status --short

$HasHead = $true

git rev-parse --verify HEAD *> $null

if ($LASTEXITCODE -ne 0) {
    $HasHead = $false
}

if (-not $HasHead) {
    git commit -m 'chore: initialize backup project structure'
}
else {
    # 如果此前仓库已经有历史，则只有发生变化才提交。
    $Changed = git status --porcelain

    if ($Changed) {
        git commit -m 'chore: repair project structure'
    }
    else {
        Write-Host '[INFO] Nothing new to commit.'
    }
}

# ============================================================
# 18. 如果要求，删除旧的 GitHub Repository
# ============================================================

Write-Host ''
Write-Host '[10/12] Checking GitHub repository...' -ForegroundColor Yellow

gh repo view $GitHubRepo *> $null
$RemoteExists = ($LASTEXITCODE -eq 0)

if ($RemoteExists) {
    Write-Host "GitHub repository already exists: $GitHubRepo"

    if ($RecreateRemote) {
        Write-Host ''
        Write-Host 'WARNING: The following GitHub repository will be deleted:' -ForegroundColor Red
        Write-Host "https://github.com/$GitHubRepo" -ForegroundColor Red
        Write-Host ''

        $Confirmation = Read-Host '输入 DELETE 确认删除并重新创建'

        if ($Confirmation -ne 'DELETE') {
            throw '用户取消 GitHub Repository 删除操作。'
        }

        gh repo delete $GitHubRepo --yes

        if ($LASTEXITCODE -ne 0) {
            throw @'
删除 GitHub Repository 失败。

如果 gh 提示缺少 delete_repo 权限，请先执行：

gh auth refresh -h github.com -s delete_repo

然后重新运行：

.\repair_project.ps1 -ResetLocalGit -RecreateRemote
'@
        }

        Write-Host '[OK] Old GitHub repository deleted.'

        $RemoteExists = $false
    }
}

# ============================================================
# 19. 创建 / 设置 GitHub Remote
# ============================================================

Write-Host ''
Write-Host '[11/12] Creating/configuring GitHub repository...' -ForegroundColor Yellow

if (-not $RemoteExists) {
    gh repo create $GitHubRepo `
        --public `
        --source . `
        --remote origin

    if ($LASTEXITCODE -ne 0) {
        throw 'GitHub Repository 创建失败。'
    }

    Write-Host '[OK] GitHub repository created.'
}
else {
    $ExpectedRemote = "https://github.com/$GitHubRepo.git"

    $OriginExists = git remote | Where-Object { $_ -eq 'origin' }

    if ($OriginExists) {
        git remote set-url origin $ExpectedRemote
    }
    else {
        git remote add origin $ExpectedRemote
    }

    Write-Host '[OK] Existing GitHub repository will be reused.'
}

# ============================================================
# 20. Push
# ============================================================

Write-Host ''
Write-Host '[12/12] Pushing main branch...' -ForegroundColor Yellow

git push -u origin main

if ($LASTEXITCODE -ne 0) {
    throw @'
Git push 失败。

如果这是因为远端已有与当前本地仓库不同的历史，
而你确认远端就是本次中断产生的无用仓库，请重新运行：

.\repair_project.ps1 -ResetLocalGit -RecreateRemote
'@
}

# ============================================================
# 21. 最终结果
# ============================================================

Write-Host ''
Write-Host '============================================================' -ForegroundColor Green
Write-Host ' Backup Project initialization completed.' -ForegroundColor Green
Write-Host '============================================================' -ForegroundColor Green
Write-Host ''
Write-Host "Local project:"
Write-Host "  $ProjectRoot"
Write-Host ''
Write-Host 'GitHub:'
Write-Host "  https://github.com/$GitHubRepo"
Write-Host ''
Write-Host 'Current branch:'

git branch --show-current

Write-Host ''
Write-Host 'Remote:'

git remote -v

Write-Host ''
Write-Host 'Git status:'

git status

Write-Host ''
Write-Host 'Project tree:'
Write-Host ''

Get-ChildItem `
    -Force `
    -Recurse `
    -Depth 2 |
    Select-Object FullName

Write-Host ''
Write-Host '下一步在 Ubuntu VM 中执行：' -ForegroundColor Cyan
Write-Host ''
Write-Host '  cd ~'
Write-Host '  git clone https://github.com/PlyusWang/backup-project.git'
Write-Host '  cd backup-project'
Write-Host '  chmod +x scripts/*.sh'
Write-Host '  ./scripts/build.sh'
Write-Host '  ./scripts/test.sh'
Write-Host ''