//
//	ast.h
//
#pragma once

#include <vector>
#include <filesystem>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"

struct VARINFO
{
	TOKEN token;
	//std::string name;	//变量名称
	llvm::Type* type;	//变量类型
	llvm::Value* value;
	bool un;			//是否为unsigned类型
	std::string type_source_name;  // 类型的源码级名字（"int"/"uint"/"float"/"Vec*"/"char*"/"bool"/"void"…），用于区分 int vs uint（二者 LLVM Type* 均为 i32），重载决议用；空 = 未赋值（codegen 前会填）
	// 对于指向 struct 的指针变量（如 this），记录其指向的 struct 类型
	//   LLVM 23 opaque pointer 无法从指针类型反推 element type，需显式携带
	llvm::StructType* pointee_st = nullptr;
};
struct VARLIST
{
	//变量范围
	//	global		全局
	//	function	函数
	//	codeblock	局部
	std::string zone;
	//变量列表
	std::map<std::string, VARINFO> info;
	// 栈 class 对象（有析构候选）的声明顺序：反序用于作用域退出时析构。
	//   存变量名（字符串），scope::set 时按条件追加；generate_dtor_calls 时按名从 info 查到 VARINFO。
	//   用字符串比 VARINFO 拷贝轻量，且 save/restore 深拷贝同步方便。
	std::vector<std::string> dtor_order;
};
extern std::vector<VARLIST> varlist;

// import 名称列表条目：原名 + 可选别名（方法别名；空=无别名，调用方按原名用）
//   仅用于函数选择性导入；struct/class 不支持别名（改名会破坏 LLVM link 类型一致性）
struct import_name_entry {
    std::string orig;
    std::string alias;  // 空=无别名,调用方按原名用
};

//变量作用域
class scope
{
public:
	static void push(std::string zone);
	static void pop();
	static void set(VARINFO vi);
	static VARINFO get(TOKEN token);

	// 栈 class 对象析构：
	//   generate_dtor_calls_topscope(): 对当前最顶层作用域（varlist.back()）内按声明反序
	//     对每个"栈 class 对象"查找 ClassName.__dtor，存在则生成 call dtor(load slot→this)。
	//     zone="global" 时跳过（全局对象本轮不支持自动析构）。
	//     当前 BB 已带 terminator 则跳过（避免在已 ret 的 BB 后塞指令）。
	static void generate_dtor_calls_topscope();
	// generate_all_dtor_calls_to_leave_functions(): 供 AST_return / sweep 补 ret 用。
	//   从 varlist 最顶层 downto index 1（index 0 = global，不析构），每层依次调 generate_dtor_calls_topscope。
	//   结果：函数内部所有作用域栈对象按"最内→最外"反序完成析构。
	static void generate_all_dtor_calls_to_leave_functions();

	//循环跳转栈：break N / continue N（N = 字面量正整数，默认 1）
	//仅 AST_for / AST_do / AST_while 构造其 codegen 中 push，codegen 尾 pop（RAII-free，手动匹配）
	//N=1 对应最内层（stack.back()），N=2 对应 stack[size-2]，以此类推
	static void push_loop_bb(llvm::BasicBlock* brk, llvm::BasicBlock* cont);
	static void pop_loop_bb();
	//返回栈中第 N 层（1-based）目标 BB；若非法（stack 空 / N>depth / N=0）用 tok（break/continue 关键字 token）带诊断 ErrorExit
	static llvm::BasicBlock* get_break_bb(unsigned levels, const TOKEN& tok);
	static llvm::BasicBlock* get_continue_bb(unsigned levels, const TOKEN& tok);

