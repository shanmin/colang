// ============================================================
// loader.h — P212 模块导入加载器（.co 源合并 · DFS 后序 · AST 前插）
//
// 主流程（driver 端调用，parse 之后 analyze 之前）：
//   1) 扫主 TU 的 imports，按引用方目录解析路径，补 `.co` 后缀
//   2) 递归加载被导入模块（read → preprocess → parse）
//   3) 去重：规范化绝对路径 cache（Windows 大小写不敏感、统一转小写）
//   4) 环检测：DFS 栈 + back-edge 报路径（例：cycle detected: a.co -> b.co -> a.co）
//   5) DFS 后序拓序输出子模块列表
//   6) 按拓序逆序前插子模块 AST 到主 TU（被依赖方在前、主源本体在后）
//
// 语义：非主模块顶层裸代码在 import 时首次加载执行一次（用户 2026-10-23 拍板）。
// 本版仅消费 `.co`；`.bc`/`.dll` 消费归 0.3.0 链接器同批。
// ============================================================
#pragma once

#include <vector>

#include "colang/ast.h"
#include "colang/common.h"

namespace colang
{

struct CompileOptions;

// 递归加载并合并 imports。返回 false 表示错误（找不到模块 / 环路 / 子模块语法错误）；
// 错误已追加到 errors。主 TU 的 imports 为空时快速返回 true、不做任何修改。
bool load_and_merge_imports(const CompileOptions &opts, TranslationUnit &main_tu,
                            std::vector<Error> &errors);

}  // namespace colang
