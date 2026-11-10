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

  // 远程控制器已经把协议失败归类为稳定的 error_kind；优先按分类翻译，
  // 不再依赖底层英文详情里是否恰好包含某个单词。
  if (technical == QStringLiteral("credentials")) {
    error.title = QStringLiteral("登录失败");
    error.summary = QStringLiteral("用户名或密码不正确。");
    error.suggestion = QStringLiteral("请检查账号信息后重新登录。");
    return error;
  }
  if (technical == QStringLiteral("pin-mismatch")) {
    error.summary = QStringLiteral("服务器身份指纹与当前服务器不一致。");
    error.suggestion = QStringLiteral("请向服务器管理员核对指纹后重试。");
    return error;
  }
  if (technical == QStringLiteral("connect-refused")) {
    error.summary = QStringLiteral("目标服务器没有接受连接。");
    error.suggestion = QStringLiteral("请确认服务器已启动，地址和端口填写正确。");
    return error;
  }
  if (technical == QStringLiteral("network")) {
    error.summary = QStringLiteral("连接在操作完成前中断。");
    error.suggestion = QStringLiteral("请检查网络后重试；已有数据不会被覆盖。");
    return error;
  }
  if (technical == QStringLiteral("not-logged-in")) {
    error.summary = QStringLiteral("登录状态已失效。");
    error.suggestion = QStringLiteral("请重新登录后再试。");
    return error;
  }
  if (technical == QStringLiteral("raw-auth")) {
    error.title = action + QStringLiteral("失败");
    error.summary = QStringLiteral("归档密码错误，或备份完整性校验失败。");
    error.suggestion = QStringLiteral("请使用创建备份时的密码，并确认归档文件完整。");
    return error;
  }
  if (technical == QStringLiteral("chain-conflict")) {
    error.title = QStringLiteral("增量备份未能继续");
    error.summary = QStringLiteral("所选基线与当前增量链不兼容。");
    error.suggestion = QStringLiteral("请使用可验证的同一条链，或明确创建新的完整备份。");
    return error;
  }

  if (Contains(technical, "server-certificate-wrong-server-id")) {
    error.summary = QStringLiteral("服务器身份与预期不一致，连接已停止。");
    error.suggestion = QStringLiteral(
        "请向服务器管理员核对服务器身份文件和预期的服务器标识后重试。");
    return error;
  }
  if (Contains(technical, "server-certificate-untrusted")) {
    error.summary =
        QStringLiteral("服务器证书的签发者不在可信根列表中，连接已停止。");
    error.suggestion = QStringLiteral(
        "请向服务器管理员确认可信根文件，并从可信渠道导入后重试。");
    return error;
  }
  if (Contains(technical, "server-certificate-expired")) {
    error.summary =
        QStringLiteral("服务器身份证书已过期或尚未生效，连接已停止。");
    error.suggestion = QStringLiteral(
        "请检查本机时间；时间正确时，请向服务器管理员索取有效的身份证书。");
    return error;
  }
  if (Contains(technical, "server-certificate-key-mismatch")) {
    error.summary =
        QStringLiteral("服务器证书与实际使用的身份密钥不一致，连接已停止。");
    error.suggestion = QStringLiteral(
        "请向服务器管理员核对服务器的身份证书和密钥配置后重试。");
    return error;
  }
  if (Contains(technical, "server-certificate-missing")) {
    error.summary = QStringLiteral("服务器没有提供身份证书，连接已停止。");
    error.suggestion =
        QStringLiteral("请向服务器管理员确认地址、端口和证书配置后重试。");
    return error;
  }
  if (Contains(technical, "server-certificate-invalid")) {
    error.summary = QStringLiteral("服务器身份证书无效，连接已停止。");
    error.suggestion =
        QStringLiteral("请向服务器管理员核对身份证书和可信根文件后重试。");
    return error;
  }
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
      "必要的技术诊断已写入本机日志。");
  return error;
}

}  // namespace backup_modern