	//struct 类型注册表：名字空间与变量名字空间共享（结构体名与变量名不能重复）
	//  - register_struct_type        ：struct AST codegen 完 setBody 之后注册；若"不同 StructType* 已占同名"报错；若"opaque 前向同指针"接受
	//  - register_struct_type_forward：ir_type 解析 struct T 时若查不到，先 forward 登记 opaque struct（允许 struct S* self 前向引用字段）
	//  - has_var_any_scope           ：查"任何作用域是否已存在同名变量"（struct 注册时互斥用）
	//  - clear_all_compile           ：单编译结束后重置全局 struct/field 表，避免两次 co2bc 复用旧 StructType* （process 内多源同名字段索引污染）
	static bool                     has_struct_type(const std::string& name);
	static llvm::StructType*        get_struct_type(const std::string& name);
	static void                     register_struct_type(llvm::StructType* st, const TOKEN& name_tok);
	static void                     register_struct_type_forward(llvm::StructType* st, const std::string& sname, const TOKEN& diag_tok = TOKEN());
	// 注册某 struct 各字段名→序号（AST_struct::codegen 内部使用）
	static void                     set_struct_field_indexes(llvm::StructType* st, const std::vector<TOKEN>& field_names);
	// 上一条的 string 重载（import struct 快照 merge 场景下无 TOKEN 可用，仅需要字段名）
	static void                     set_struct_field_indexes(llvm::StructType* st, const std::vector<std::string>& field_names);
	//返回字段序号：若字段不存在，返回 (unsigned)-1 供调用方判断是否 ErrorExit
	static unsigned                 get_struct_field_index(llvm::StructType* st, const std::string& field_name);
	static bool                     has_var_any_scope(const std::string& name);
	// 标记某个 struct/class 的 import 侧可见性：true=public（import 方 forward 注册），false/private 不可见
	//   - AST_class codegen：顶层写 public class → 标 true；private class 或省略 → 标 false
	//   - struct 默认 false（保持旧行为，不自动跨 import 可见）；若将来支持 public struct，再入口写 true
	static void                     mark_struct_import_public(const std::string& sname, bool is_public);
	static bool                     is_struct_import_public (const std::string& sname);
	// true iff 当前 Module 已有名为 name 的 GlobalValue（function 或 global var），用于 struct 注册时与"已存在的全局函数名"冲突检测
	static bool                     has_global_function_or_ivar(const std::string& name);
	static void                     clear_all_compile();

	// 嵌套 import 编译的 scope 快照 save/restore：避免嵌套 ir() 里 clear_all_compile 毁掉外层
	//   save_state() 返回一个不透明句柄（调用方用 void* 持有），调用 restore_state 时原子 swap 回
	//   并释放句柄内存；不能调两次 restore（同一句柄）。
	static void*                    save_state();
	static void                     restore_state(void* state);

	// 嵌套 import 编译辅助：在 inner scope 仍有效时（restore 之前），从 state（或当前全局表）
	//   中提取"当前 scope 中的 struct 类型 + 字段索引"快照，返回不透明句柄（堆 struct）。
	//  然后在 restore 完成（outer scope 已恢复）后，把快照合并进 outer scope：
	//   - struct 名已存在（outer 同名定义）→ 跳过（不覆盖）
	//   - struct 名不存在 → 以相同名字 create/get 一个 Opaque StructType（与 inner 的同名
	//     StructType 由 LLVM llvm-link 按名字合并，IR 层保持一致），并 forward 注册到 outer
	//   - 字段索引同步：outer 的同名 struct 若字段定义顺序相同（由 linker 保证），
	//     直接复制 inner 的字段名→索引映射供 outer 解析 GEP 使用。
	//  目的：import 方编译 struct/class 相关代码（new/字段访问/方法调用）时能查到类型名
	//        → 不报错"未定义的类型名"。
	static void*                    snapshot_struct_scope_from_current();
	// merge import 模块的 struct/class 类型注册到主模块作用域
	//   names: selective 模式时的白名单（star=true 时忽略）；star: 是否全量导入
	//   struct/class 只按原名匹配白名单，不支持别名（改名破坏 LLVM link 类型一致性）
	static void                     merge_imported_struct_scope(void* snap_handle, const TOKEN& diag_tok,
	                                              const std::vector<import_name_entry>& names, bool star);
};

struct LABEL_LIST
{
	std::map<std::string, llvm::BasicBlock*> info;
};


extern std::unique_ptr<llvm::IRBuilder<>> ir_builder;
extern llvm::Module* ir_module;
extern llvm::LLVMContext ir_context;

//extern std::vector<VAR_LIST> ir_varlist; //局部变量范围
extern std::vector<LABEL_LIST> ir_labellist; //局部标签


////////////////////////////////////////////////////////////////////////////////
//
//	AST
//
////////////////////////////////////////////////////////////////////////////////

