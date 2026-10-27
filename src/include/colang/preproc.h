// ============================================================
// preproc.h — 预处理器（P1 全部完成：扫描/切分/条件编译/宏展开/包含/诊断指令）
//
// 方案 A：预处理在 pp-token 层工作。按「逻辑行」从源栈栈顶拉取 token，
// 识别 # 指令、维护 #if 条件栈、维护宏表并展开宏、#include 压入新源；
// 非活跃分支的普通 token 被丢弃，活跃分支的普通 token 展开后转发到输出流。
// ============================================================
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "colang/driver.h"
#include "colang/lexer.h"

namespace colang
{

class Preprocessor
{
public:
	Preprocessor(const CompileOptions &opts, std::vector<Error> &errors);

	// 运行预处理，产出的 token 流填入 out（末尾不含 EndOfFile）。
	bool run(std::vector<Token> &out);

private:
	// 一个活动源（主文件或某层 #include）；Lexer 值持有其文本。
	// 前瞻缓存按源独立，保证 #include 压栈/弹栈时行边界 token 不跨源串位。
	struct Src
	{
		Lexer lexer;
		Token look;          // 本源专属 1-token 前瞻
		bool have_look = false;
		Src(std::string file, std::string text) : lexer(std::move(file), std::move(text))
		{
		}
	};

	// 宏记录。对象式宏（P1.4）与函数式宏（P1.5）均已展开。
	struct Macro
	{
		std::vector<Token> body;          // 替换体（对象式：名后全部；函数式：')' 之后）
		bool is_function_like = false;    // #define NAME( 紧邻左括号 → 函数式
		std::vector<std::string> params;  // 形参名（不含 ...）
		bool is_variadic = false;         // 末尾 ...（__VA_ARGS__）
	};

	// 展开工作序列的元素：一个 pp-token 附带其「蓝铅笔隐藏集」（P174）。
	// 宏体字面 token 沿产生链传递累加；实参代入 token 与 ## 粘贴产物 token 隐藏集重置为空，
	// 使 00201 型 `CAT(A,B)(x)` 末尾的 CAT 得以重新展开，同时互递归 X↔Y 仍终止。
	struct PEntry
	{
		Token tok;
		std::unordered_set<std::string> hide;
	};

	// 一层条件组状态：外层是否全真 / 本组当前分支是否活跃 / 本组是否已命中一次
	struct CondState
	{
		bool parent_active;
		bool branch_active;
		bool any_taken;
	};

	enum class LineResult
	{
		Ok,
		Eof,
		Error
	};

	bool pull(Token &out, bool &at_eof);      // 从栈顶拉一 token（含 1-token 前瞻）
	void unread(const Token &t);              // 退回一个 token
	// 种入「实现预定义宏」（C §6.10.8.3：以下划线开头的名字为实现保留）。
	// 供 colib 标准头做平台/架构分派；当前唯一支持目标 = Windows/x64(MSVC ABI)，
	// 值对齐本机 clang/MSVC ground-truth。Linux 移植时在此按目标切换。
	void seed_predefined_macros();
	// 自动加载：把 colang.h 压入源栈栈顶，先于主源展开；找不到静默跳过。仅主翻译单元调用
	// （import 子模块经 CompileOptions::preload_colang_h=false 排除，避免重复注入）。
	void push_builtin_include(const std::string &name);
	LineResult read_logical_line(std::vector<Token> &line); // 读一个逻辑行
	bool is_active() const;                   // 当前是否处于活跃分支

	bool process_line(const std::vector<Token> &line, std::vector<Token> &out);
	bool handle_directive(const std::vector<Token> &line);
	bool handle_if(const std::vector<Token> &line);
	bool handle_ifdef(const std::vector<Token> &line, bool negate);
	bool handle_elif(const std::vector<Token> &line);
	bool handle_elifdef(const std::vector<Token> &line, bool negate);
	bool handle_else(const std::vector<Token> &line);
	bool handle_endif(const std::vector<Token> &line);
	bool handle_define(const std::vector<Token> &line);
	bool handle_undef(const std::vector<Token> &line);
	bool handle_include(const std::vector<Token> &line);
	// #embed：把外部二进制文件在编译期展开成 0xNN 十六进制 token 序列追加到 out。
	// 与 #include 不同（不压新源），故直接产出 token；支持 limit/prefix/suffix/tail/if-empty。
	bool handle_embed(const std::vector<Token> &line, std::vector<Token> &out);
	bool handle_error(const std::vector<Token> &line);
	bool handle_warning(const std::vector<Token> &line);
	bool handle_pragma(const std::vector<Token> &line);
	bool handle_line(const std::vector<Token> &line);

	// #if / #elif 常量表达式求值（§6.10.1）；成功写 result 返回 true
	bool eval_expr(const std::vector<Token> &line, std::size_t from, std::intmax_t &result);

	// —— 对象式/函数式宏展开（P1.4/P1.5）——
	// 对 in 全序列展开；blue 为当前被隐藏（蓝铅笔）的宏名集，防自递归；
	// in_if 为真时额外保护 `defined X`/`defined(X)` 的操作数不被展开。
	void expand_seq(const std::vector<Token> &in, std::vector<Token> &out,
		const std::unordered_set<std::string> &blue, bool in_if) const;
	// 对活跃分支普通行做宏展开后转发到 out。
	void expand_line(const std::vector<Token> &line, std::vector<Token> &out) const;
	// 从 buf[open]=='(' 起收集实参（顶层逗号分隔、括号嵌套），end 为右括号后一位。
	// 作用于展开工作序列 buf：实参可跨越此前已 splice 拼回的宏体 token（与 clang 一致）。
	bool collect_args(const std::vector<PEntry> &buf, std::size_t open,
		std::vector<std::vector<Token>> &args, std::size_t &end) const;
	// 对函数式宏体做替换，产出逐 token 带隐藏集的序列：普通（非 #/## 操作数）参数的实参先独立
	// 完整宏展开（§6.10.3.1）再代入（hide=∅），# 字符串化与 ## 粘贴用原样实参（paste 结果 hide=∅），
	// 宏体字面 token 取 bodyHide（= 触发宏名 token 的 hide ∪ {本宏}，沿链传递累加）。in_if 透传给实参展开。
	void substitute(const Macro &m, const std::vector<std::vector<Token>> &args,
		std::vector<PEntry> &out, const SourceLoc &at, bool in_if,
		const std::unordered_set<std::string> &bodyHide) const;

	const CompileOptions &opts_;
	std::vector<Error> &errors_;
	std::vector<std::unique_ptr<Src>> src_stack_;
	std::vector<CondState> cond_stack_;
	std::unordered_map<std::string, Macro> macros_;
	std::unordered_set<std::string> pragma_once_; // #pragma once 标记的文件路径（归一化）
	// #pragma push_macro/pop_macro（clang/gcc 扩展，修 plct 00206）：
	//   每个宏名对应一条定义快照栈；nullopt 表示快照时该宏未定义。
	std::unordered_map<std::string, std::vector<std::optional<Macro>>> push_macro_stack_;
};

// 驱动管线入口：运行预处理器，产物写入 state.tokens。
bool preprocess(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors);

}  // namespace colang
