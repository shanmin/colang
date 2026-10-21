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

struct CType
{
	TypeKind kind = TypeKind::Int;
	bool is_const = false;

	// Pointer / Array：元素类型
	CTypePtr base;

	// Array：元素数量（0 = 未指定）
	std::size_t array_size = 0;

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
	StmtPtr body; // CompoundStmt if definition; nullptr if declaration only
	SourceLoc loc;
};

struct GlobalVar
{
	std::string name;
	CTypePtr type;
	ExprPtr init; // nullptr = 无显式初始化
	bool is_static = false;  // InternalLinkage
	bool is_extern = false;  // 纯声明（无定义）
	unsigned align = 0;      // C23 _Alignas 强制对齐（字节；0 = 未指定）
	SourceLoc loc;
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
	std::vector<StructMember> members;
	SourceLoc loc;
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
};

using TUPtr = std::shared_ptr<TranslationUnit>;

}  // namespace colang
