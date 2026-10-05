#!/bin/sh
# /usr/bin/backupctl —— 客户端命令行。与 GUI 共用同一份核心实现。
exec /usr/lib/backup-project-client/bin/backupctl "$@"
