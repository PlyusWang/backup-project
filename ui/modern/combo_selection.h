#ifndef BACKUP_PROJECT_UI_MODERN_COMBO_SELECTION_H_
#define BACKUP_PROJECT_UI_MODERN_COMBO_SELECTION_H_

#include <QObject>

namespace backup_modern {

// 使用 Qt 的 C++ 属性入口提交选项，保留调用页安装的 currentIndex 绑定。
class ComboSelection : public QObject {
  Q_OBJECT

 public:
  explicit ComboSelection(QObject* parent = nullptr) : QObject(parent) {}
  Q_INVOKABLE bool setCurrentIndex(QObject* combo, int index) const;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_COMBO_SELECTION_H_