class AST
{
public:
	virtual llvm::Value* codegen() = 0;			//生成代码
	virtual void show(std::string pre) = 0;		//显示代码
	virtual bool is_un() { return false; }		//运算数是否为无符号类型（默认有符号），用于选择 udiv/udiv 类指令
	//若当前节点是「返回类型 int + 名 main + 无参数」的函数定义 → 返回 true；用于顶层 ir() 决定要不要预建空 main 占位。
	//默认 false；只有 AST_function 按自身存储的 name/ret 检查后返回 true。
	virtual bool is_int_main_function_def() { return false; }
};

class AST_call :public AST
{
	TOKEN name;
	std::vector<AST*> args;
	// 模块限定前缀（如 m1.aa() 中的 "m1"）；空串=无前缀，走三步查找链
	std::string module_prefix;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_call(std::vector<TOKEN>& tokens);
};

class AST_codeblock :public AST
{
	std::vector<AST*> body;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_codeblock(std::vector<TOKEN>& tokens);
};

class AST_do :public AST
{
	TOKEN name;
	AST* expr = NULL;
	AST* body = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_do(std::vector<TOKEN>& tokens);
};

class AST_expr :public AST
{
public:
	TOKEN op;
	AST* left;
	AST* right;
public:
	//int op_pri;
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	bool is_un() override { return left->is_un(); } //结果符号性继承左操作数
	//AST_expr();
	//AST_expr(std::vector<TOKEN>& tokens);
	AST_expr(AST* left, TOKEN op, AST* right) :left(left), op(op), right(right) {};
	friend std::pair<llvm::Value*, llvm::Type*>
		ast_expr_field_address_and_type(AST_expr* dot_expr);
};

//一元前缀运算符（~ 位取反；将来扩展 - 负号、! 逻辑非）
//  codegen：~x 等价 x ^ all-ones，整型用 CreateNot；浮点不支持（报错）
//  is_un：位运算不改符号性，继承操作数
class AST_unary :public AST
{
	TOKEN op;
	AST* operand;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	bool is_un() override { return operand->is_un(); }
	AST_unary(TOKEN op, AST* operand) :op(op), operand(operand) {}
};

class AST_if :public AST
{
	TOKEN name;
	AST* expr1 = NULL;
	AST* thenbody = NULL;
	AST* elsebody = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_if(std::vector<TOKEN>& tokens);
};

class AST_label :public AST
{
	TOKEN name;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_label(std::vector<TOKEN>& tokens);
};

class AST_for :public AST
{
	TOKEN name;
	AST* var = NULL;//变量定义
	AST* expr1 = NULL;
	AST* expr2 = NULL;
	AST* expr3 = NULL;
	AST* body = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_for(std::vector<TOKEN>& tokens);
};

class AST_function :public AST
{
	std::vector<TOKEN> rett;
	TOKEN name;
	std::vector<TOKEN> args;
	//std::vector<AST*> body;
	AST* body=NULL;
	// 访问修饰符：true = private（默认，InternalLinkage 仅模块内可见），false = public（ExternalLinkage 跨模块可见）
	//   main 入口函数强制 ExternalLinkage，不受此字段影响（入口点必须对外可见）
	bool is_private = true;
public:
	// 类方法上下文：非空表示此类方法属于 class_name 指定的类
	//   codegen 时自动 mangle 为 "class_name.method_name"，并注入 this 指针为第一参数
	std::string class_name;
	// 是否为构造函数（name == class_name 时自动设置）
	bool is_constructor = false;
	// 是否为析构函数（~ClassName 时 AST_class 设置）
	bool is_destructor = false;
	// —— 函数重载登记缓存（构造结束后填充；codegen 阶段读）——
	//   mangled 名登记为「与实际 LLVM Function 符号名完全一致」，保证 Step 5 resolve 结果 getFunction 直接命中
	std::string self_mangled_name;
	//   为 true 表示构造阶段已完成登记（嵌套 ir() 场景 / 多次 codegen 时不再重复登记）
	bool        overload_registered = false;
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	bool is_int_main_function_def() override;
	AST_function(std::vector<TOKEN>& tokens);
};

class AST_goto :public AST
{
	TOKEN name;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_goto(std::vector<TOKEN>& tokens);
};

class AST_noncode :public AST
{
	TOKEN value;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_noncode(std::vector<TOKEN>& tokens);
};

