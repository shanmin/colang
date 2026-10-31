// ============================================================
// ast.h — 抽象语法树 + 类型系统（P2 起步，逐阶段增长）
//
// 设计原则：
//   - tagged-struct（不用 class 虚函数层次）；节点经 shared_ptr 递归引用。
//   - CType 由 parser 产出（type-specifier / pointer），P3 sema 补推断。
//   - Expr / Stmt 由 parser 构造，P3 sema 填充 type / 检查约束。
//   - P2 最小集：int / char / void / pointer / function / call / return /
//     compound / string/int literal / identifier。后续增 struct/array/enum 等。
// ============================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "colang/common.h"
#include "colang/lexer.h"

namespace colang
{

// ============================================================
// 类型系统
// ============================================================
enum class TypeKind
{
	Void,
	Bool,       // _Bool (C99)
	Char,
	WChar,      // wchar_t：2 字节无符号（对齐宿主 CRT 的 unsigned short，见 ADR-104 裁定）
	Char8,      // char8_t (C23)：1 字节恒无符号（i8）
	Char16,     // char16_t (C11)：2 字节恒无符号（i16）
	Char32,     // char32_t (C11)：4 字节恒无符号（i32）
	Short,      // short：2 字节有/无符号整型（i16；is_unsigned 区分 signed/unsigned short）
	Int,
	Long,
	LongLong,   // long long (C99)
	Float,
	Double,
	Pointer,
	Array,
	Function,
	Struct,   // P3+
	Union,    // P3+
	Enum,     // P3+
	Class,    // P300 Co class 引用类型（0.4.0）
	BitInt,   // _BitInt(N) (C23)
};

struct CType;
using CTypePtr = std::shared_ptr<CType>;

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;   // 前置声明：CType 需携带 VLA 维度表达式

// 路 B 阶段1b-iii：限定访问绑定字段需引 FuncDecl/GlobalVar 指针——前置声明（完整定义见后）。
struct FuncDecl;
struct GlobalVar;

struct CType
{
	TypeKind kind = TypeKind::Int;
	bool is_const = false;

	// —— P253：诊断拼写专用的限定符/别名位 ——
	// 仅供 `describe_type` 还原 clang 形拼写；**不参与**大小、对齐、布局与 `types_compatible`
	//   判定，故对代码生成零影响（is_const 因其历史职责仍兼作 `_Generic`/lvalue 判定位）。
	bool is_volatile = false; // 修前 volatile 在解析期被整组丢弃（`volatile int` 打印成 `int`）
	bool is_restrict = false; // 仅指针位有意义（修前与 const 共用 is_const 占位、不可区分）
	bool is_atomic = false;   // `_Atomic`：指针位 clang 拼作 `_Atomic(T)`
	// 使用点书写的 typedef 别名（可含使用点外加的 const/volatile 前缀，如 `const volatile MyInt`）。
	//   空串 = 非别名类型。别名仅覆盖整棵子树，故拼写时直接原样输出并另出去别名形（`aka`）。
	std::string alias;

	// Pointer / Array：元素类型
	CTypePtr base;

	// Array：元素数量（0 = 未指定）
	std::size_t array_size = 0;

	// Array（C99 VLA，§6.7.2.1）：维度为非常量表达式时置位，vla_dim 供 codegen 运行期求值。
	// 与 array_size==0（未指定/柔性）区分开：柔性数组无维度表达式，VLA 有运行期个数。
	bool is_vla = false;
	ExprPtr vla_dim;

	// Function：返回类型用 base；参数列表 + 是否变参
	std::vector<CTypePtr> params;
	bool is_variadic = false;

	// Struct/Union/Enum（P3+）：名称
	std::string tag;

	// BitInt（C23）：位宽 + 无符号标志
	unsigned bit_width = 0;
	bool is_unsigned = false;

