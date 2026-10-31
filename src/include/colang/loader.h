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
	bool is_union = false; // struct vs union（is_enum=false 且 is_class=false 时有义）
	bool is_enum = false;  // 是否为 enum 标签
	bool is_class = false; // P300 Co class：是否为 public class 标签（宿主裸类名作类型的 parse 种子）
	// P343：public 记录的布局快照与成员表。前向声明（is_definition=false）留空 → 消费方
	//   仍按不完整处理；完整定义则把定义点算定的 size/align/成员偏移原样带出，使宿主的
	//   常量路径（sizeof/_Alignof/offsetof）不再因「嵌套类型不在本 TU」而回落到 4。
	std::vector<StructMember> members;
	int64_t layout_size = 0;
	int64_t layout_align = 0;
	std::vector<int64_t> layout_member_off;
};

// P256：加载阶段采集的 public const int 值种子。供 parser 在「parse 主源时」把
//   被导入模块的 public const int 作 case 标签/常量表达式求值（镜像 ImportTagSeed 前置机制）。
//   P306：`owner` = 消费方可书写限定名（`as` 别名优先、否则文件 stem）——供 `p.K` 这类限定访问
//   按 `owner.NAME` 索引；裸名口径照旧只看 `name`。
//   P307：`is_enum_const` = 该条目来自 `public enum` 的常量。此类种子**只进限定表**（`b.DIM` 可在
//   常量上下文求值），不进裸名通道——「enum 常量不跨模块」是 P225 的既有裁定（Win32 宏常量
//   走 `public const` 而非 enum），若让裸名 `DIM` 也命中就是静默扩语言面，须另行定向。
struct ImportConstSeed
{
	std::string name;
	std::int64_t value;
	std::string owner;
	bool is_enum_const = false; // P307：来自 public enum（只供限定名求值，不开裸名）
};

// P304：加载阶段采集的文件作用域 typedef 种子。与前两类不同，public 的那份必须携带**完整
//   已解析类型**（`CTypePtr`）——typedef 的对外语义就是「底层类型」，仅靠名字无法展开；
//   `owner` 是可书写限定名（`as` 别名优先、否则文件 stem），供限定展开与同名多来源的歧义裁定。
//   同名**不去重**（与标签种子不同）：裸名歧义必须在 parse 期就能报出来。
//   非 public typedef 也会出一条种子，但 `type` 为空——只为了让消费方把 `a.Hidden` 认成
//   限定类型名（从而由 sema 报 `no public type`），而不是报成语法错。
struct ImportTypedefSeed
{
	std::string name;
	CTypePtr type; // 空 = 非 public（仅送名字、不可展开）
	std::string owner;
	SourceLoc loc{};
};

// P233：两阶段 import 加载的中间载体。load 阶段填充（已加子 TU + 拓序 + 标签种子），
//   merge 阶段消费。`cache`/`postorder` 仅 loader.cpp 内部使用。
struct ImportBundle
{
	std::vector<ImportTagSeed> tag_seeds;
	std::vector<ImportConstSeed> const_seeds;     // P256：各模块 public const int 采集值（P307 亦含 public enum 常量，限限定名可用）
	std::vector<ImportTypedefSeed> typedef_seeds; // P304：各模块 public typedef（含底层类型）
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