class AST_struct :public AST
{
	TOKEN name;
	// 顶层可见性修饰符：方案 B 与 class 对称；没写/private → import 侧不可见；显式 public → import 侧可见
	//   （由 AST_struct 构造函数解析 tokens 开头的 public/private；没有修饰符时默认 false，与 B 方案保持一致。）
	bool toplevel_public = false;
public:
	//字段类型 token 列表（每字段 1 个 vector<TOKEN>；ir_type() 可直接消费：支持 int / int* / struct T / struct T*）
	std::vector<std::vector<TOKEN>> field_types;
	//字段名 token（与 field_types 按索引对齐）
	std::vector<TOKEN> field_names;

	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_struct(std::vector<TOKEN>& tokens);
};

class AST_return :public AST
{
	TOKEN token;
	AST* value = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_return(std::vector<TOKEN>& tokens);
};

class AST_value :public AST
{
	llvm::Value* _value; //解析后的常量
public:
	TOKEN value;
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	bool is_un() override;
	AST_value(TOKEN token) :value(token) {}
	AST_value(std::vector<TOKEN>& tokens);
};

class AST_var :public AST
{
	std::vector<TOKEN> type;
	TOKEN name;
	// 可选：parse 时发现 mod.ClassName 形式 → 保存 mod 名（空串 = 无模块限定）
	//   codegen 阶段（ir() 顺序遍历 AST，此时 import 已 codegen 过并登记了 import_register_module）
	//   里校验 mod 是否已 import；构造阶段不做 import 注册表校验（注册表那时还没填）。
	TOKEN mod_prefix_tok;
	// ---- 栈分配构造函数初始化式：CLASS c = CLASS(args); ----
	//   has_ctor_init=true 时 init_args 非空（含 0 个参数 = 无参构造）
	bool has_ctor_init = false;
	TOKEN init_class_name_tok;      // = ClassName(args) 中的 ClassName
	TOKEN init_mod_prefix_tok;       // 初始化式中可选 mod. 前缀；空=无
	std::vector<AST*> init_args;    // 参数表达式 AST 列表
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_var(std::vector<TOKEN>& tokens);
};

class AST_while :public AST
{
	TOKEN name;
	AST* expr = NULL;
	AST* body = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_while(std::vector<TOKEN>& tokens);
};

class AST_break :public AST
{
	TOKEN kw;           // break 关键字 token（用于报错定位）
	unsigned levels;    // 默认 1；仅支持十进制无符号字面量正整数（构造期校验）
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_break(std::vector<TOKEN>& tokens);
};

class AST_continue :public AST
{
	TOKEN kw;           // continue 关键字 token
	unsigned levels;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_continue(std::vector<TOKEN>& tokens);
};

class AST_import :public AST
{
public:
	TOKEN import_tok;     // import 关键字 token（用于报错定位）
	std::string filename; // 解析后的文件名（已补 .co、相对主文件 dir 解析完成）
	// 名称列表:{原名, 别名(空=无别名)};star=true 时表示 import *（names 此时为空）
	std::vector<import_name_entry> names;
	bool star = false;
	// 模块别名:from "x" as B 中的 B（空=无别名,用原文件 stem 作模块名）
	//   起别名后原模块名不可用于限定调用（只登记别名→stem 映射,不登记 stem）
	std::string module_alias;

	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_import(std::vector<TOKEN>& tokens);
};

class AST_class :public AST
{
	TOKEN name;
	// 字段（同 AST_struct）
	std::vector<std::vector<TOKEN>> field_types;
	std::vector<TOKEN> field_names;
	// 类方法（含构造函数）
	std::vector<AST_function*> methods;
	// 顶层可见性修饰：true=写了 public class，允许 import 方 new/用类型名；false=private class 或未写（默认）
	bool toplevel_public = false;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_class(std::vector<TOKEN>& tokens);
};

class AST_new :public AST
{
	TOKEN class_name_tok;
	std::vector<AST*> args;
	// 可选模块前缀（空 Value = 没限定）。构造函数只存前缀不校验，codegen() 阶段查 import 注册表
	//   理由与 AST_var::mod_prefix_tok：parse 阶段模块尚未 codegen，import_register_module 是空。
	TOKEN mod_prefix_tok;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_new(std::vector<TOKEN>& tokens);
};

// delete 表达式：delete 指针变量; → 先调 ClassName.__dtor 再 free
class AST_delete :public AST
{
	AST* expr;           // delete 后面的表达式（通常是变量名引用）
	TOKEN var_name_tok;  // 若 delete 后第一个是 code（变量名），保存下来用于查 VARINFO.pointee_st 反推类名
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_delete(std::vector<TOKEN>& tokens);
};