	// C23 _Alignas：强制对齐（字节；0 = 未指定，用类型自然对齐）
	// 承载于 typedef/类型形式 _Alignas(T) 解析结果，对象声明时并入 obj_align
	unsigned align = 0;
};

// 便捷构造
inline CTypePtr make_type(TypeKind k, bool cnst = false)
{
	auto t = std::make_shared<CType>();
	t->kind = k;
	t->is_const = cnst;
	return t;
}

inline CTypePtr make_pointer(CTypePtr pointee)
{
	auto t = std::make_shared<CType>();
	t->kind = TypeKind::Pointer;
	t->base = std::move(pointee);
	return t;
}

inline CTypePtr make_array(CTypePtr elem, std::size_t n)
{
	auto t = std::make_shared<CType>();
	t->kind = TypeKind::Array;
	t->base = std::move(elem);
	t->array_size = n;
	return t;
}

// 字符串字面量作**数组对象**时的类型（§6.4.5p6/p7）：char[N]/wchar_t[N]/char32_t[N]，N 含 '\0'。
// 仅用于**不施加数组→指针退化**的语境（sizeof；§6.3.2.1 的例外）；其余表达式语境仍按
//   resolve_expr_type / gen_expr 退化为元素指针（如 `char *p = "abc"`、`_Generic("abc", char*:…)`）。
inline CTypePtr ctype_of_string_literal(const std::string &raw)
{
	const std::size_t n = co_str_lit_len(raw);
	switch (co_str_lit_width(raw))
	{
	case 2: return make_array(make_type(TypeKind::WChar), n);
	case 4: return make_array(make_type(TypeKind::Char32), n);
	default: return make_array(make_type(TypeKind::Char), n);
	}
}

inline CTypePtr make_func_type(CTypePtr ret, std::vector<CTypePtr> ps, bool variadic)
{
	auto t = std::make_shared<CType>();
	t->kind = TypeKind::Function;
	t->base = std::move(ret);
	t->params = std::move(ps);
	t->is_variadic = variadic;
	return t;
}

// ============================================================
// 表达式
// ============================================================
enum class ExprKind
{
	IntLiteral,
	FloatLiteral,   // 浮点字面量（3.14 等）
	CharLiteral,
	StringLiteral,
	VarRef,
	Call,
	Assign,
	Binary,
	Unary,
	Cast,         // (type)expr， type 字段存目标类型
	Ternary,      // cond ? then : else，lhs=cond args[0]=then args[1]=else
	Comma,        // a, b, c， args=子表达式列表，结果=最后一个
	Member,       // obj.member 或 ptr->member，lhs=对象 op="."/"->" name=成员名 type=结构体类型
	InitList,     // {e1, e2, ...} 复合初始化器，args=子表达式列表
	Generic,      // _Generic(sel, type:expr, ...)  C11 类型选择
	New,          // P300 new ClassName(ctor_args)  heap 实例化
	ThisRef,      // P300 this 当前对象引用
};

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

// 指定初始化器的一个设计符步（C11 §6.7.9p6 designator-list）：`.name` 或 `[index]`。
// 多个设计符可串接成链，如 `.ps[1].y`。
struct DesigStep
{
	bool is_index = false;      // true = [index]，false = .name
	std::string name;           // is_index == false 时有效
	std::intmax_t index = 0;    // is_index == true 时有效（-1 = 下标解析失败）
};

struct Expr
{
	ExprKind kind = ExprKind::IntLiteral;
	SourceLoc loc;
	CTypePtr type; // sema 填充（P2 阶段可为 null）

	// IntLiteral / CharLiteral
	std::intmax_t int_val = 0;

	// CharLiteral：编码前缀 "u8"/"u"/"U"/"L"（无前缀为空）。
	//   按 ISO C §6.4.4.4，前缀决定字面量类型；parser 据此置 type 字段。
	std::string char_prefix;

	// FloatLiteral
	double float_val = 0.0;

	// StringLiteral（原始内容，含转义解码后）
	std::string str_val;

	// VarRef / Call（被调函数名）
	std::string name;

	// Call：实参列表
	std::vector<ExprPtr> args;

	// Assign / Binary / Unary
	ExprPtr lhs;
	ExprPtr rhs;
	std::string op; // 运算符拼写
	// P317：二元**运算符 token 自身**的 loc。clang 的部分二元诊断（指针差的类型相容）
	//   定在运算符列，而 `loc` 存的是 rhs 侧起点（见 parser::parse_binary）→ 二者分开保存。
	// P328：`[]` 下标（同为 Binary）填**该层 `[` 的 loc**——clang 的「不可下标」诊断定在
	//   `[` 列，而 `loc` 存的是整条基址最左起点。其余节点保持默认（零值）。
	SourceLoc op_loc;
	bool postfix = false; // Unary ++/-- 后缀标志

