// tests/unit/ssh_tunnel_manager_test.cpp
//
// SshTunnelManager 的独立单元测试（PR #22）。
//
// 它**不依赖 ECS、不依赖网络、不依赖 22 端口上的任何真实主机**，因此可以进
// final gate：一个"每次都跑、每次都判"的套件，而不是"网络好的时候才跑"。
//
// 怎么做到既离线又真实：
//
//   * 状态机用**替身 ssh**驱动 —— 测试在临时目录里写一个可执行脚本，脚本把
//     自己收到的**参数向量**逐条记进日志，然后按剧本行事（绑定 -L 里的本地
//     端口并常驻 / 立刻退出并打印某一句 stderr / 干脆挂住 / 忽略 SIGTERM）。
//     替身只替换"ssh 这个可执行文件"，管理器本身的进程管理、就绪判定、
//     超时、回收、分类全部是真的。
//   * 参数向量的安全性质因此可以被**直接断言**：替身记下来的 argv 里不允许
//     出现 StrictHostKeyChecking / UserKnownHostsFile / /dev/null，不允许把
//     目标拆成多个参数，不允许被 shell 展开（注入用例里带 \$() 与 ; 的
//     目标必须原样出现在一个参数里，且副作用文件不存在）。
//
// 编号（与交接文档 05-PROCESS-LIFECYCLE.md 一致）：
//   SSH-U01  PickFreeLoopbackPort / IsLoopbackPortOpen
//   SSH-U02  IsValidSshTarget 拒绝危险与畸形写法
//   SSH-U03  SanitizeSshStderr：ANSI / 控制字符 / 口令脱敏 / 长度有界
//   SSH-U04  ClassifySshStderr：四类失败各自归类，不互相吞并
//   SSH-U05  参数向量：没有 shell、没有弱化 host verification 的开关
//   SSH-U06  目标里的 shell 元字符原样传递、不被展开
//   SSH-U07  Starting -> Ready 必须真的连得上，而不是"进程还活着"
//   SSH-U08  超时：挂住的 ssh 在有界时间内被判定失败并被回收
//   SSH-U09  Ready 之后进程退出 -> kExited
//   SSH-U10  ssh 不存在 / 目标不合法
//   SSH-U11  四类 ssh 失败经真实进程退出被正确分类
//   SSH-U12  自动端口撞车的有界重试
//   SSH-U13  快速 start/stop 循环不留僵尸、不留孤儿
//   SSH-U14  正在建立时重复 Start 被拒（不会起第二个进程）
//   SSH-U15  外部监听者只被借用，绝不被杀
//   SSH-U16  建立过程中析构：不留下孤儿 ssh
//   SSH-U17  大量 stderr 有界且可安全进界面
//   SSH-U18  忽略 SIGTERM 的子进程最终仍被回收
//
// 退出码 0 = 全部通过。

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QList>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <functional>
#include <initializer_list>

#include "ssh_tunnel_manager.h"

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const QString& label, const QString& detail = QString()) {
  if (ok) {
    ++g_passed;
    std::printf("[ssh-tunnel]   ok   %s\n", qPrintable(label));
    return;
  }
  ++g_failed;
  if (detail.isEmpty()) {
    std::printf("[ssh-tunnel]   FAIL %s\n", qPrintable(label));
    return;
  }
  std::printf("[ssh-tunnel]   FAIL %s（%s）\n", qPrintable(label),
              qPrintable(detail));
}