////////////////////////////////////////////////////////////////////////////////
//
//	import 全局基础设施（跨 co2m 调用共享）
//
////////////////////////////////////////////////////////////////////////////////

//解析 import 路径：相对当前源文件所在目录解析目标文件名；若目标无扩展名自动补 .co
//	cur_source_file：当前执行 import 的 .co 路径（绝对或相对均可）
//	import_target  ：import 语句字符串 token 内容（不含引号）
//	diag_tok       ：用于报错定位的 token（建议传字符串 token）
//返回规范化后的绝对路径（带 \ 分隔符），失败 ErrorExit 不返回
std::string import_resolve_path(const char* cur_source_file, const std::string& import_target, const TOKEN& diag_tok);

//取 .co 文件对应的"模块名"：去掉路径和扩展名（test.co / a/b/c.co → test / c）
//	用户符号 mangling 用：模块名作为前缀（如 @模块名.函数名）
std::string import_module_name(const std::string& co_path);

//查询/注册"该 .bc 是否已生成并加入链接清单"（去重 + 防环）
//	true  = 已处理过，跳过；false = 首次，调用方生成并 add_bc_to_link_list
bool import_has_processed(const std::string& abs_co_path);
void import_mark_processed(const std::string& abs_co_path);

//向最终链接清单追加一个 .bc 文件（绝对路径）；主模块 .bc 也需加入
void import_add_bc_to_link_list(const std::string& abs_bc_path);

//写出 import_list.txt（主 .co 同目录，每行一个 bc 绝对路径）供批处理消费
//	只有顶层 co2m 调用结束时写一次；嵌套 import 调用不写
void import_write_manifest(const char* main_co_path);

//重置所有 import 全局状态（供 clear_all_compile 调用或未来测试调用）
void import_clear_state();


////////////////////////////////////////////////////////////////////////////////
//
//	OTHER
//
////////////////////////////////////////////////////////////////////////////////

std::vector<AST*> ast(std::vector<TOKEN>& tokens);
AST* ast1(std::vector<TOKEN>& tokens);
AST* ast_parse_expr(std::vector<TOKEN>& tokens, int left_pri = 0, AST* left = NULL);
AST* ast_parse_expr1(std::vector<TOKEN>& tokens);
AST* ast_parse_expr_add1(AST* old, std::vector<TOKEN>& tokens);

void ast_echo(std::vector<AST*> ast_list, std::string pre);

llvm::Type* ir_type(std::vector<TOKEN>& tokens);
bool ir_type_unsigned(const std::string& name); //判断类型名是否为无符号整数类型（byte/ushort/uint/ulong）
bool is_compound_assign(const std::string& op); //判断是否为复合赋值运算符（+= -= *= /=）
llvm::Value* ir_type_conver(llvm::Value* value, llvm::Type* to, bool is_src_un = false);

//字面量赋值范围检查（方案 B：warning 不改退出码）。
//在 "= / += / ..." 中，当右侧是一个编译期可识别的字面量（ConstantInt / ConstantFP，
//AST_value number token 来源）时调用：
//  target_type_name : 类型名（如 "int"/"uint"/"float"/"half"…，原样传给警告文案）；
//  target_llvm_type : 变量的 LLVM 类型（含位宽、isFloatingPointTy 判定）；
//  target_un       : 是否无符号整型（类型名决定，整型用，浮点忽略）；
//  lit_token       : 字面量原始 token（含 row/col 定位 + Value 原始字符串：十/十六进制、含 '.' 为浮点）；
//  trunc_ap        : 截断后的 iN 整型值（用于告警打印"将变成 X"）。
void check_literal_assignment_range(
	const std::string& target_type_name,
	llvm::Type* target_llvm_type,
	bool target_un,
	const TOKEN& lit_token);

void ir(std::vector<AST*>& ast_list, const char* filename);

//将 co2m / ir 标记为"嵌套调用"（import 时用）：当前 ir() 结尾
//  - 不创建 main 函数（导入文件不该有自己的 main）
//  - 不写 .ll/.bc 文件（由调用方 / 独立 co2bc 调用写）
//  - 不 verify 后不调用 exit()（嵌套报错仍可用 ErrorExit 终止）
void ir_set_nested_mode(bool nested);
bool ir_is_nested_mode();

