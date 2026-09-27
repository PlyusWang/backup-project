// operation_gate.cpp
//
// 见 operation_gate.h。

#include "operation_gate.h"

namespace backup_modern {

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
  }
  return "未知操作";
}

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

OperationGuard::OperationGuard(OperationGate* gate, OperationGate::Kind kind)
    : gate_(gate), kind_(kind) {
  if (gate_ == nullptr) return;
  acquired_ = gate_->Acquire(kind_, &reason_);
}

OperationGuard::~OperationGuard() { Release(); }

void OperationGuard::Release() {
  if (!acquired_ || gate_ == nullptr) return;
  gate_->Release(kind_);
  acquired_ = false;
}

}  // namespace backup_modern