	// 指定初始化器（C99）
	int desig_index = -1;       // [idx] 数组下标（-1 = 无）；链式时为首级
	std::string desig_field;    // .member 成员名（空 = 无）；链式时为首级
	// 完整设计符链（P237）：链长 ≥ 2 时走链式路径，首级仍回填上面两个兼容字段
	std::vector<DesigStep> desig_chain;

	// 路 B 阶段1b-iii：限定访问 m.f() / m.x 绑定（sema 决议后挂 AST 指针，codegen 直读、
	//   不经名字符串复找回）。仅 per-module 转正路径的限定访问填；旧单遍/.c/无 import 恒空、
	//   codegen 逐字等价零改动。
	const FuncDecl *q_func = nullptr;    // m.f()/m.f 决议到的目标函数（Member 节点上）
	const GlobalVar *q_global = nullptr; // m.x 决议到的目标全局变量（Member 节点上）
	bool module_qualifier = false;       // 基 VarRef 名是模块限定符（非变量），抑制 undeclared 诊断
};

// ============================================================
// 语句
// ============================================================
enum class StmtKind
{
	Return,
	ExprStmt,
	Compound,
	Decl,
	If,
	While,
	DoWhile,
	For,
	Break,
	Continue,
	Goto,
	Label,
	Switch,       // switch(expr) body
	Case,         // case N: stmt
	Default,      // default: stmt
	Delete,       // P300 Co class（0.4.0）：delete <句柄变量>; 强制销毁（expr=操作数，上下文物、非 C 语句）
};

struct Stmt;
using StmtPtr = std::shared_ptr<Stmt>;

struct Stmt
{
	StmtKind kind = StmtKind::ExprStmt;
	SourceLoc loc;

	// Return / ExprStmt / If.cond / While.cond / DoWhile.cond / Switch.disc / For.cond
	ExprPtr expr;

	// Compound / Switch.body
	std::vector<StmtPtr> body;

	// If: then/else; While/DoWhile/For: then=body; Switch: then=body; Label: then=labeled_stmt
	StmtPtr then_branch;
	StmtPtr else_branch;

	// Decl（局部变量）
	CTypePtr var_type;
	std::string var_name;
	SourceLoc var_ident_loc; // P331：声明符标识符位置；与声明起点 loc 分离，供 void 源初始化诊断。
	ExprPtr var_init;
	bool is_static = false;  // 局部 static
	bool is_extern = false;  // 块作用域 extern 声明（引用文件作用域对象）
	unsigned var_align = 0;  // C23 _Alignas 强制对齐（字节；0 = 未指定）

	// Compound: 仅真块 {...} 为 true；parser 把多声明符折成的合成 Compound 为 false
	bool is_block_scope = false;

	// For: for_init=init_stmt, for_incr=incr_expr
	StmtPtr for_init;
	ExprPtr for_incr;

	// C23 if/switch init-statement
	StmtPtr init_stmt;

	// Case: case_val
	std::int64_t case_val = 0;

