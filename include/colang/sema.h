// ============================================================
// sema.h — 语义分析（P3 落地）
//
// 类型检查、标准转换（整型提升 / usual arithmetic conversions /
// 隐式指针转换）、作用域与链接、约束项检查（6.3 各条）。
// 是 C 语义正确性的主战场，逐条对照标准实现。
// ============================================================
#pragma once

#include <vector>

#include "colang/driver.h"

namespace colang
{

// 语义分析：对 AST 做类型检查与规范化。接口为框架占位，实现时细化。
bool analyze(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors);

}  // namespace colang
