// ============================================================
// codegen.h — 代码生成（P2/P3 落地）
//
// 用 LLVM IRBuilder 把规范化后的 AST 翻译为 LLVM IR，产出 .bc/.ll，
// 优化与机器码交给 LLVM 后端（llc/clang）。
// ============================================================
#pragma once

#include <string>
#include <vector>

#include "colang/driver.h"

namespace colang
{

// 代码生成：AST → LLVM IR。接口为框架占位，实现时细化
// （届时引入 llvm::Module / IRBuilder，并按需暴露 write_ir / write_bitcode）。
bool generate(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors);

}  // namespace colang
