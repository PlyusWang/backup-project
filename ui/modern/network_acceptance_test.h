#ifndef BACKUP_PROJECT_UI_MODERN_NETWORK_ACCEPTANCE_TEST_H_
#define BACKUP_PROJECT_UI_MODERN_NETWORK_ACCEPTANCE_TEST_H_

#include <QString>

class QQuickWindow;
namespace backup_modern {
class RemoteController;
}

int RunNetworkAcceptanceTest(QQuickWindow* window,
                             backup_modern::RemoteController* remote,
                             const QString& fixture_directory,
                             const QString& output_directory);

#endif
