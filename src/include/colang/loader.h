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

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "colang/ast.h"
#include "colang/common.h"

namespace colang
{

struct CompileOptions;

// P233：加载阶段可前置于主源 parse 完成，供 parser 把已导入模块的 public 标签
//   作为「裸标签作类型」种子（`.co` 新面）。仅携带 parser 所需最小信息。
struct ImportTagSeed
{
	std::string name;      // 用户可见标签名（源码写的 tag）
	bool is_union = false; // struct vs union（is_enum=false 时有义）
	bool is_enum = false;  // 是否为 enum 标签
};

// P256：加载阶段采集的 public const int 值种子。供 parser 在「parse 主源时」把
//   被导入模块的 public const int 作 case 标签/常量表达式求值（镜像 ImportTagSeed 前置机制）。
struct ImportConstSeed
{
	std::string name;
	std::int64_t value;
};

// P233：两阶段 import 加载的中间载体。load 阶段填充（已加子 TU + 拓序 + 标签种子），
//   merge 阶段消费。`cache`/`postorder` 仅 loader.cpp 内部使用。
struct ImportBundle
{
	std::vector<ImportTagSeed> tag_seeds;
	std::vector<ImportConstSeed> const_seeds;     // P256：各模块 public const int 采集值
	std::unordered_map<std::string, TUPtr> cache; // 规范化 key -> 子模块 TU
	std::vector<std::string> postorder;           // DFS 后序（不含主源）
};

// P233 阶段一：递归加载 root_imports 引用的模块（read→preprocess→parse）、去重、环检测，
//   将已加载子 TU 与拓序存入 bundle、并采集 public struct/enum 标签种子。**不合并到主源**。
//   root_imports 来自主源 parse **之前**的预扫（driver 端），故本阶段可在 parse 主源前跑。
bool load_imports(const CompileOptions &opts, const std::vector<ImportDecl> &root_imports,
                  ImportBundle &bundle, std::vector<Error> &errors);

// P233 阶段二：按拓序逆序前插 bundle 已加载子模块 AST 到主 TU（与现 merge_into_main 同构），
//   并回填 main_tu.imports 各项的 resolved_module_key（供 sema 可见集）。root 加载需先跑 load_imports。
bool merge_imports(const CompileOptions &opts, TranslationUnit &main_tu, ImportBundle &bundle);

// 递归加载并合并 imports（= load_imports + merge_imports 的组合，主源 imports 由传入 TU 读取）。
// 返回 false 表示错误（找不到模块 / 环路 / 子模块语法错误）；错误已追加到 errors。
// 主 TU 的 imports 为空时快速返回 true、不做任何修改。
bool load_and_merge_imports(const CompileOptions &opts, TranslationUnit &main_tu,
                            std::vector<Error> &errors);

}  // namespace colang
