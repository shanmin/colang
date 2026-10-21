// ============================================================
// parser.h — 语法分析器 + AST（P2 落地，P3 扩展）
//
// 手写递归下降。C 的核心难点：声明符语法与表达式语法的歧义
// （依赖 typedef 名的上下文消解）。产出 AST 供语义层检查。
// ============================================================
#pragma once

#include <vector>

#include "colang/driver.h"

namespace colang
{

// 语法分析：由 token 流构建 AST。接口为框架占位，实现时细化。
bool parse(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors);

}  // namespace colang
