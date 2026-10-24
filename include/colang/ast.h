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
	BitInt,   // _BitInt(N) (C23)
};

struct CType;
using CTypePtr = std::shared_ptr<CType>;

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;   // 前置声明：CType 需携带 VLA 维度表达式

struct CType
{
	TypeKind kind = TypeKind::Int;
	bool is_const = false;

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
};

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr
{
	ExprKind kind = ExprKind::IntLiteral;
	SourceLoc loc;
	CTypePtr type; // sema 填充（P2 阶段可为 null）

	// IntLiteral / CharLiteral
	std::intmax_t int_val = 0;

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
	bool postfix = false; // Unary ++/-- 后缀标志

	// 指定初始化器（C99）
	int desig_index = -1;       // [idx] 数组下标（-1 = 无）
	std::string desig_field;    // .member 成员名（空 = 无）
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
	bool has_lib = false;
	std::string lib_name;         // [[lib("user32")]] 的链接名（不含 .lib 后缀/路径）
};

// ============================================================
// 函数 / 全局变量
// ============================================================
struct Param
{
	std::string name;
	CTypePtr type;
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
	std::string lib_name;    // 空 = 未标注 [[lib]]
	StmtPtr body; // CompoundStmt if definition; nullptr if declaration only
	SourceLoc loc;
	// P212 loader 归属标记：源模块规范化绝对路径 key；空串 = 主源 TU 本体。
	//   仅 loader 在合并前写入；parser/sema/codegen 默认空即视为「主源」。
	std::string source_module_key;
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
	SourceLoc loc;
	std::string source_module_key;  // P212 同 FuncDecl
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
};

struct StructDef
{
	std::string tag;     // struct Name 的 Name
	bool is_union = false;
	bool is_definition = false; // 带 { ... } 的完整定义（false = 仅前置引用，成员表为空不代表空类型）
	std::vector<StructMember> members;
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
	SourceLoc loc;
	bool is_public = false;         // P212
	std::string source_module_key;  // P212
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
struct TranslationUnit
{
	std::vector<FuncDecl> funcs;
	std::vector<GlobalVar> globals;
	std::vector<StructDef> structs;
	std::vector<EnumDef> enums;
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
};

using TUPtr = std::shared_ptr<TranslationUnit>;

}  // namespace colang
