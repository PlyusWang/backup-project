// dev_harnesses.h
//
// 开发与验收 harness 的对外接口。
//
// 这些函数只服务于 main() 的命令行开关（--smoke-test / --screenshot /
// --self-test / --remote-acceptance
// 等），产品的正常启动路径一条都不会调到它们。 实现在
// dev_harnesses.cpp，与应用入口分开编译。

#pragma once

#include <qqml.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMap>
#include <QMetaObject>
#include <QPointF>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>

#include "app_paths.h"
#include "app_theme.h"
#include "application_instance_lock.h"
#include "archive_pipeline.h"
#include "backup_controller.h"
#include "config_manager.h"
#include "filter_rule_model.h"
#include "incremental_delta.h"
#include "operation_gate.h"
#include "realtime_controller.h"
#include "remote_controller.h"
#include "remote_incremental.h"
#include "schedule_controller.h"
#include "schedule_frequency.h"
#include "schedule_store.h"
#include "scheduler_lock.h"
#include "server_profile.h"

const int kPageCount = 7;

void WaitForAnimation(int milliseconds);
int CaptureScreenshots(QQuickWindow* window, backup_modern::AppTheme* theme,
                       backup_modern::BackupController* controller,
                       backup_modern::RemoteController* remote,
                       const QString& directory);
int RunPreviewTest(backup_modern::FilterRuleModel* model, const QString& source,
                   const QStringList& arguments);
int RunSelfTest(backup_modern::BackupController* controller,
                const QString& source, const QString& archive_file,
                const QString& destination);
int RunRepositoryTest(backup_modern::BackupController* controller,
                      const QString& source, const QString& repository,
                      const QString& destination);
int RunPathTest(backup_modern::BackupController* controller);
int RunCloseGuardTest(QQuickWindow* window,
                      backup_modern::BackupController* controller,
                      backup_modern::ScheduleController* schedule,
                      backup_modern::RealtimeController* realtime);
int RunComboHoverTest(QQuickWindow* window, backup_modern::AppTheme* theme);
int RunFilterUxTest(QQuickWindow* window,
                    backup_modern::FilterRuleModel* manual_model,
                    backup_modern::FilterRuleModel* schedule_model,
                    backup_modern::FilterRuleModel* realtime_model);
int RunRemoteAcceptance(QQuickWindow* window,
                        backup_modern::RemoteController* remote,
                        backup_modern::BackupController* controller,
                        backup_modern::AppTheme* theme, const QString& out_dir,
                        const QString& host, const QString& port_text,
                        const QString& username);
int RunRemoteSmoke(backup_modern::RemoteController* remote,
                   backup_modern::BackupController* controller,
                   const QString& host, const QString& port_text,
                   const QString& username, const QString& password);
int RunOfficialAcceptance(QQuickWindow* window,
                          backup_modern::RemoteController* remote,
                          const QString& username, const QString& out_png);
int RunRemoteScreenshot(QQuickWindow* window,
                        backup_modern::RemoteController* remote,
                        backup_modern::AppTheme* theme,
                        const QString& directory);
int RunRemoteTest(QQuickWindow* window, backup_modern::RemoteController* remote,
                  backup_modern::BackupController* controller,
                  backup_modern::AppTheme* theme,
                  const QString& config_file_path,
                  const QString& schedule_file_path,
                  const QString& realtime_file_path);
int RunGuiContractTest(QQuickWindow* window,
                       backup_modern::BackupController* controller);
int RunIncrementalTest(backup_modern::BackupController* controller,
                       const QString& source, const QString& repository);
int RunBackupOptionsTest(backup_modern::BackupController* controller,
                         const QString& config_file_path);
int RunScheduleShow(backup_modern::ScheduleController* schedule);
int RunRealtimeShow(backup_modern::RealtimeController* realtime);
int RunRealtimeTest(backup_modern::RealtimeController* realtime,
                    backup_modern::OperationGate* gate,
                    backup_modern::BackupController* controller,
                    const QString& config_path);
int RunScheduleTest(backup_modern::ScheduleController* schedule,
                    backup_modern::BackupController* backup_controller,
                    const QString& config_path);