	// Goto/Label: label_name
	std::string label_name;
};

// ============================================================
// 声明（顶层）
// ============================================================
// C23 属性（P214）
// ============================================================
// parser 在声明位识别并记录 [[...]]，产物累积在本结构中，最终落到 FuncDecl/GlobalVar。
// 本批仅落地两个属性：
//   [[deprecated]] / [[deprecated("消息")]] —— ISO C23 §6.7.11 标准属性
//   [[lib("链接名")]]                     —— Co 层新定义属性（声明该符号由哪个导入库实现）
// 其余标准属性（nodiscard/maybe_unused/…）与厂商限定名分别按「接受不生效」/「拒收报错」处理。
// 「接受不生效」的位置（局部声明/语句位/typedef）不经本结构，直接整段消费。
struct DeclAttributes
{
	bool deprecated = false;
	std::string deprecated_msg;   // [[deprecated("msg")]] 的消息（可空）
	// P249：C23 [[nodiscard]] / [[noreturn]]（本批落地为真实语义，见 sema）
	bool nodiscard = false;
	std::string nodiscard_msg;    // [[nodiscard("msg")]] 的理由（可空）
	bool noreturn_attr = false;
	bool has_lib = false;
	std::string lib_name;         // [[lib("user32")]] 的链接名（不含 .lib 后缀/路径）
	// P229 Co 层新属性：[[subsystem("windows")]] → 链接子系统切 Windows GUI（无控制台）。
	//   仅入口函数（WinMain/main）标注有效；由 codegen 汇聚后经 CompileState 交链接层。
	bool subsystem_windows = false;
	// 0.4.1 a2-follow：本列表中出现过的属性名拼写+定位（含 :: 限定链全名）。
	//   供「类型语境」拒收逐名对齐 clang：`'X' attribute cannot be applied to types`；
	//   空列表 [[::]] 无条目 → 合法零诊断（C23 §6.7.11p3）。
	std::vector<std::pair<SourceLoc, std::string>> spellings;
};

// ============================================================
// 函数 / 全局变量
// ============================================================
struct Param
{
	std::string name;
	CTypePtr type;
	SourceLoc loc; // 形参声明起始位置（诊断 note 用）
};

struct FuncDecl
{
	std::string name;
	CTypePtr return_type;
	std::vector<Param> params;
	bool is_variadic = false;
	bool is_static = false;  // static 函数 (InternalLinkage)
	bool is_public = false;  // P211 Co 层：public 标记 → DLL 导出（不改 linkage）
	// P214 C23 属性：[[deprecated]] / [[lib("X")]]
	bool is_deprecated = false;
	std::string deprecated_msg;
	// P249：[[nodiscard]]（返回值不得丢弃）/ [[noreturn]]（永不返回）
	bool is_nodiscard = false;
	std::string nodiscard_msg;
	bool is_noreturn = false;
	std::string lib_name;    // 空 = 未标注 [[lib]]
	bool subsystem_windows = false;  // P229：[[subsystem("windows")]] → Windows GUI 子系统
	StmtPtr body; // CompoundStmt if definition; nullptr if declaration only
	SourceLoc loc;
	// P212 loader 归属标记：源模块规范化绝对路径 key；空串 = 主源 TU 本体。
	//   仅 loader 在合并前写入；parser/sema/codegen 默认空即视为「主源」。
	std::string source_module_key;
	// 路 B 阶段2-c：sema 撞名分流裁定的 LLVM 链接符号名（空 = 用 name）。
	//   跨模块同名需共存时挂 dunder 合成名 `__<stem>__<name>`（仍合法 C 标识符）；codegen 取符号一律走此。
	std::string symbol_name;
};

struct GlobalVar
{
	std::string name;
	CTypePtr type;
	ExprPtr init; // nullptr = 无显式初始化
	bool is_static = false;  // InternalLinkage
	bool is_extern = false;  // 纯声明（无定义）
	bool is_public = false;  // P211 Co 层：public 标记 → DLL 导出（不改 linkage）
	// P214 C23 属性：[[deprecated]] / [[lib("X")]]
	bool is_deprecated = false;
	std::string deprecated_msg;
	std::string lib_name;
	unsigned align = 0;      // C23 _Alignas 强制对齐（字节；0 = 未指定）
	// P252：`_Alignas` 跨声明一致性（§6.7.2.1p4）所需事实（比对在 sema 侧做）
	bool align_specified = false; // 本条声明是否出现过 _Alignas（含 _Alignas(0)；与 align==0「未指定」不可混同）
	unsigned align_effective = 0; // 本条的结果对齐 = max(_Alignas 值, 类型自然对齐)（clang「A vs B」取此值）
	SourceLoc align_spec_loc;     // _Alignas 关键字位置（「redeclaration has different alignment」指此列）
	SourceLoc ident_loc;          // 声明符标识符位置（「must be specified on definition」指此列）
	SourceLoc loc;
	std::string source_module_key;  // P212 同 FuncDecl
	// 路 B 阶段2-c：sema 撞名分流裁定的 LLVM 链接符号名（空 = 用 name；同 FuncDecl）。
	std::string symbol_name;
};

// ============================================================
// struct / union / enum
// ============================================================
struct StructMember
{
	std::string name;
	CTypePtr type;
	int bit_width = -1;  // -1 = 普通成员; >=0 = 位域宽度
	unsigned align = 0;  // C23 _Alignas 成员强制对齐（字节；0 = 未指定）
	// P337：GNU __attribute__((packed)) 施于字段——无显式对齐时有效对齐降为 1（显式 align 仍压过它）。
	bool packed = false;
	SourceLoc loc;       // 成员声明起始位置（诊断 note 用）
};

struct StructDef
{
	std::string tag;     // struct Name 的 Name
	bool is_union = false;
	bool is_definition = false; // 带 { ... } 的完整定义（false = 仅前置引用，成员表为空不代表空类型）
	std::vector<StructMember> members;
	// P343：布局快照。定义点（成员类型信息已齐）由 parser 用 record_layout 算定，随 public 导出
	//   经 ImportTagSeed 传给消费方；`layout_size == 0` 表示未算，一切读取点回落到惰性推导。
	//   存在的理由：导入到宿主 TU 的记录只有 inert 种子，成员里若嵌了「导出方内部的非 public
	//   记录」，宿主侧递归推导找不到它而静默按 4 计；带上整张快照才能一次传准（含多层导入）。
	int64_t layout_size = 0;            // 上取整到 align 后的字节数
	int64_t layout_align = 0;           // 记录有效对齐
	std::vector<int64_t> layout_member_off; // 与 members 等长：成员起始偏移（位域＝存储单元起点）
	unsigned align = 0;   // GNU __attribute__((aligned(N))) 施加于 struct/union 类型本身（字节；0=未指定）
	// P337：GNU __attribute__((packed)) 施于类型——其内无显式对齐成员一律按 1 对齐（不递归改写
	//   已定义嵌套类型的内部布局；显式 _Alignas/类型级 aligned 压不住，见 clang 实测 Ha/T1/S10）。
	bool packed = false;
	// P338：`#pragma pack(N)` 定义点对本记录的对齐上限 cap（0=无 pack 作用域）。与 packed 并列的另一条
	//   路：无显式对齐成员按 min(自然对齐, cap) 摆放（cap 不递归改写嵌套内部、显式声明对齐压不动，
	//   见 clang 实测 pk_pack2：PK1/PK4/PK5/PK6）。含位域记录 clang 本就不因 pack 缩小，故不应用。
	unsigned pack_cap = 0;
	SourceLoc loc;
	bool is_public = false;         // P212：struct 定义可标 public 参与 import
	std::string source_module_key;  // P212 同 FuncDecl
};

struct EnumConstant
{
	std::string name;
	std::int64_t value;
};

struct EnumDef
{
	std::string tag;     // enum Name 的 Name
	std::vector<EnumConstant> constants;
	// P337：GNU __attribute__((aligned(N))) 施于 enum 类型——clang 实测改 _Alignof 而不改 sizeof
	//   （`enum __attribute__((aligned(16))) EA{..}` → sizeof 4 / _Alignof 16）；packed 对 enum 无效。
	unsigned align = 0;
	SourceLoc loc;
	bool is_public = false;         // P212
	std::string source_module_key;  // P212
};

// ============================================================
// class 定义（P300 Co 层，0.4.0）
// ============================================================
struct ClassMember
{
	CTypePtr type;          // 成员变量类型（本批限基元）
	std::string name;
	bool is_public = false;
	bool is_static = false; // static 类变量（零初始化、模块级内链接全局）
	SourceLoc loc;
};

struct ClassMethod
{
	std::string name;       // 方法名（构造=类名、析构="~"+类名）
	CTypePtr ret_type;      // 返回类型（ctor/dtor 为 Void）
	std::vector<std::pair<std::string, CTypePtr>> params; // 形参（本批限基元）
	StmtPtr body;
	bool is_public = false;
	bool is_static = false;
	bool is_ctor = false;
	bool is_dtor = false;
	SourceLoc loc;
};

struct ClassDef
{
	std::string tag;                    // 类名
	std::vector<ClassMember> members;
	std::vector<ClassMethod> methods;
	bool has_ctor = false;
	bool has_dtor = false;
	SourceLoc loc;
	bool is_public = false;
	std::string source_module_key;
};

// ============================================================
// import 声明（P212 Co 层）
// ============================================================
// 语法（沿用 .old3 D38、附录 B.4；filename 双形式）：
//   import * | name [as alias], … from ("file" | module_name) [as ModAlias];
// filename 双形式：
//   【字符串字面量】支持跨目录/特殊字符（如 "sub/lib.co"、"my-mod"）
//     无后缀自动补 `.co`；以引用方目录为基准
//   【简化标识符】from a 等价 from "a.co"（仅同目录，无引号/后缀/分隔符）
// 例：
//   import * from "stdio.co";                        // 全量导入（字符串）
//   import printf from "stdio.co";                    // 指定符号（字符串）
//   import puts as print_line from "io.co";           // 符号改名
//   import a, b as x, c from "m.co" as MyMod;         // 多项 + 模块别名
//   import * from stdio;                              // 简化：同 stdio.co
//   import add from mod;                              // 简化：同 mod.co
//   import x from "sub/deep";                         // 跨目录（字符串形式必要）
struct ImportSpecifier
{
	std::string name;   // 非 star 时的符号名；is_star=true 时为空
	std::string alias;  // `as X` 目标名（可空，为空时与 name 同）
	bool is_star = false; // `import *` 全量导入
	SourceLoc loc;
};

struct ImportDecl
{
	std::vector<ImportSpecifier> specs;
	std::string filename;      // 字符串字面量内容（已 co_decode_string 剥前缀/解转义）
	std::string module_alias;  // `as ModAlias` 可选（本文件内引用该模块的限定名）
	SourceLoc loc;
	// P212 loader 解析回填：规范化绝对路径 key，用于跨模块可见性匹配。
	//   parser 产出时为空；loader 在 load_recursive 前填入主源本层 imports 对应项。
	std::string resolved_module_key;
};

// ============================================================
// 翻译单元
// ============================================================
// P304：文件作用域 typedef 的跨模块导出面。
//   colang 的 typedef 本是 parser 私有表的纯 parse 期概念（使用点即展开为底层类型），
//   AST 里不留别名；跨模块要用它，就必须把「已解析的底层类型」本身运过模块边界。
//   本表**全量**记录文件作用域 typedef（含非 public）——种子只挑 public 的出给消费方 parser，
//   全量这份供 sema 在 `no public type` 处判「该名属于哪个模块」。
struct TypedefExport
{
	std::string name;
	CTypePtr type;              // 已展开的底层类型（含 _Alignas/GNU aligned）
	bool is_public = false;
	std::string source_module_key; // parser 产空；loader merge 时回填规范化 key
	SourceLoc loc{};
};

struct TranslationUnit
{
	std::vector<FuncDecl> funcs;
	std::vector<GlobalVar> globals;
	// P256：本模块「public const int 全局名 → 已折常量值」（parser 在全局声明处折叠填充）。
	//   供跨模块 import 时 harvest_const_seeds 采集、在消费方 parse 期把 import 的 public const int
	//   作 case 标签/常量表达式穿透求值（此时被并入主 TU 的 merge_imports 尚未运行）。
	std::vector<std::pair<std::string, std::int64_t>> const_globals;
	std::vector<StructDef> structs;
	std::vector<EnumDef> enums;
	std::vector<ClassDef> classes;  // P300 Co class（0.4.0）
	// P304：文件作用域 typedef 导出面（见上方 TypedefExport）。块作用域 typedef 不入表。
	std::vector<TypedefExport> typedefs;
	// P3+: union decls (folded into structs with is_union flag)
	// P211 Co 层入口模型：文件作用域的裸语句序列（printf/if/for/return/...）
	//   按源码顺序保留，codegen 在合成 main 中依序发射。
	std::vector<StmtPtr> top_level_body;
	// P212 Co 层：顶层 import 声明。driver 在 parse 后递归加载被导入模块、
	//   DFS 后序拓序合并 AST 到主源 TU（本字段保留原始 import 声明供 sema 可见集构造）。
	std::vector<ImportDecl> imports;
	// P212 loader 诊断展示路径（环检测报路径、module not found 拼串用）。
	//   主源 = opts.source_path 文件名；子模块 = 已解析的绝对路径文件名。不依默认行为。仅 loader 写。
	std::string diagnostics_display_path;
	// P212 loader：合并后 top_level_body 头部属于「被导入模块」的语句条数（拓序前插总长）。
	//   主源自身的顶层裸语句 = top_level_body[imported_top_level_body_count .. end)。
	//   sema 走查可见性时对 index < 本字段的项目跳过（视为 imported 内部事务、不查跨模块可见性）。
	std::size_t imported_top_level_body_count = 0;
	// --per-module 探针（路 B）：逐模块顶层裸句分段视图（拓序：最深依赖在前）。
	//   双写字段：top_level_body 照旧摊平前插、既有路径零变动；仅 codegen 分桶模式读此。key＝规范化绝对路径。
	std::vector<std::pair<std::string, std::vector<StmtPtr>>> top_level_segments;
};

using TUPtr = std::shared_ptr<TranslationUnit>;

}  // namespace colang
