#include "combo_selection.h"

#include <QVariant>

namespace backup_modern {

bool ComboSelection::setCurrentIndex(QObject* combo, int index) const {
  if (combo == nullptr || index < 0 ||
      index >= combo->property("count").toInt())
    return false;
  // QML 内的直接赋值会移除绑定；QObject 的属性写入只调用控件 setter。
  // 不通过逐项递增选择，避免跳选时向页面发出多次 activated。
  return combo->setProperty("currentIndex", index) &&
         combo->property("currentIndex").toInt() == index;
}

}  // namespace backup_modern