//获取/设置"当前模块名"：进入 ir() 前设置，codegen 用做符号 mangling 前缀
//	空字符串=顶层模式（不 mangle，兼容旧行为：main + 顶层函数不改名）
void ir_set_current_module_name(const std::string& mod);
const std::string& ir_get_current_module_name();

//对"用户定义的符号"（函数名 / 全局变量名）做 mangling：
//  若当前非顶层（模块名非空）且符号不是 printf 等保留 extern → 返回 "模块名.原名"
//  否则（顶层 / 保留 C extern）返回原名
std::string ir_mangle_symbol(const std::string& user_name);

//用户符号查找（code 层）：AST_call（函数调用）、AST_var（变量用）、scope::set/get（变量注册/查找）都走这里。
//  逻辑：① scope 当前作用域查原名（先查同模块变量，兼容顶层不变）
//        ② 对于符号名解析（函数 call name / 全局变量 access）：若找不到原名 + 当前模块存在，自动尝试 mangled 名
//  为避免改动过大，先在 AST_call 的 codegen 用此包装做 getFunction 查找；后续变量等再陆续接入。
//  对 call name：若 mangled 名找到 Function，用之；否则退回原名（顶层 / 外部 C printf）
llvm::Function* ir_find_function_or_nul(const std::string& call_name);
//注册函数到外部符号映射：import B 成功后，把 B.mangle 记录下来，其他模块才能按"模块.符号"找到
//  （当前不做显式 import 注册表，直接靠模块名 + mangling 在 getFunction 中找 ——
//   但 import 进来的符号在本 module 中并不存在，getFunction 只能拿到 ir_module 内的）
//  → 更简单做法：AST_import codegen 结束后，把 import 模块的函数声明复制/引用到调用方 ir_module
//    （ExternalLinkage + Declaration 即可），这样 ir_module->getFunction("mod.fn") 一定能查到
//  下面工具：把 source_module 中所有非 main、非 printf 的 ExternalLinkage 函数声明以 declaration 形式加入 dst_module
//    （不复制 body，只声明 —— 链接时 llvm-link 会合并 body）
//    names/star: selective 模式白名单（star=true 忽略白名单全量导入）；
//    注册表 key = 别名(若有)否则原名；selective 模式找不到的原名会 ErrorExit
void ir_import_external_decls(llvm::Module& src, llvm::Module& dst,
    const std::vector<import_name_entry>& names, bool star, const TOKEN& diag_tok);

// import 模块名注册表：AST_import codegen 成功加载模块后登记模块名（含去重），
//  供 AST_var / AST_new 中遇到 "模块名.类型名" 限定写法时校验"模块前缀真的 import 过"。
//   - import_register_module(name)：幂等登记（多次 import 同名模块 OK）
//   - import_has_registered_module(name)：true=已 import 过
//   - import_clear_registered_modules()：同进程多编译结尾清，避免串（走 import_clear_state）
void import_register_module(const std::string& name);
bool import_has_registered_module(const std::string& name);
void import_clear_registered_modules();

// 模块别名登记/查询:from "x" as B → 登记 alias B → stem x
//   - import_register_module_alias(alias, stem):起别名时调（不登记 stem 到 g_registered_module_names,
//     故原名不可用于限定调用;alias→stem 单独存于别名表）
//   - import_resolve_module_alias(name):返回 name 对应的 stem;若 name 是已登记别名返回其 stem,
//     否则原样返回 name（调用方再走 import_has_registered_module 校验是否为原模块名）
//   - import_clear_module_aliases():清别名表（import_clear_state 内部调）
void import_register_module_alias(const std::string& alias, const std::string& stem);
std::string import_resolve_module_alias(const std::string& name);
void import_clear_module_aliases();
//VARINFO ir_var(std::string name, std::vector<VARLIST> var_list, TOKEN token);
//llvm::Value* ir_var_load(VARINFO& var_info);


////////////////////////////////////////////////////////////////////////////////
//
//  函数重载基础设施 —— 轻量类型表示 TCType + 注册表 API
//
////////////////////////////////////////////////////////////////////////////////

// 轻量前端语义类型：与 LLVM Type* 解耦，重点补充 source_name 区分 int/uint 等符号性差异
struct TCType
{
	std::string source_name;  // 源码名（"int"/"uint"/"float"/"Vec*"/"char*"/"void"/"bool"/""）：空表示无法回溯（codegen 期合成的 Value）
	llvm::Type* ty = nullptr; // 对应 LLVM 类型
	bool        un = false;   // 整型是否无符号（浮点/void/bool/ptr/struct 忽略）
};

