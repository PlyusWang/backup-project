// ui/modern/user_error.h
//
// 把共享核心的英文错误翻译成“标题 / 简短原因 / 下一步建议”。
//
// 三条边界：
//   * 技术原文由调用方保留为脱敏诊断，不在普通错误对话框展示；
//   * 翻译只做“归类 + 一句人话”，不假装知道核心没说的东西；
//   * 无法归类时给中性结论与可执行的下一步，绝不照搬英文到主界面。

#ifndef BACKUP_PROJECT_UI_MODERN_USER_ERROR_H_
#define BACKUP_PROJECT_UI_MODERN_USER_ERROR_H_

#include <QString>

namespace backup_modern {

struct UserFacingError {
  QString title;
  QString summary;
  QString suggestion;
};

// action：用户做的事（“备份”“恢复”“上传”“登录”），用于标题。
UserFacingError ExplainOperationError(const QString& technical,
                                      const QString& action);

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_USER_ERROR_H_
