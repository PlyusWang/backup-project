// workflow_acceptance_test.h

#ifndef BACKUP_PROJECT_UI_MODERN_WORKFLOW_ACCEPTANCE_TEST_H_
#define BACKUP_PROJECT_UI_MODERN_WORKFLOW_ACCEPTANCE_TEST_H_

#include <QString>

class QQuickWindow;
namespace backup_modern {
class BackupController;
class ServerIdentityImport;
}  // namespace backup_modern

int RunWorkflowAcceptanceTest(QQuickWindow* window,
                              backup_modern::BackupController* controller,
                              backup_modern::ServerIdentityImport* identity,
                              const QString& output);

#endif  // BACKUP_PROJECT_UI_MODERN_WORKFLOW_ACCEPTANCE_TEST_H_
