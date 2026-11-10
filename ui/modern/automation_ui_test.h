// automation_ui_test.h
#ifndef BACKUP_PROJECT_UI_MODERN_AUTOMATION_UI_TEST_H_
#define BACKUP_PROJECT_UI_MODERN_AUTOMATION_UI_TEST_H_

#include <QString>

class QQuickWindow;
namespace backup_modern {
class BackupController;
class RealtimeController;
class ScheduleController;
}  // namespace backup_modern

int RunAutomationUiTest(QQuickWindow* window,
                        backup_modern::BackupController* controller,
                        backup_modern::ScheduleController* schedule,
                        backup_modern::RealtimeController* realtime,
                        const QString& output);

#endif  // BACKUP_PROJECT_UI_MODERN_AUTOMATION_UI_TEST_H_
