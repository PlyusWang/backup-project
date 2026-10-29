// ui/modern/user_error.cpp

#include "user_error.h"

namespace backup_modern {
namespace {

bool Contains(const QString& text, const char* needle) {
  return text.contains(QString::fromLatin1(needle), Qt::CaseInsensitive);
}

}  // namespace

UserFacingError ExplainOperationError(const QString& technical,
                                      const QString& action) {
  UserFacingError error;
  error.title = action + QStringLiteral("没有完成");

  if (Contains(technical, "not empty")) {
    error.summary =
        QStringLiteral("目标文件夹中已有文件，请选择一个空文件夹。");
    error.suggestion = QStringLiteral("选择一个新的空文件夹后重试。");
    return error;
  }
  if (Contains(technical, "no snapshot with a usable content manifest") ||
      Contains(technical, "no usable baseline")) {
    error.summary = QStringLiteral("暂时没有可用的完整备份。");
    error.suggestion = QStringLiteral("先创建一份完整备份，再执行增量或恢复。");
    return error;
  }
  if (Contains(technical, "No space left") || Contains(technical, "空间")) {
    error.summary = QStringLiteral("磁盘空间不足，操作已停止。");
    error.suggestion =
        QStringLiteral("清理目标磁盘或换一个容量充足的位置后重试。");
    return error;
  }
  if (Contains(technical, "Permission denied") ||
      Contains(technical, "Access denied")) {
    error.summary = QStringLiteral("没有访问该文件或目录的权限。");
    error.suggestion = QStringLiteral("检查路径权限，或改用有权限的目录。");
    return error;
  }
  if (Contains(technical, "already exists") ||
      Contains(technical, "ALREADY_EXISTS")) {
    error.summary = QStringLiteral("同名文件已经存在，未覆盖任何内容。");
    error.suggestion =
        QStringLiteral("换一个名称，或先确认已有文件是否需要保留。");
    return error;
  }
  if (Contains(technical, "UNAUTHORIZED") ||
      Contains(technical, "unauthorized")) {
    error.summary = QStringLiteral("用户名或密码不正确，尚未登录。");
    error.suggestion =
        QStringLiteral("检查账号信息；连续失败会触发登录节流，请稍后再试。");
    return error;
  }
  if (Contains(technical, "password") || Contains(technical, "口令")) {
    error.summary = QStringLiteral("密码不正确，或这份备份需要密码。");
    error.suggestion = QStringLiteral("重新输入正确的密码后再试一次。");
    return error;
  }
  if (Contains(technical, "authentication") || Contains(technical, "HMAC")) {
    error.summary =
        QStringLiteral("备份认证未通过，密码可能不正确或文件已损坏。");
    error.suggestion = QStringLiteral("确认原密码，并检查备份文件是否完整。");
    return error;
  }
  if (Contains(technical, "chain") || Contains(technical, "parent") ||
      Contains(technical, "依赖")) {
    error.summary = QStringLiteral("备份的依赖链不完整，无法安全恢复。");
    error.suggestion =
        QStringLiteral("保留同一链上的全部备份，或重新创建一份完整备份。");
    return error;
  }
  if (Contains(technical, "does not exist") ||
      Contains(technical, "No such file")) {
    error.summary = QStringLiteral("指定的文件或文件夹不存在。");
    error.suggestion = QStringLiteral("确认路径仍然存在，然后重新选择。");
    return error;
  }
  if (Contains(technical, "connection") || Contains(technical, "connect") ||
      Contains(technical, "timed out") || Contains(technical, "断")) {
    error.summary = QStringLiteral("无法连接到服务器。");
    error.suggestion = QStringLiteral("检查服务器地址、网络和登录状态后重试。");
    return error;
  }

  error.summary = QStringLiteral("操作未能完成，已有数据没有被覆盖。");
  error.suggestion = QStringLiteral(
      "请检查路径、权限与可用空间后重试；"
      "具体原因可在详情中查看。");
  return error;
}

}  // namespace backup_modern