// 两类型是否"语义相等"：
//   - 两者 source_name 都非空时优先比 source_name（可区分 int vs uint）
//   - 否则比 LLVM Type 指针相等 + un 相同（struct/ptr 类型的 source_name 可能为空）
bool tctype_equal(const TCType& a, const TCType& b);

// 取可读字符串（用于错误诊断）：优先 source_name，否则退化为 LLVM 打印
std::string tctype_str(const TCType& t);

// ——— 重载 bucket 注册表 ———
//  （overload_map 存于 ast.cpp 全局，以下 API 全部声明 extern）

// 单一重载条目（存储于 bucket 中）
struct OverloadEntry
{
	std::string     raw_name;   // 用户书写的函数名（不含 class/mangle 前缀）
	std::string     mangled;    // 实际 LLVM 符号名（含参数签名编码）
	std::vector<TCType> params; // 用户参数（不含 this 实现细节）
	TCType          ret;        // 返回类型
	bool            is_vararg = false;
	std::string     class_name; // 空 = 顶层/模块函数；非空 = 类方法（bucket_key = Class##method）
	bool            is_ctor = false;   // 是否构造函数（raw_name 被替换为 __init__）
	bool            is_dtor = false;   // 是否析构函数（raw_name 被替换为 __dtor）
	bool            is_public_entry = true; // import 时：仅 ExternalLinkage=true 的函数合并到调用方
};

// 生成 bucket_key：普通函数 = raw_name；类方法 = class_name + "##" + raw_name；模块限定 = mod_prefix + "##" + raw_name
std::string overload_bucket_key(const std::string& raw_name, const std::string& class_name, const std::string& mod_prefix = "");

// 统一 mangled 名编码：唯一标识一个重载（供 Function::Create 使用）
//   格式：__co_{name}{class_suffix}_{N}_{typecodes}[_vararg]
//     typecode: vi=void i8c i16s i32i i64l u8b u16us u32ui u64ul b1 f16h f32f f64d pN(ptr to N 递归) sName(struct name)
//   构造函数：raw_name 自动替换为 "__init__"
//   顶层模式（ir_get_current_module_name() 非空）：最终 mangled 名还要再套 ir_mangle_symbol 的模块前缀
std::string overload_mangle(const std::string& raw_name, const std::vector<TCType>& params, bool is_vararg, const std::string& class_name, bool is_ctor);

// 登记一个重载条目：按 bucket_key 追加；同 bucket 内若存在"签名完全相同"（params tctype_equal 全相等 + ret equal + is_vararg 同 + class_name 同）则 ErrorExit diag_tok
void overload_register(const OverloadEntry& entry, const TOKEN& diag_tok = TOKEN());

// 重载决议：给定实参类型列表，从 bucket 内挑选最佳重载。返回 mangled 名；
//   out_found=true：有 bucket 参与了决议（无论是否匹配）；out_found=false：bucket 为空，调用方应走旧三步链（兼容 printf 等未登记的 C extern）
//   失败走 ErrorExit（ambiguous / not found）
std::string overload_resolve(const std::string& bucket_key, const std::vector<TCType>& arg_types, const TOKEN& diag_tok, bool* out_found = nullptr);

// 返回 bucket 内全部候选签名的可读串（一行一个，用于报错候选列表展示）
std::string overload_candidates_str(const std::string& bucket_key);

// Function::Create 后同步最后一条目的 mangled 名（LLVM 可能因同名冲突自动重命名 .1/.2）
//   raw_name 应与 overload_register 时的 entry.raw_name 一致（构造函数已是 "__init__"）
void overload_sync_last_mangled(const std::string& raw_name, const std::string& class_name,
                                const std::string& new_mangled);

// 重置 overload_map（import_clear_state / clear_all_compile 调用）
void overload_clear();

// import / scope 快照用：取出当前 overload_map 的深拷贝（不透明 void*），restore 时原子 swap 回
void* overload_save_state();
void  overload_restore_state(void* state);
// 从内层快照合并到当前（import 场景：非 public entry 跳过；同签名重复跳过）—— 返回实际合并条目数
size_t overload_merge_from_snapshot(void* snap_handle, const TOKEN& diag_tok);
void   overload_free_snapshot(void* snap_handle);


//	THE END