// 有界等待：既等信号，也等 deleteLater() 这类"下一轮事件循环"的工作。
bool WaitFor(const std::function<bool()>& predicate, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < timeout_ms) {
    if (predicate()) {
      return true;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return predicate();
}

bool PathExists(const QString& path) { return QFileInfo::exists(path); }

bool ProcessAlive(qint64 pid) {
  if (pid <= 0) {
    return false;
  }
  return PathExists(QStringLiteral("/proc/%1").arg(pid));
}

// 本进程的僵尸子进程数（直接读 /proc，不依赖 ps）。
int ZombieChildCount() {
  QDir proc(QStringLiteral("/proc"));
  const QStringList entries =
      proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
  const qint64 self = static_cast<qint64>(QCoreApplication::applicationPid());
  int zombies = 0;
  for (const QString& entry : entries) {
    bool ok = false;
    entry.toLongLong(&ok);
    if (!ok) {
      continue;
    }
    QFile stat(QStringLiteral("/proc/%1/stat").arg(entry));
    if (!stat.open(QIODevice::ReadOnly)) {
      continue;
    }
    const QByteArray line = stat.readLine();
    // 格式：pid (comm) state ppid ...
    const int close = line.lastIndexOf(')');
    if (close < 0 || close + 3 >= line.size()) {
      continue;
    }
    const QList<QByteArray> rest = line.mid(close + 2).simplified().split(' ');
    if (rest.size() < 2) {
      continue;
    }
    const char state = rest.at(0).at(0);
    const qint64 ppid = rest.at(1).toLongLong();
    if (ppid == self && state == 'Z') {
      ++zombies;
    }
  }
  return zombies;
}

bool WriteExecutableScript(const QString& path, const QString& body) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  file.write(body.toUtf8());
  file.close();
  return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                             QFileDevice::ExeGroup | QFileDevice::ReadOther |
                             QFileDevice::ExeOther);
}

// 替身 ssh 的公共前导：把自己的参数向量逐条记进日志，再从 -L 里取出本地端口。
// 刻意用 POSIX sh 写，且**不做任何拼接求值** —— 记下来的是什么就是什么。
QString FakePreamble(const QString& log_path) {
  return QStringLiteral(R"SH(#!/bin/sh
LOG='%1'
for a in "$@"; do
  printf 'ARG:%s\n' "$a" >> "$LOG"
done
L=""
prev=""
for a in "$@"; do
  if [ "$prev" = "-L" ]; then L="$a"; fi
  prev="$a"
done
PORT=$(printf '%s' "$L" | cut -d: -f2)
printf 'PORT:%s\n' "$PORT" >> "$LOG"
)SH")
      .arg(log_path);
}

// 绑定本地端口并常驻：模拟"隧道真的建起来了"。
QString BindAndSleepBlock() {
  return QStringLiteral(R"SH(python3 -c 'import socket,sys,time
s=socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(16)
time.sleep(3600)' "$PORT"
)SH");
}

QString ArgvFromLog(const QString& log_path) {
  QFile file(log_path);
  if (!file.open(QIODevice::ReadOnly)) {
    return QString();
  }
  return QString::fromUtf8(file.readAll());
}

QStringList ArgsFromLog(const QString& log_path) {
  QStringList args;
  const QStringList lines = ArgvFromLog(log_path).split(QLatin1Char('\n'));
  for (const QString& line : lines) {
    if (line.startsWith(QLatin1String("ARG:"))) {
      args.append(line.mid(4));
    }
  }
  return args;
}

QString PortFromLog(const QString& log_path) {
  const QStringList lines = ArgvFromLog(log_path).split(QLatin1Char('\n'));
  for (const QString& line : lines) {
    if (line.startsWith(QLatin1String("PORT:"))) {
      return line.mid(5).trimmed();
    }
  }
  return QString();
}

