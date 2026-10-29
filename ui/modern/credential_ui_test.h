// credential_ui_test.h

#ifndef BACKUP_PROJECT_UI_MODERN_CREDENTIAL_UI_TEST_H_
#define BACKUP_PROJECT_UI_MODERN_CREDENTIAL_UI_TEST_H_

#include <QString>

class QQuickWindow;
namespace backup_modern {
class BackupController;
class RealtimeController;
class RemoteController;
class ScheduleController;
}  // namespace backup_modern

int RunCredentialUiTest(QQuickWindow* window,
                        backup_modern::ScheduleController* schedule,
                        backup_modern::RealtimeController* realtime,
                        backup_modern::RemoteController* remote,
                        backup_modern::BackupController* controller,
                        const QString& output);

#endif  // BACKUP_PROJECT_UI_MODERN_CREDENTIAL_UI_TEST_H_
