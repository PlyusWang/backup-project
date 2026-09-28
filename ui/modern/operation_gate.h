// operation_gate.h
//
// Modern GUI 进程内"同一时刻只有一个会改动持久状态的业务操作"的闸门。
//
// 为什么必须有它，而不是各自维护一个 busy_ 标志：
//
//   BackupController 与 ScheduleController 是**两个**对象，各自有 busy_。
//   手动操作忙的时候 schedule 知道（它订阅了 BackupController::busyChanged），
//   但反过来 schedule 忙的时候手动操作完全不知道。那是一条真实可达的路径：
//   后台评估线程正在写 schedule.json / 归档，GUI 线程同时
//   delete → reconcile → store.Save，于是同一进程里出现两个 writer。
//   原来只有 QML 的 enabled: !busy 挡着，那是界面礼貌，不是不变式。
//
// 语义刻意做得很小：
//
//   * Acquire(kind)：没人持有时成功并记住持有者；否则失败，并把"谁在占着"
//     写进 reason，调用方据此给出人类可读的状态；
//   * 同一个 kind 的第二次 Acquire 同样失败 —— 不变量是"最多一个持有者"，
//     不是"最多一个每种 kind"。派生写（删除归档之后的计划状态同步）由持有者
//     在自己内部完成，见 BackupController::ArchiveDeletedObserver；
//   * Release(kind)：只有持有者能释放。
//
// 它是纯状态，不加锁：所有 Acquire/Release 都发生在 GUI 主线程上
// （后台线程不碰闸门，它只被"启动"与"结束"两件事包住）。

#ifndef BACKUP_PROJECT_UI_MODERN_OPERATION_GATE_H_
#define BACKUP_PROJECT_UI_MODERN_OPERATION_GATE_H_

#include <QString>

namespace backup_modern {

class OperationGate {
 public:
  enum class Kind {
    kNone,
    // 手动备份：写仓库 + 更新 catalog。
    kManualBackup,
    // 受管恢复：写目的地目录。
    kManualRestore,
    // 删除归档：改仓库，并顺带同步 ScheduleStore 的 managed 名单。
    kManualDelete,
    // 改仓库路径：写 config.json。
    kRepositoryChange,
    // 保存 / 启停计划：写 schedule.json。
    kScheduleConfig,
    // 后台评估：写归档 + manifest + schedule.json。
    kScheduleEvaluation,
    // 保存 / 启停实时备份：写 realtime.json。
    kRealtimeConfig,
    // 后台实时触发评估：写归档 + .realtime marker + 淘汰旧实时快照。
    kRealtimeEvaluation,
  };

  // 给人看的名字，用于状态栏里那句"XX 正在运行"。
  static const char* KindName(Kind kind);

  bool Acquire(Kind kind, QString* reason);
  void Release(Kind kind);

  bool busy() const { return owner_ != Kind::kNone; }
  Kind owner() const { return owner_; }

 private:
  Kind owner_ = Kind::kNone;
};

// RAII 版本：让"每一条 return 路径上都要记得 Release"这件事由编译器保证。
class OperationGuard {
 public:
  OperationGuard(OperationGate* gate, OperationGate::Kind kind);
  ~OperationGuard();

  OperationGuard(const OperationGuard&) = delete;
  OperationGuard& operator=(const OperationGuard&) = delete;

  bool acquired() const { return acquired_; }
  const QString& reason() const { return reason_; }
  // 提前释放（例如"闸门只覆盖真正写盘的那一小段"）。
  void Release();

 private:
  OperationGate* gate_ = nullptr;
  OperationGate::Kind kind_ = OperationGate::Kind::kNone;
  bool acquired_ = false;
  QString reason_;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_OPERATION_GATE_H_