bool HasArgContaining(const QStringList& args, const QString& needle) {
  for (const QString& arg : args) {
    if (arg.contains(needle)) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  using backup_modern::SshTunnelManager;
  using Failure = SshTunnelManager::Failure;
  using State = SshTunnelManager::State;

  QTemporaryDir temp;
  if (!temp.isValid()) {
    std::fprintf(stderr, "[ssh-tunnel] 无法创建临时目录\n");
    return 1;
  }
  const QString dir = temp.path();
  const QString python =
      QStandardPaths::findExecutable(QStringLiteral("python3"));
  if (python.isEmpty()) {
    std::fprintf(stderr, "[ssh-tunnel] 需要 python3 来写替身 ssh\n");
    return 1;
  }
  const auto scenario = [&dir](const QString& name, const QString& log_name,
                               const QString& body) {
    const QString log = dir + QLatin1Char('/') + log_name;
    const QString path = dir + QLatin1Char('/') + name;
    if (!WriteExecutableScript(path, FakePreamble(log) + body)) {
      std::fprintf(stderr, "[ssh-tunnel] 无法写替身脚本 %s\n", qPrintable(path));
      return QString();
    }
    return path;
  };

  // ---- SSH-U01: 端口工具 ----
  {
    int port = 0;
    QString error;
    Check(SshTunnelManager::PickFreeLoopbackPort(&port, &error) && port >= 1024 &&
              port <= 65535,
          QStringLiteral("SSH-U01a 自动挑到一个合法回环端口"),
          QStringLiteral("port=%1 error=%2").arg(port).arg(error));
    QString open_error;
    Check(!SshTunnelManager::IsLoopbackPortOpen(port, 200, &open_error),
          QStringLiteral("SSH-U01b 刚关掉的端口连不上（没有假阳性）"),
          open_error);
    QTcpServer listener;
    Check(listener.listen(QHostAddress::LocalHost, 0),
          QStringLiteral("SSH-U01c 测试用的本地监听者起得来"));
    const int listening_port = listener.serverPort();
    QString ok_error;
    Check(SshTunnelManager::IsLoopbackPortOpen(listening_port, 500, &ok_error),
          QStringLiteral("SSH-U01d 有监听者的端口连得上"), ok_error);
    Check(!SshTunnelManager::PickFreeLoopbackPort(nullptr, nullptr),
          QStringLiteral("SSH-U01e 空输出指针被拒"));
    listener.close();
  }

  // ---- SSH-U02: 目标校验 ----
  {
    QString error;
    Check(!SshTunnelManager::IsValidSshTarget(QString(), &error),
          QStringLiteral("SSH-U02a 空目标被拒"), error);
    Check(!SshTunnelManager::IsValidSshTarget(
              QStringLiteral("-oProxyCommand=curl evil"), &error),
          QStringLiteral("SSH-U02b '-' 开头的目标被拒（参数注入）"), error);
    Check(!SshTunnelManager::IsValidSshTarget(QStringLiteral("a b"), &error),
          QStringLiteral("SSH-U02c 带空格的目标被拒"), error);
    Check(!SshTunnelManager::IsValidSshTarget(QStringLiteral(" a"), &error),
          QStringLiteral("SSH-U02d 首尾空白被拒"), error);
    Check(!SshTunnelManager::IsValidSshTarget(QStringLiteral("a\nb"), &error),
          QStringLiteral("SSH-U02e 控制字符被拒"), error);
    Check(SshTunnelManager::IsValidSshTarget(QStringLiteral("aliyun-ecs"),
                                             &error),
          QStringLiteral("SSH-U02f ssh config 别名被接受"), error);
    Check(SshTunnelManager::IsValidSshTarget(QStringLiteral("ubuntu@8.130.9.200"),
                                             &error),
          QStringLiteral("SSH-U02g user@host 被接受"), error);
  }

  // ---- SSH-U03: stderr 脱敏 ----
  {
    QByteArray raw = "\x1b[31mError\x1b[0m: bad \x07thing\r\npassword: hunter2\n";
    const QString cleaned = SshTunnelManager::SanitizeSshStderr(raw);
    Check(!cleaned.contains(QLatin1Char('\x1b')) &&
              !cleaned.contains(QLatin1Char('\x07')) &&
              !cleaned.contains(QLatin1Char('\r')),
          QStringLiteral("SSH-U03a ANSI 与控制字符被清掉"), cleaned);
    Check(!cleaned.contains(QStringLiteral("hunter2")) &&
              cleaned.contains(QStringLiteral("<redacted>")),
          QStringLiteral("SSH-U03b 口令字面量被脱敏"), cleaned);
    QByteArray huge(2 * 1024 * 1024, 'x');
    huge.append("\nlast-line\n");
    const QString bounded = SshTunnelManager::SanitizeSshStderr(huge);
    Check(bounded.size() < 3000 && bounded.endsWith(QStringLiteral("last-line")),
          QStringLiteral("SSH-U03c 超大 stderr 被截断且保留尾部"),
          QStringLiteral("size=%1").arg(bounded.size()));
  }

  // ---- SSH-U04: 失败分类 ----
  {
    struct Case {
      const char* text;
      Failure expected;
      const char* label;
    };
    const Case cases[] = {
        {"Host key verification failed.", Failure::kHostKeyFailed,
         "host key 失败"},
        {"Permission denied (publickey,password).", Failure::kAuthFailed,
         "认证失败"},
        {"bind [127.0.0.1]:18765: Address already in use", Failure::kForwardFailed,
         "本地端口被占用"},
        {"ssh: Could not resolve hostname nope.invalid: Name or service not known",
         Failure::kHostUnreachable, "主机解析失败"},
        {"ssh: connect to host 10.0.0.1 port 22: Connection refused",
         Failure::kHostUnreachable, "连接被拒绝"},
    };
    for (const Case& item : cases) {
      const Failure got = SshTunnelManager::ClassifySshStderr(
          QString::fromLatin1(item.text));
      Check(got == item.expected,
            QStringLiteral("SSH-U04 %1 -> %2")
                .arg(QString::fromLatin1(item.label),
                     SshTunnelManager::FailureKindName(item.expected)),
            QStringLiteral("实际 %1").arg(SshTunnelManager::FailureKindName(got)));
    }
  }

  // ---- SSH-U05 / U06: 参数向量与注入 ----
  {
    const QString script = scenario(QStringLiteral("ssh-argv.sh"),
                                    QStringLiteral("argv.log"),
                                    BindAndSleepBlock());
    Check(!script.isEmpty(), QStringLiteral("SSH-U05 替身 ssh 写好"));
    // 目标里带 shell 元字符：必须原样进一个参数，且不允许被展开。
    const QString injected = QStringLiteral("host;touch_/tmp/pr22_pwned_$(id)");
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    options.ssh_program = script;
    options.ssh_target = injected;
    options.remote_port = 18765;
    options.ready_timeout_ms = 8000;
    tunnel.Start(options);
    const bool ready = WaitFor(
        [&tunnel] { return tunnel.state() != State::kStarting; }, 9000);
    Check(ready && tunnel.IsReady(),
          QStringLiteral("SSH-U05a 替身 ssh 让通道进入 Ready"),
          QStringLiteral("state=%1 failure=%2")
              .arg(static_cast<int>(tunnel.state()))
              .arg(tunnel.failureKindName()));
    const QStringList args = ArgsFromLog(dir + QStringLiteral("/argv.log"));
    Check(!args.isEmpty(), QStringLiteral("SSH-U05b 替身收到了参数向量"));
    Check(args.contains(QStringLiteral("-N")),
          QStringLiteral("SSH-U05c argv 含 -N"));
    const int dashdash = args.indexOf(QStringLiteral("--"));
    Check(dashdash >= 0 && dashdash + 1 == args.size() - 1 &&
              args.last() == injected,
          QStringLiteral("SSH-U05d 目标原样是**一个**参数，且在 -- 之后"),
          args.join(QLatin1Char('|')));
    bool weakened = false;
    for (const QString& arg : args) {
      if (arg.contains(QStringLiteral("StrictHostKeyChecking")) ||
          arg.contains(QStringLiteral("UserKnownHostsFile")) ||
          arg.contains(QStringLiteral("/dev/null")) ||
          arg.contains(QStringLiteral("GlobalKnownHostsFile"))) {
        weakened = true;
      }
    }
    Check(!weakened, QStringLiteral("SSH-U05e argv 里没有任何弱化 host verification 的开关"),
          args.join(QLatin1Char('|')));
    Check(HasArgContaining(args, QStringLiteral("BatchMode=yes")),
          QStringLiteral("SSH-U05f argv 里 BatchMode=yes（不留下交互式提示）"));
    Check(HasArgContaining(args, QStringLiteral("ExitOnForwardFailure=yes")),
          QStringLiteral("SSH-U05g argv 里 ExitOnForwardFailure=yes"));
    const QString local = PortFromLog(dir + QStringLiteral("/argv.log"));
    Check(local.toInt() > 1024 && tunnel.localPort() == local.toInt(),
          QStringLiteral("SSH-U05h -L 用的就是自动挑的那个端口"),
          QStringLiteral("log=%1 tunnel=%2")
              .arg(local)
              .arg(tunnel.localPort()));
    // 注入的副作用文件必须不存在（没有 shell，就没有展开）。
    Check(!PathExists(QStringLiteral("/tmp/pr22_pwned_")),
          QStringLiteral("SSH-U06 shell 元字符没有被展开（无副作用文件）"));
    const qint64 pid = tunnel.processId();
    tunnel.Stop();
    Check(WaitFor([pid] { return !ProcessAlive(pid); }, 5000),
          QStringLiteral("SSH-U06b Stop 之后自有进程消失"), QString::number(pid));
  }

  // ---- SSH-U07: "进程活着"不等于 Ready ----
  {
    // 替身只挂住、不绑定端口：进程一直在，但端口永远连不上。
    const QString script = scenario(QStringLiteral("ssh-hang.sh"),
                                    QStringLiteral("hang.log"),
                                    QStringLiteral("sleep 3600\n"));
    Check(!script.isEmpty(), QStringLiteral("SSH-U07 挂住的替身写好"));
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    options.ssh_program = script;
    options.ssh_target = QStringLiteral("fake-hang");
    options.local_port = 0;
    options.ready_timeout_ms = 1500;
    tunnel.Start(options);
    const bool settled = WaitFor(
        [&tunnel] { return tunnel.state() == State::kFailed; }, 6000);
    Check(settled, QStringLiteral("SSH-U07a 挂住的 ssh 在有界时间内判失败"),
          QStringLiteral("state=%1").arg(static_cast<int>(tunnel.state())));
    Check(tunnel.failure() == Failure::kTimeout,
          QStringLiteral("SSH-U07b 失败原因是超时"),
          QStringLiteral("failure=%1 diagnostic=%2")
              .arg(tunnel.failureKindName())
              .arg(tunnel.diagnosticText().left(200)));
    Check(tunnel.processId() == 0 && !tunnel.OwnsProcess(),
          QStringLiteral("SSH-U07c 超时之后不再持有进程"));
  }

  // ---- SSH-U08/U09: Ready 之后进程退出 ----
  {
    // 先绑定端口（于是真的会 Ready），2 秒后自己退出：模拟"隧道在使用中
    // 断掉"。注意不能在 BindAndSleepBlock() 后面接 sleep —— 那一段的 python
    // 是前台常驻的，后面的行永远不会执行（这个坑第一版就踩过）。
    const QString script = scenario(
        QStringLiteral("ssh-die.sh"), QStringLiteral("die.log"),
        QStringLiteral("python3 -c 'import socket,sys,time\n"
                       "s=socket.socket()\n"
                       "s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
                       "s.bind((\"127.0.0.1\", int(sys.argv[1])))\n"
                       "s.listen(16)\n"
                       "time.sleep(2)' \"$PORT\"\n"
                       "exit 255\n"));
    Check(!script.isEmpty(), QStringLiteral("SSH-U08 会自己退出的替身写好"));
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    options.ssh_program = script;
    options.ssh_target = QStringLiteral("fake-die");
    options.ready_timeout_ms = 8000;
    tunnel.Start(options);
    const bool became_ready =
        WaitFor([&tunnel] { return tunnel.IsReady(); }, 9000);
    Check(became_ready, QStringLiteral("SSH-U08a 先进入 Ready"),
          QStringLiteral("state=%1 failure=%2 diagnostic=%3")
              .arg(static_cast<int>(tunnel.state()))
              .arg(tunnel.failureKindName())
              .arg(tunnel.diagnosticText().left(200)));
    const bool died = WaitFor(
        [&tunnel] { return tunnel.state() == State::kFailed; }, 12000);
    Check(died && tunnel.failure() == Failure::kExited,
          QStringLiteral("SSH-U09 Ready 之后进程退出 -> kExited"),
          QStringLiteral("state=%1 failure=%2")
              .arg(static_cast<int>(tunnel.state()))
              .arg(tunnel.failureKindName()));
  }

  // ---- SSH-U10: ssh 不存在 / 目标不合法 ----
  {
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    options.ssh_program = dir + QStringLiteral("/no-such-ssh-binary");
    options.ssh_target = QStringLiteral("whatever");
    tunnel.Start(options);
    Check(tunnel.state() == State::kFailed &&
              tunnel.failure() == Failure::kSshMissing,
          QStringLiteral("SSH-U10a 找不到 ssh -> kSshMissing"),
          tunnel.failureKindName());
    SshTunnelManager tunnel2;
    SshTunnelManager::Options bad;
    bad.ssh_target = QStringLiteral("-oProxyCommand=evil");
    tunnel2.Start(bad);
    Check(tunnel2.state() == State::kFailed &&
              tunnel2.failure() == Failure::kInvalidTarget,
          QStringLiteral("SSH-U10b 目标以 '-' 开头 -> kInvalidTarget"),
          tunnel2.failureKindName());
  }

  // ---- SSH-U11: 四类失败经真实进程退出被分类 ----
  {
    struct Case {
      const char* script_name;
      const char* stderr_text;
      Failure expected;
      const char* label;
    };
    const Case cases[] = {
        {"ssh-hostkey.sh", "Host key verification failed.\n",
         Failure::kHostKeyFailed, "host key"},
        {"ssh-auth.sh", "Permission denied (publickey,password).\n",
         Failure::kAuthFailed, "auth"},
        {"ssh-unreach.sh",
         "ssh: Could not resolve hostname nope.invalid: Name or service not known\n",
         Failure::kHostUnreachable, "unreachable"},
        {"ssh-forward.sh",
         "bind [127.0.0.1]:18765: Address already in use\n"
         "channel_setup_fwd_listener_tcpip: cannot listen to port: 18765\n"
         "Could not request local forwarding.\n",
         Failure::kForwardFailed, "forward"},
    };
    for (const Case& item : cases) {
      const QString name = QString::fromLatin1(item.script_name);
      const QString script = scenario(
          name, name + QStringLiteral(".log"),
          QStringLiteral("printf '%s' '") +
              QString::fromLatin1(item.stderr_text).replace(
                  QStringLiteral("'"), QStringLiteral("'\\''")) +
              QStringLiteral("' >&2\nexit 255\n"));
      if (script.isEmpty()) {
        Check(false, QStringLiteral("SSH-U11 %1 替身写好")
                         .arg(QString::fromLatin1(item.label)));
        continue;
      }
      SshTunnelManager tunnel;
      SshTunnelManager::Options options;
      options.ssh_program = script;
      options.ssh_target = QStringLiteral("fake");
      // 自动端口：即使触发"端口撞车"的有界重试，最终分类也必须是原始的
      // 那一个（重试不改变结论，只改变尝试次数）。
      options.local_port = 0;
      options.ready_timeout_ms = 4000;
      tunnel.Start(options);
      const bool settled = WaitFor(
          [&tunnel] { return tunnel.state() == State::kFailed; }, 15000);
      Check(settled && tunnel.failure() == item.expected,
            QStringLiteral("SSH-U11 %1 -> %2")
                .arg(QString::fromLatin1(item.label),
                     SshTunnelManager::FailureKindName(item.expected)),
            QStringLiteral("state=%1 failure=%2 diagnostic=%3")
                .arg(static_cast<int>(tunnel.state()))
                .arg(tunnel.failureKindName())
                .arg(tunnel.diagnosticText().left(80)));
    }
  }

  // ---- SSH-U12: 自动端口撞车的重试是**有界**的 ----
  {
    const QString name = QStringLiteral("ssh-always-forward-fail.sh");
    const QString script = scenario(
        name, QStringLiteral("always-forward-fail.log"),
        QStringLiteral("printf 'bind [127.0.0.1]:%s: Address already in use\\n' "
                       "\"$PORT\" >&2\nexit 255\n"));
    Check(!script.isEmpty(), QStringLiteral("SSH-U12 替身写好"));
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    options.ssh_program = script;
    options.ssh_target = QStringLiteral("fake");
    options.local_port = 0;
    options.ready_timeout_ms = 4000;
    tunnel.Start(options);
    const bool settled = WaitFor(
        [&tunnel] { return tunnel.state() == State::kFailed; }, 20000);
    // ArgsFromLog 已经把 "ARG:" 前缀剥掉了，这里数的是真正的参数本身。
    const int attempts = ArgsFromLog(dir + QStringLiteral("/always-forward-fail.log"))
                             .count(QStringLiteral("-N"));
    Check(settled && tunnel.failure() == Failure::kForwardFailed,
          QStringLiteral("SSH-U12a 始终绑定失败 -> kForwardFailed"),
          tunnel.failureKindName());
    Check(attempts >= 2 && attempts <= 5,
          QStringLiteral("SSH-U12b 重试次数有界（2..5）"),
          QStringLiteral("attempts=%1 args=[%2]")
              .arg(attempts)
              .arg(ArgvFromLog(dir + QStringLiteral("/always-forward-fail.log"))
                       .replace(QLatin1Char('\n'), QLatin1Char('|'))));
  }

  // ---- SSH-U13: 快速 start/stop 循环 ----
  {
    const QString script = scenario(QStringLiteral("ssh-cycle.sh"),
                                    QStringLiteral("cycle.log"),
                                    BindAndSleepBlock());
    Check(!script.isEmpty(), QStringLiteral("SSH-U13 替身写好"));
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    options.ssh_program = script;
    options.ssh_target = QStringLiteral("fake-cycle");
    options.ready_timeout_ms = 6000;
    bool saw_ready = false;
    bool all_stopped = true;
    for (int index = 0; index < 12; ++index) {
      tunnel.Start(options);
      saw_ready = WaitFor([&tunnel] { return tunnel.IsReady(); }, 8000) || saw_ready;
      tunnel.Stop();
      if (tunnel.state() != State::kStopped) {
        all_stopped = false;
      }
    }
    Check(saw_ready, QStringLiteral("SSH-U13a 循环里至少有一次真的 Ready"),
          QStringLiteral("state=%1 failure=%2 diagnostic=%3")
              .arg(static_cast<int>(tunnel.state()))
              .arg(tunnel.failureKindName())
              .arg(tunnel.diagnosticText().left(200)));
    Check(all_stopped, QStringLiteral("SSH-U13b 每次 Stop 之后都回到 Stopped"));
    Check(ZombieChildCount() == 0,
          QStringLiteral("SSH-U13c 循环之后没有僵尸子进程"),
          QStringLiteral("zombies=%1").arg(ZombieChildCount()));
  }

  // ---- SSH-U14: 正在建立时重复 Start ----
  {
    const QString script = scenario(QStringLiteral("ssh-slow.sh"),
                                    QStringLiteral("slow.log"),
                                    QStringLiteral("sleep 1\n") + BindAndSleepBlock());
    Check(!script.isEmpty(), QStringLiteral("SSH-U14 替身写好"));
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    options.ssh_program = script;
    options.ssh_target = QStringLiteral("fake-slow");
    options.ready_timeout_ms = 9000;
    Check(tunnel.Start(options), QStringLiteral("SSH-U14a 第一次 Start 被受理"));
    const bool second = tunnel.Start(options);
    Check(!second, QStringLiteral("SSH-U14b 正在建立时第二次 Start 被拒"));
    const bool ready = WaitFor([&tunnel] { return tunnel.IsReady(); }, 12000);
    Check(ready, QStringLiteral("SSH-U14c 第一次请求仍然正常完成"),
          QStringLiteral("state=%1 failure=%2 diagnostic=%3")
              .arg(static_cast<int>(tunnel.state()))
              .arg(tunnel.failureKindName())
              .arg(tunnel.diagnosticText().left(200)));
    tunnel.Stop();
  }

  // ---- SSH-U15: 外部监听者只借不杀 ----
  {
    QTcpServer external;
    Check(external.listen(QHostAddress::LocalHost, 0),
          QStringLiteral("SSH-U15 外部监听者起得来"));
    const int external_port = external.serverPort();
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    // 故意给一个不存在的 ssh：如果实现去起进程，就会失败；能 Ready 就说明
    // 走的确实是"复用外部监听者"这条路。
    options.ssh_program = dir + QStringLiteral("/no-such-ssh-binary");
    options.ssh_target = QStringLiteral("external-user-tunnel");
    options.local_port = external_port;
    tunnel.Start(options);
    Check(tunnel.IsReady() && tunnel.IsExternalReuse() &&
              tunnel.localPort() == external_port,
          QStringLiteral("SSH-U15a 外部监听者被识别并复用"),
          QStringLiteral("state=%1 reuse=%2 port=%3")
              .arg(static_cast<int>(tunnel.state()))
              .arg(tunnel.IsExternalReuse() ? 1 : 0)
              .arg(tunnel.localPort()));
    Check(tunnel.processId() == 0 && !tunnel.OwnsProcess(),
          QStringLiteral("SSH-U15b 复用时本对象不持有任何进程"));
    tunnel.Stop();
    QString still_open_error;
    Check(SshTunnelManager::IsLoopbackPortOpen(external_port, 500,
                                               &still_open_error),
          QStringLiteral("SSH-U15c Stop 之后外部监听者仍然活着（没被杀）"),
          still_open_error);
    external.close();
  }

  // ---- SSH-U16: 建立过程中析构 ----
  {
    const QString script = scenario(QStringLiteral("ssh-destroy.sh"),
                                    QStringLiteral("destroy.log"),
                                    QStringLiteral("sleep 2\n") + BindAndSleepBlock());
    Check(!script.isEmpty(), QStringLiteral("SSH-U16 替身写好"));
    SshTunnelManager::Options options;
    options.ssh_program = script;
    options.ssh_target = QStringLiteral("fake-destroy");
    options.ready_timeout_ms = 9000;
    auto* tunnel = new SshTunnelManager();
    tunnel->Start(options);
    // 真的起过进程才算测到"建立过程中析构"。
    const bool started = WaitFor(
        [tunnel] { return tunnel->processId() > 0; }, 5000);
    const qint64 pid = tunnel->processId();
    delete tunnel;  // 析构必须把进程收干净，而且不能卡住
    Check(started, QStringLiteral("SSH-U16a 析构之前进程已经起来"),
          QString::number(pid));
    Check(WaitFor([pid] { return !ProcessAlive(pid); }, 6000),
          QStringLiteral("SSH-U16b 析构之后没有留下孤儿 ssh"),
          QString::number(pid));
  }

  // ---- SSH-U17: 大量 stderr ----
  {
    const QString script = scenario(
        QStringLiteral("ssh-noisy.sh"), QStringLiteral("noisy.log"),
        QStringLiteral(
            "i=0\nwhile [ $i -lt 4000 ]; do\n"
            "  printf 'noise line %s with a fair amount of text\\n' \"$i\" >&2\n"
            "  i=$((i+1))\ndone\n") +
            BindAndSleepBlock());
    Check(!script.isEmpty(), QStringLiteral("SSH-U17 替身写好"));
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    options.ssh_program = script;
    options.ssh_target = QStringLiteral("fake-noisy");
    options.ready_timeout_ms = 15000;
    tunnel.Start(options);
    const bool ready = WaitFor([&tunnel] { return tunnel.IsReady(); }, 20000);
    Check(ready, QStringLiteral("SSH-U17a 大量 stderr 不影响就绪判定"),
          QStringLiteral("state=%1 failure=%2 diagnostic=%3")
              .arg(static_cast<int>(tunnel.state()))
              .arg(tunnel.failureKindName())
              .arg(tunnel.diagnosticText().left(200)));
    const QString diagnostic = tunnel.diagnosticText();
    Check(diagnostic.size() <= 2100,
          QStringLiteral("SSH-U17b 诊断文本有界"),
          QStringLiteral("size=%1").arg(diagnostic.size()));
    bool control_free = true;
    for (const QChar ch : diagnostic) {
      if (ch.unicode() < 0x20 && ch != QLatin1Char('\n')) {
        control_free = false;
      }
    }
    Check(control_free, QStringLiteral("SSH-U17c 诊断文本没有控制字符"));
    tunnel.Stop();
  }

  // ---- SSH-U18: 忽略 SIGTERM 的子进程仍被回收 ----
  {
    const QString script = scenario(
        QStringLiteral("ssh-stubborn.sh"), QStringLiteral("stubborn.log"),
        QStringLiteral("trap '' TERM\n") + BindAndSleepBlock());
    Check(!script.isEmpty(), QStringLiteral("SSH-U18 替身写好"));
    SshTunnelManager tunnel;
    SshTunnelManager::Options options;
    options.ssh_program = script;
    options.ssh_target = QStringLiteral("fake-stubborn");
    options.ready_timeout_ms = 9000;
    tunnel.Start(options);
    const bool ready = WaitFor([&tunnel] { return tunnel.IsReady(); }, 12000);
    const qint64 pid = tunnel.processId();
    QElapsedTimer stop_timer;
    stop_timer.start();
    tunnel.Stop();
    const qint64 stop_ms = stop_timer.elapsed();
    Check(ready, QStringLiteral("SSH-U18a 顽固替身也能 Ready"),
          QStringLiteral("state=%1 failure=%2 diagnostic=%3")
              .arg(static_cast<int>(tunnel.state()))
              .arg(tunnel.failureKindName())
              .arg(tunnel.diagnosticText().left(200)));
    Check(WaitFor([pid] { return !ProcessAlive(pid); }, 6000),
          QStringLiteral("SSH-U18b 忽略 SIGTERM 之后仍被回收"), QString::number(pid));
    Check(stop_ms < 8000, QStringLiteral("SSH-U18c Stop 的等待是有界的"),
          QStringLiteral("%1 ms").arg(stop_ms));
  }

  // ---- 收尾：僵尸与孤儿 ----
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  Check(ZombieChildCount() == 0, QStringLiteral("SSH-U19 全程没有僵尸子进程"),
        QStringLiteral("zombies=%1").arg(ZombieChildCount()));

  std::printf("[ssh-tunnel] passed=%d failed=%d\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
