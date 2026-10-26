// ============================================================
// parser.h — 语法分析器 + AST（P2 落地，P3 扩展）
//
// 手写递归下降。C 的核心难点：声明符语法与表达式语法的歧义
// （依赖 typedef 名的上下文消解）。产出 AST 供语义层检查。
// ============================================================
#pragma once

#include <vector>

#include "colang/driver.h"
#include "colang/loader.h" // P233：ImportTagSeed 类型（parse 种子入参）

namespace colang
{

// 语法分析：由 token 流构建 AST。接口为框架占位，实现时细化。
// P233：seeds 非空（仅 .co）时，parse 先把这些「已导入模块的 public 标签」登记为
//   裸标签类型种子（inert forward 引用），使主源 parse 期即可裸用跨模块标签。
//   默认空 = 现行为（.c / 无 import / 非种子路径逐字不变）。
bool parse(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors,
           const std::vector<ImportTagSeed> &seeds = {},
           const std::vector<ImportConstSeed> &const_seeds = {});

// P233：顶层 import 预扫——复用 parse_import_decl 语法、只提取顶层 import 声明，不做完整解析。
//   供 driver 在「parse 主源之前」拿到 root_imports、先 load_imports 采集标签种子再正式 parse。
//   fail-safe：本函数内部诊断走独立 scratch、不污染 errors；漏扫只会导致某标签未种子（正式 parse 明确报错）。
void collect_imports(const CompileState &state, std::vector<ImportDecl> &out);

}  // namespace colang
