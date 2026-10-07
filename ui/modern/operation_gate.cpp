// operation_gate.cpp
//
// 见 operation_gate.h。

#include "operation_gate.h"

namespace backup_modern {

// 返回的是**用户可见文案**（会被拼进状态栏那句话），不是日志用的枚举名；
// 未知 Kind 返回“未知操作”而不是空串，界面不会出现空标题。
const char* OperationGate::KindName(Kind kind) {
  switch (kind) {
    case Kind::kNone:
      return "空闲";
    case Kind::kManualBackup:
      return "手动备份";
    case Kind::kManualRestore:
      return "手动恢复";
    case Kind::kManualDelete:
      return "删除备份";
    case Kind::kRepositoryChange:
      return "修改备份仓库";
    case Kind::kScheduleConfig:
      return "保存计划配置";
    case Kind::kScheduleEvaluation:
      return "定时备份评估";
    case Kind::kRealtimeConfig:
      return "保存实时配置";
    case Kind::kRealtimeEvaluation:
      return "实时备份评估";
  }
  return "未知操作";
}

// 这是互斥量而不是信号量：owner_ 记的是“谁”而不是计数，所以同一个 kind 的
// 第二次 Acquire 同样失败（不变量见 operation_gate.h）。
// 所有调用都发生在 GUI 主线程，因此不加锁、不用原子变量。
bool OperationGate::Acquire(Kind kind, QString* reason) {
  if (kind == Kind::kNone) {
    if (reason != nullptr) *reason = QStringLiteral("内部错误：空的操作用途");
    return false;
  }
  if (owner_ != Kind::kNone) {
    if (reason != nullptr) {
      *reason = QStringLiteral("%1 正在进行，请等它结束后再试。")
                    .arg(QString::fromUtf8(KindName(owner_)));
    }
    return false;
  }
  owner_ = kind;
  return true;
}

void OperationGate::Release(Kind kind) {
  // 只有持有者能释放。非持有者调用是编程错误，但这里不做崩溃式断言：
  // 静默忽略比在一个"释放错了"的 bug 上把界面打死要好。
  if (owner_ == kind) owner_ = Kind::kNone;
}

// 构造即尝试获取：失败不抛异常，把原因留在 reason_ 里，由业务代码决定是
// 中止还是先提示；gate_ == nullptr 时整个 guard 退化成空操作。
OperationGuard::OperationGuard(OperationGate* gate, OperationGate::Kind kind)
    : gate_(gate), kind_(kind) {
  if (gate_ == nullptr) return;
  acquired_ = gate_->Acquire(kind_, &reason_);
}

OperationGuard::~OperationGuard() { Release(); }

// 幂等：重复调用只有第一次生效，acquired_ 会被清掉。
void OperationGuard::Release() {
  if (!acquired_ || gate_ == nullptr) return;
  gate_->Release(kind_);
  acquired_ = false;
}

}  // namespace backup_modern
