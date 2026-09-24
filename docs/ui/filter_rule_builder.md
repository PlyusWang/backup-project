# Filter 规则构造中间层（filter_rule_builder）

> 状态：已实现（include/filter_rule_builder.h、src/filter/filter_rule_builder.cpp）

## 为什么单独一层

可视化编辑器需要把「表单」变成「规则文本」，又不能让 GUI 变成第二个匹配引擎。
中间层只做四件事，且不含任何匹配逻辑：

1. 结构校验：空值、非法日期、区间反向等表单级问题；
2. 序列化：FilterClauseDraft / FilterRuleDraft -> 现有 DSL；
3. 摘要：生成人类可读中文摘要；
4. CLI 参数拼装：--include / --exclude 与规则文本交替。

**最终语法裁决仍然交给 Filter::AddRule**：ValidateRule() 内部就是这么做的，
所以「前端校验通过」与「后端接受」不可能不一致。

## 位置取舍

放在 src/filter/（进 CORE_SOURCES），而不是 GUI 私有目录：它是纯 C++、零 Qt 依赖，
CLI、Modern GUI 与测试都能复用。Qt 桥（ui/modern/filter_rule_model.*）只负责
QObject / 信号槽 / QML 暴露，不重新实现 DSL、校验或匹配。

## 测试

| 入口 | 内容 |
| --- | --- |
| scripts/filter_rule_builder_test.sh | 单元测试：字段序列化、ext 列表、size 单位与区间、mtime 各种形式、非法输入、摘要、CLI 参数；并断言每条生成的 DSL 都能被真实核心接受 |
| scripts/filter_rule_builder_int_test.sh | 端到端：builder 生成的规则驱动真实 backupctl 做 backup + restore，校验该在的在、该排除的不在，并与手写 CLI 规则的结果逐字节比对 |

两个脚本都真实执行且可重复运行；日志写在 tests/output/（已 gitignore）。
