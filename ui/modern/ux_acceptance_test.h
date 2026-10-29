// 真实控件事件的产品交互回归；仅由显式自检入口调用。
#pragma once

class QQuickWindow;
namespace backup_modern {
class AppTheme;
class ScheduleController;
class RealtimeController;
}  // namespace backup_modern

int RunUxAcceptanceTest(QQuickWindow* window, backup_modern::AppTheme* theme,
                        backup_modern::ScheduleController* schedule,
                        backup_modern::RealtimeController* realtime);
