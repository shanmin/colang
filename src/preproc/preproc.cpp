// ============================================================
// preproc.cpp — 预处理器实现（P1.1–P1.7 全部完成）
//
// 拉模型：按逻辑行从源栈栈顶取 pp-token；# 指令分派；#if 族维护条件栈；
// #define/#undef 维护宏表；#include 压入新源（源 EOF 自动弹栈回父源）；
// 活跃分支的普通行经宏展开（对象式/函数式、# 字符串化、## 粘贴、变参）后转发到输出。
// ============================================================

#include "colang/preproc.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace colang
{

namespace
{

// —— #include 路径解析与文件读取 ——
// 取路径的目录部分（含末尾分隔符）；无目录则返回空串。
std::string path_dirname(const std::string &path)
{
	const std::size_t pos = path.find_last_of("/\\");
	if (pos == std::string::npos)
	{
		return std::string();
	}
	return path.substr(0, pos + 1);
}

// 拼接目录与文件名；dir 为空则原样返回 name。
std::string path_join(const std::string &dir, const std::string &name)
{
	if (dir.empty())
	{
		return name;
	}
	const char tail = dir.back();
	if (tail == '/' || tail == '\\')
	{
		return dir + name;
	}
	return dir + "/" + name;
}

// 读整个文件为字符串；剥除 UTF-8 BOM。失败返回 false。
bool read_whole_file(const std::string &path, std::string &out)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
		static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
	{
		out.erase(0, 3);
	}
	return true;
}

// 原始读取（不剥 BOM，供 #embed 取真实字节）
bool read_whole_file_raw(const std::string &path, std::string &out)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	return true;
}

// 头名搜索（同 #include 策略）：quote 先查当前文件目录、再 -I、再裸名；angle 仅 -I。
// 命中返回 true 并写 out_path。
bool resolve_header_path(const std::string &name, bool quote_form, const std::string &at_file,
	const CompileOptions &opts, std::string &out_path)
{
	std::vector<std::string> candidates;
	if (quote_form)
	{
		candidates.push_back(path_join(path_dirname(at_file), name));
	}
	for (const std::string &dir : opts.include_dirs)
	{
		candidates.push_back(path_join(dir, name));
	}
	if (quote_form)
	{
		candidates.push_back(name);
	}
	for (const std::string &cand : candidates)
	{
		std::ifstream t(cand, std::ios::binary);
		if (t)
		{
			out_path = cand;
			return true;
		}
	}
	return false;
}

// 字符串字面量内容：去类型前缀（L/u8/u/U）与外围引号；转义按原样保留
// （供 #embed prefix/suffix/tail 的字符内容重切分为 pp-token）。
std::string string_literal_inner(const std::string &lit)
{
	std::string s = lit;
	while (!s.empty() && (s.front() == 'L' || s.front() == 'u' || s.front() == 'U'))
	{
		if (s.front() == 'u' && s.size() > 1 && s[1] == '8')
		{
			s.erase(0, 2);
			continue;
		}
		s.erase(0, 1);
	}
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
	{
		s = s.substr(1, s.size() - 2);
	}
	return s;
}

// 路径归一（用于 #pragma once 比较）：统一正斜杠 + 转小写（Windows 不区分大小写）
std::string normalize_path(const std::string &p)
{
	std::string out;
	out.reserve(p.size());
	for (char c : p)
	{
		if (c == '\\')
		{
			out += '/';
		}
		else
		{
			out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	return out;
}

// 拼合 line[from..] 的 token 文本为消息串（#error / #warning 用）
std::string gather_msg(const std::vector<Token> &line, std::size_t from)
{
	std::string msg;
	for (std::size_t i = from; i < line.size(); ++i)
	{
		if (!msg.empty())
		{
			msg += ' ';
		}
		msg += line[i].text;
	}
	return msg;
}

// —— pp-number → 整数（供 #if 求值）——
// 支持 0x/0X、0b/0B、八进制（前导 0）、十进制，尾部 u/U/l/L 后缀；
// 含 '.' 或指数（e/p）等非整数形式者返回 false。
bool parse_pp_integer(const std::string &s, std::intmax_t &out)
{
	const std::size_t n = s.size();
	if (n == 0)
	{
		return false;
	}
	std::size_t start = 0;
	int base = 10;
	if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
	{
		base = 16;
		start = 2;
	}
	else if (n >= 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
	{
		base = 2;
		start = 2;
	}
	else if (n >= 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O'))
	{
		base = 8;
		start = 2;
	}
	else if (n >= 2 && s[0] == '0' && s[1] >= '0' && s[1] <= '7')
	{
		base = 8;
		start = 1;
	}

	auto digit_ok = [base](char c) -> bool {
		switch (base)
		{
		case 2:
			return c == '0' || c == '1';
		case 8:
			return c >= '0' && c <= '7';
		case 16:
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
				(c >= 'A' && c <= 'F');
		default:
			return c >= '0' && c <= '9';
		}
	};

	std::size_t k = start;
	while (k < n && digit_ok(s[k]))
	{
		++k;
	}
	if (k == start)
	{
		return false; // 前缀后无有效数字
	}
	const std::string digits = s.substr(start, k - start);
	while (k < n && (s[k] == 'u' || s[k] == 'U' || s[k] == 'l' || s[k] == 'L'))
	{
		++k; // 整型后缀
	}
	if (k != n)
	{
		return false; // 后缀后仍有字符：非整数常数
	}
	errno = 0;
	const unsigned long long v = std::strtoull(digits.c_str(), nullptr, base);
	out = static_cast<std::intmax_t>(v);
	return true;
}

// —— #if 常量表达式递归下降求值器（§6.10.1）——
// 档序由低到高：?: → || → && → | → ^ → & → ==/!= → 关系 → 移位 → 加减 → 乘除 → 一元 → 本项
class CondExpr
{
public:
	CondExpr(const std::vector<Token> &toks, std::size_t begin, std::size_t end,
		std::function<bool(const std::string &)> is_defined,
		std::function<bool(const std::string &)> has_include = nullptr,
		std::function<bool(const std::string &)> has_c_attribute = nullptr)
		: toks_(toks), i_(begin), end_(end), is_defined_(std::move(is_defined)),
		  has_include_(std::move(has_include)), has_c_attribute_(std::move(has_c_attribute))
	{
	}

	std::intmax_t eval()
	{
		const std::intmax_t v = parse_conditional();
		return ok_ ? v : 0;
	}
	bool ok() const { return ok_; }
	bool consumed() const { return i_ == end_; }
	std::size_t index() const { return i_; } // 停止位置（trailing token 定位用）
	const std::string &error() const { return err_; }

private:
	bool at_end() const { return i_ >= end_; }
	const Token &cur() const { return toks_[i_]; }
	bool is_punct(const char *p) const
	{
		return !at_end() && cur().kind == TokKind::Punct && cur().text == p;
	}
	bool eat_punct(const char *p)
	{
		if (is_punct(p))
		{
			++i_;
			return true;
		}
		return false;
	}
	bool is_name() const
	{
		return !at_end() && (cur().kind == TokKind::Identifier || cur().kind == TokKind::Keyword);
	}

	std::intmax_t fail(const std::string &msg)
	{
		if (ok_)
		{
			ok_ = false;
			err_ = msg;
		}
		return 0;
	}

	std::intmax_t parse_conditional()
	{
		const std::intmax_t cond = parse_or();
		if (!ok_ || !is_punct("?"))
		{
			return cond;
		}
		++i_;
		// ?: 短路（§6.5.15）：仅“被选中分支”正常求值，“未选中分支”仍解析以校验
		// 语法但抑制其求值期错误（如 0/0）。c-testsuite 00145：`-1 ? 3 : (0/0)`。
		const bool take_mid = (cond != 0);
		const bool saved = suppress_value_error_;
		suppress_value_error_ = (saved || !take_mid);
		const std::intmax_t mid = parse_conditional();
		suppress_value_error_ = saved;
		if (!ok_)
		{
			return 0;
		}
		if (!eat_punct(":"))
		{
			return fail("expected ':' in conditional expression");
		}
		suppress_value_error_ = (saved || take_mid);
		const std::intmax_t rhs = parse_conditional();
		suppress_value_error_ = saved;
		if (!ok_)
		{
			return 0;
		}
		return take_mid ? mid : rhs;
	}

	// 二元档：调用下一档并在匹配运算符处左结合折叠
	std::intmax_t parse_binary(std::intmax_t (CondExpr::*next_level)(), const char *const *ops,
		std::intmax_t (*combine)(std::intmax_t, std::intmax_t))
	{
		std::intmax_t lhs = (this->*next_level)();
		while (ok_)
		{
			int op = -1;
			for (int k = 0; ops[k] != nullptr; ++k)
			{
				if (is_punct(ops[k]))
				{
					op = k;
					++i_;
					break;
				}
			}
			if (op < 0)
			{
				break;
			}
			const std::intmax_t rhs = (this->*next_level)();
			if (!ok_)
			{
				return 0;
			}
			lhs = combine(lhs, rhs);
		}
		return lhs;
	}

	std::intmax_t parse_or()
	{
		// || 短路（§6.5.13）：左侧非 0 时结果必为 1，右侧仅作 token 消费并
		// 抑制其求值期错误（如 0/0），与 GCC/Clang 预处理器一致。
		std::intmax_t lhs = parse_and();
		while (ok_ && is_punct("||"))
		{
			++i_;
			const bool dead = (lhs != 0);
			const bool saved = suppress_value_error_;
			if (dead) suppress_value_error_ = true;
			const std::intmax_t rhs = parse_and();
			suppress_value_error_ = saved;
			if (!ok_) return 0;
			lhs = (lhs || rhs) ? 1 : 0;
		}
		return lhs;
	}
	std::intmax_t parse_and()
	{
		// && 短路（§6.5.14）：左侧为 0 时结果必为 0，右侧仅作 token 消费并
		// 抑制其求值期错误。c-testsuite 00145：`0 && (0/0)` 不得报除零。
		std::intmax_t lhs = parse_bitor();
		while (ok_ && is_punct("&&"))
		{
			++i_;
			const bool dead = (lhs == 0);
			const bool saved = suppress_value_error_;
			if (dead) suppress_value_error_ = true;
			const std::intmax_t rhs = parse_bitor();
			suppress_value_error_ = saved;
			if (!ok_) return 0;
			lhs = (lhs && rhs) ? 1 : 0;
		}
		return lhs;
	}
	std::intmax_t parse_bitor()
	{
		static const char *const ops[] = {"|", nullptr};
		return parse_binary(&CondExpr::parse_bitxor, ops,
			[](std::intmax_t a, std::intmax_t b) { return a | b; });
	}
	std::intmax_t parse_bitxor()
	{
		static const char *const ops[] = {"^", nullptr};
		return parse_binary(&CondExpr::parse_bitand, ops,
			[](std::intmax_t a, std::intmax_t b) { return a ^ b; });
	}
	std::intmax_t parse_bitand()
	{
		static const char *const ops[] = {"&", nullptr};
		return parse_binary(&CondExpr::parse_equality, ops,
			[](std::intmax_t a, std::intmax_t b) { return a & b; });
	}
	// 相等档同理需区分 == / !=，泛型 combine 拿不到具体运算符，故单独实现。
	std::intmax_t parse_equality()
	{
		std::intmax_t lhs = parse_relational();
		while (ok_ && (is_punct("==") || is_punct("!=")))
		{
			const bool eq = is_punct("==");
			++i_;
			const std::intmax_t rhs = parse_relational();
			if (!ok_)
			{
				return 0;
			}
			lhs = (eq ? (lhs == rhs) : (lhs != rhs)) ? 1 : 0;
		}
		return lhs;
	}
	// 关系档需区分 4 种子比较，泛型 parse_binary 的 combine 拿不到具体运算符，
	// 故单独实现左结合折叠（与 parse_shift 同构）。
	std::intmax_t parse_relational()
	{
		std::intmax_t lhs = parse_shift();
		while (ok_ && (is_punct("<") || is_punct("<=") || is_punct(">") || is_punct(">=")))
		{
			const std::string op = cur().text;
			++i_;
			const std::intmax_t rhs = parse_shift();
			if (!ok_)
			{
				return 0;
			}
			if (op == "<")
			{
				lhs = (lhs < rhs) ? 1 : 0;
			}
			else if (op == "<=")
			{
				lhs = (lhs <= rhs) ? 1 : 0;
			}
			else if (op == ">")
			{
				lhs = (lhs > rhs) ? 1 : 0;
			}
			else
			{
				lhs = (lhs >= rhs) ? 1 : 0; // ">="
			}
		}
		return lhs;
	}

	std::intmax_t parse_shift()
	{
		std::intmax_t lhs = parse_additive();
		while (ok_ && (is_punct("<<") || is_punct(">>")))
		{
			const bool left = is_punct("<<");
			++i_;
			const std::intmax_t rhs = parse_additive();
			if (!ok_)
			{
				return 0;
			}
			if (rhs < 0 || rhs >= 64)
			{
				// 短路死分支：移位越界属求值期错误，抑制后保留 lhs 继续（值被丢弃）
				if (suppress_value_error_) { continue; }
				return fail("shift count out of range in #if expression");
			}
			if (left)
			{
				lhs = static_cast<std::intmax_t>(static_cast<std::uintmax_t>(lhs)
					<< static_cast<unsigned>(rhs));
			}
			else
			{
				lhs = static_cast<std::intmax_t>(static_cast<std::uintmax_t>(lhs)
					>> static_cast<unsigned>(rhs));
			}
		}
		return lhs;
	}

	std::intmax_t parse_additive()
	{
		std::intmax_t lhs = parse_mul();
		while (ok_ && (is_punct("+") || is_punct("-")))
		{
			const bool plus = is_punct("+");
			++i_;
			const std::intmax_t rhs = parse_mul();
			if (!ok_)
			{
				return 0;
			}
			auto ua = static_cast<std::uintmax_t>(lhs);
			auto ub = static_cast<std::uintmax_t>(rhs);
			lhs = static_cast<std::intmax_t>(plus ? (ua + ub) : (ua - ub));
		}
		return lhs;
	}

	std::intmax_t parse_mul()
	{
		std::intmax_t lhs = parse_unary();
		while (ok_ && (is_punct("*") || is_punct("/") || is_punct("%")))
		{
			const char op = is_punct("*") ? '*' : (is_punct("/") ? '/' : '%');
			++i_;
			const std::intmax_t rhs = parse_unary();
			if (!ok_)
			{
				return 0;
			}
			if (op == '*')
			{
				lhs = static_cast<std::intmax_t>(static_cast<std::uintmax_t>(lhs)
					* static_cast<std::uintmax_t>(rhs));
			}
			else
			{
				if (rhs == 0)
				{
					// 短路死分支：除/模零属求值期错误，抑制后得 0 继续（值被丢弃）
					if (suppress_value_error_) { lhs = 0; continue; }
					return fail("division by zero in #if expression");
				}
				// INTMAX_MIN / -1 与 INTMAX_MIN % -1 在 x86 上会触发 idiv 除法溢出陷阱(SIGFPE)，
				// 且是有符号 UB。按二补数回绕语义特判 rhs == -1：除法得 -lhs（无符号取负回绕），
				// 取模恒为 0（任意整数除以 -1 余数为 0）；与 GCC/Clang 预处理器 -fwrapv 行为一致。
				if (rhs == -1)
				{
					lhs = (op == '/')
						? static_cast<std::intmax_t>(0ULL - static_cast<std::uintmax_t>(lhs))
						: 0;
				}
				else
				{
					lhs = (op == '/') ? (lhs / rhs) : (lhs % rhs);
				}
			}
		}
		return lhs;
	}

	std::intmax_t parse_unary()
	{
		if (is_punct("!"))
		{
			++i_;
			const std::intmax_t v = parse_unary();
			return v ? 0 : 1;
		}
		if (is_punct("~"))
		{
			++i_;
			return ~parse_unary();
		}
		if (is_punct("+"))
		{
			++i_;
			return parse_unary();
		}
		if (is_punct("-"))
		{
			++i_;
			const std::intmax_t v = parse_unary();
			auto u = static_cast<std::uintmax_t>(v);
			return static_cast<std::intmax_t>(0 - u);
		}
		return parse_primary();
	}

	std::intmax_t parse_primary()
	{
		if (at_end())
		{
			return fail("unexpected end of #if expression");
		}
		const Token &t = cur();
		if (t.kind == TokKind::Number)
		{
			std::intmax_t v = 0;
			if (!parse_pp_integer(t.text, v))
			{
				return fail("floating constant may not be used in #if");
			}
			++i_;
			return v;
		}
		if (t.kind == TokKind::Char)
		{
			// 字符常量在 #if/#elif 中合法（§6.10.1）；与 parser/codegen 同源 co_decode_char 解码，
			//   UCN（\u/\U）同样先展开（与 parser 处同口径）。
			++i_;
			return static_cast<std::intmax_t>(co_decode_char(co_expand_ucn(t.text)));
		}
		if (eat_punct("("))
		{
			const std::intmax_t v = parse_conditional();
			if (!ok_)
			{
				return 0;
			}
			if (!eat_punct(")"))
			{
				return fail("expected ')' in #if expression");
			}
			return v;
		}
		if (is_name())
		{
			if (t.text == "defined")
			{
				++i_;
				std::string name;
				if (eat_punct("("))
				{
					if (!is_name())
					{
						return fail("expected identifier after 'defined ('");
					}
					name = cur().text;
					++i_;
					if (!eat_punct(")"))
					{
						return fail("expected ')' after defined name");
					}
				}
				else
				{
					if (!is_name())
					{
						return fail("expected identifier after 'defined'");
					}
					name = cur().text;
					++i_;
				}
				return is_defined_(name) ? 1 : 0;
			}
			if (t.text == "true")
			{
				++i_;
				return 1;
			}
			if (t.text == "false")
			{
				++i_;
				return 0;
			}
			// __has_include(header)
			if (t.text == "__has_include" && has_include_)
			{
				++i_; // consume '__has_include'
				if (!eat_punct("("))
					return fail("expected '(' after __has_include");
				std::string header;
				if (i_ < end_ && cur().kind == TokKind::String)
				{
					// 字符串形式: 去掉外围引号
					std::string raw = cur().text;
					if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
						header = raw.substr(1, raw.size() - 2);
					else
						header = raw;
					++i_;
				}
				else if (i_ < end_ && cur().text == "<")
				{
					// <angle> —— 收集直到 '>'
					++i_; // '<'
					while (i_ < end_ && cur().text != ">")
					{
						header += cur().text;
						++i_;
					}
					if (i_ < end_) ++i_; // '>'
					header = "<" + header + ">";
				}
				else
				{
					return fail("invalid header in __has_include");
				}
				if (!eat_punct(")"))
					return fail("expected ')' after __has_include header");
				return has_include_(header) ? 1 : 0;
			}
			// __has_c_attribute(attr-name)：C23 §6.10.1。参数为属性名序列（如 nodiscard），
			// 拼接所有 token 文本后交由回调判定（作用域名 :: 与未知名均不匹配 → 0）。
			if (t.text == "__has_c_attribute" && has_c_attribute_)
			{
				++i_; // consume '__has_c_attribute'
				if (!eat_punct("("))
					return fail("expected '(' after __has_c_attribute");
				std::string name;
				int depth = 1;
				bool closed = false;
				while (i_ < end_)
				{
					const Token &a = cur();
					const bool punct = (a.kind == TokKind::Punct);
					if (punct && a.text == "(")
					{
						++depth;
						name += a.text;
						++i_;
						continue;
					}
					if (punct && a.text == ")")
					{
						--depth;
						if (depth == 0) { ++i_; closed = true; break; }
						name += a.text;
						++i_;
						continue;
					}
					name += a.text;
					++i_;
				}
				if (!closed)
					return fail("expected ')' after __has_c_attribute attribute name");
				return has_c_attribute_(name) ? 1 : 0;
			}
			// 其余标识符：宏尚未展开，按未定义记 0（P1.4/P1.5 落地展开）
			++i_;
			return 0;
		}
		return fail("invalid token in #if expression");
	}

	const std::vector<Token> &toks_;
	std::size_t i_;
	std::size_t end_;
	std::function<bool(const std::string &)> is_defined_;
	std::function<bool(const std::string &)> has_include_;
	std::function<bool(const std::string &)> has_c_attribute_;
	bool ok_ = true;
	bool suppress_value_error_ = false; // ||/&& 短路死分支中抑制求值期错误（除零/移位越界）
	std::string err_;
};

}  // namespace

// ============================================================
// Preprocessor
// ============================================================
Preprocessor::Preprocessor(const CompileOptions &opts, std::vector<Error> &errors)
	: opts_(opts), errors_(errors)
{
	seed_predefined_macros();
}

// 种入实现预定义宏（平台/架构分派用）。值均为整型常量，以单个 Number token 作对象式宏替换体。
// ground-truth：本机 clang（Windows x64、MS 兼容）-dM 实测 _WIN32=1 _WIN64=1 _M_X64=100
// _MSC_VER=1951 _INTEGRAL_MAX_BITS=64 __x86_64__=1；Linux 移植时按目标在此切换。
void Preprocessor::seed_predefined_macros()
{
	auto def_num = [this](const char *name, const char *val) {
		Macro m;
		Token t;
		t.kind = TokKind::Number;
		t.text = val;
		m.body.push_back(t);
		macros_[name] = std::move(m);
	};
	// —— 目标 / 数据模型分派（单一真值源：opts_.target，见 target.h）——
	// 采 GCC 式数据模型宏（与 __x86_64__ 同类）：如实体制当前目标的数据模型
	//   —— Windows→LLP64(long=4)、Linux→LP64(long=8)。当前仅 x86-64 目标。
	if (opts_.target.is_linux)
	{
		// —— Linux / glibc（ELF, LP64）——
		def_num("__linux__", "1");
		def_num("__gnu_linux__", "1");
		def_num("__ELF__", "1");
		def_num("__x86_64__", "1");
		def_num("__LP64__", "1");   // 32 位目标应改 __ILP32__；当前仅 x86-64
		// 不发：_WIN32/_WIN64/_M_X64/_MSC_VER/_INTEGRAL_MAX_BITS
	}
	else
	{
		// —— Windows（COFF, LLP64）：MSVC 兼容集 + GCC 式数据模型宏 ——
		// _WIN32 在 32/64 位 Windows 上均定义（惯例）
		def_num("_WIN32", "1");
		def_num("_WIN64", "1");
		def_num("_M_X64", "100");            // MSVC x64 约定值
		def_num("__x86_64__", "1");          // 通用 x86-64 约定值
		def_num("_INTEGRAL_MAX_BITS", "64"); // 最大整型位宽
		def_num("_MSC_VER", "1951");         // MSVC 兼容版本（本机 clang 实测）
		def_num("__LLP64__", "1");           // GCC 式数据模型宏（long=4、指针=8）
	}
	// 注：__FILE__/__LINE__/__DATE__/__TIME__/__STDC__/__STDC_VERSION__/__STDC_HOSTED__
	//     由 expand_seq 内置伪宏链特殊处理（并列入 is_defined），此处不重复种入。
}

bool Preprocessor::pull(Token &out, bool &at_eof)
{
	while (!src_stack_.empty())
	{
		Src &s = *src_stack_.back();
		if (s.have_look)
		{
			out = s.look;
			s.have_look = false;
			at_eof = (out.kind == TokKind::EndOfFile);
			return true;
		}
		Error err;
		const LexResult r = s.lexer.next(out, err);
		if (r == LexResult::Error)
		{
			errors_.push_back(std::move(err));
			return false;
		}
		if (r == LexResult::Token)
		{
			at_eof = false;
			return true;
		}
		// 当前源结束：弹栈，继续父源（支持 #include 嵌套）
		src_stack_.pop_back();
	}
	out = Token{};
	at_eof = true;
	return true;
}

void Preprocessor::unread(const Token &t)
{
	if (!src_stack_.empty())
	{
		Src &s = *src_stack_.back();
		s.look = t;
		s.have_look = true;
	}
}

Preprocessor::LineResult Preprocessor::read_logical_line(std::vector<Token> &line)
{
	line.clear();
	Token first;
	bool at_eof = false;
	if (!pull(first, at_eof))
	{
		return LineResult::Error;
	}
	if (at_eof)
	{
		return LineResult::Eof;
	}
	line.push_back(first);
	for (;;)
	{
		Token t;
		bool eof = false;
		if (!pull(t, eof))
		{
			return LineResult::Error;
		}
		if (eof)
		{
			break;
		}
		if (t.bol)
		{
			unread(t);
			break;
		}
		line.push_back(t);
	}
	return LineResult::Ok;
}

bool Preprocessor::is_active() const
{
	return cond_stack_.empty() ? true : cond_stack_.back().branch_active;
}

// —— 函数式宏展开的拼接/字符串化辅助（§6.10.3.4）——
namespace
{
// 对拼接后的拼写重新切分为 pp-token（## 粘贴结果需重扫为单一 token）。
void relex_tokens(const std::string &src, std::vector<Token> &out)
{
	Lexer lx(std::string(), src);
	std::vector<Token> tt;
	std::vector<Error> ee;
	if (lx.tokenize(tt, ee))
	{
		for (Token &t : tt)
		{
			if (t.kind != TokKind::EndOfFile)
			{
				out.push_back(t);
			}
		}
		return;
	}
	// 词法失败：退化为单个标识符 token，保留原拼写
	Token fb;
	fb.kind = TokKind::Identifier;
	fb.text = src;
	out.push_back(fb);
}

// 邻接粘贴：L 的末 token 与 R 的首 token 合并；任一侧为空则直接取另一侧。
void paste_tokens(const std::vector<Token> &L, const std::vector<Token> &R, std::vector<Token> &out)
{
	if (L.empty())
	{
		out.insert(out.end(), R.begin(), R.end());
		return;
	}
	if (R.empty())
	{
		out.insert(out.end(), L.begin(), L.end());
		return;
	}
	const std::string combined = L.back().text + R.front().text;
	std::vector<Token> merged;
	relex_tokens(combined, merged);
	out.insert(out.end(), L.begin(), L.end() - 1);
	out.insert(out.end(), merged.begin(), merged.end());
	out.insert(out.end(), R.begin() + 1, R.end());
}

// 字符串化：实参 token 拼写按单空格连接，转义 \" 与 \\，外包双引号。
Token stringize_token(const std::vector<Token> &arg, const SourceLoc &at)
{
	std::string raw;
	for (std::size_t i = 0; i < arg.size(); ++i)
	{
		if (i != 0)
		{
			raw += ' ';
		}
		raw += arg[i].text;
	}
	std::string lit = "\"";
	for (char ch : raw)
	{
		if (ch == '"' || ch == '\\')
		{
			lit += '\\';
			lit += ch;
		}
		else
		{
			lit += ch;
		}
	}
	lit += "\"";
	Token t;
	t.kind = TokKind::String;
	t.text = lit;
	t.loc = at;
	return t;
}
} // namespace

// —— 宏展开（§6.10.3 / §6.10.8）——
// 逐 token 扫描：遇宏名（非蓝铅笔隐藏）则展开。对象式宏拉入替换体就地重扫；
// 函数式宏仅当紧随 '(' 才构成调用，收集实参、替换（参数原样代入、# 字符串化、
// ## 粘贴）后整段就地重扫。蓝铅笔集 blue 逐步加入本次宏名以防自递归。in_if 为真时
// 保护 defined 操作数不被展开。
void Preprocessor::expand_seq(const std::vector<Token> &in, std::vector<Token> &out,
	const std::unordered_set<std::string> &blue, bool in_if) const
{
	// P174：逐 token 携带蓝铅笔集 + 展开产物就地 splice 回当前序列重扫。
	// 不再对宏产物递归调用 expand_seq 写独立 out（那会让 `AB` 与其后原文 `(x)` 分处两序列、
	// 边界不成宏调用，00201 失败）。隐藏集规则：宏体字面 token 传递累加，实参/粘贴产物重置 ∅。
	std::vector<PEntry> buf;
	buf.reserve(in.size());
	for (const Token &t : in)
	{
		buf.push_back(PEntry{t, blue});
	}
	const std::size_t budget = in.size() * 512 + 1000000; // 迭代护栏：防规则失配致无穷展开
	std::size_t steps = 0;
	std::size_t i = 0;
	while (i < buf.size())
	{
		if (++steps > budget)
		{
			const Token &ct = buf[i].tok;
			errors_.push_back(Error{ct.loc.file, Pos{ct.loc.line, ct.loc.column},
				"macro expansion exceeded recursion budget (possible unbounded expansion)"});
			break;
		}
		const Token &t = buf[i].tok;
		const std::unordered_set<std::string> hide = buf[i].hide;
		// defined（#if 上下文）：连同操作数原样保留、跳过之（操作数不展开）
		if (in_if && t.kind == TokKind::Identifier && t.text == "defined")
		{
			std::size_t j = i + 1;
			const bool paren = (j < buf.size() && buf[j].tok.kind == TokKind::Punct && buf[j].tok.text == "(");
			if (paren)
			{
				++j;
			}
			if (j < buf.size() && (buf[j].tok.kind == TokKind::Identifier || buf[j].tok.kind == TokKind::Keyword))
			{
				++j; // 操作数宏名不展开
			}
			if (paren && j < buf.size() && buf[j].tok.kind == TokKind::Punct && buf[j].tok.text == ")")
			{
				++j;
			}
			i = j; // defined 与操作数保持在 buf 中原样输出，不再处理
			continue;
		}
		if (t.kind == TokKind::Identifier)
		{
			// 关键字伪宏（§6.10.8）：就地替换为终态 token（重定向到调用点位置），++i 不再重扫
			bool pseudo = true;
			if (t.text == "__FILE__")
			{
				Token r;
				r.kind = TokKind::String;
				// 缺陷 #27：__FILE__ 必须是合法的字符串字面量——路径中的 \ 与 " 须转义，
				// 否则 Windows 路径 tests\x.co 里的 \x 会被下游字符串解码当作转义序列吞掉
				std::string body;
				for (char ch : t.loc.file)
				{
					if (ch == '\\' || ch == '"')
					{
						body += '\\';
					}
					body += ch;
				}
				r.text = "\"" + body + "\"";
				r.loc = t.loc;
				buf[i].tok = r;
			}
			else if (t.text == "__LINE__")
			{
				Token r;
				r.kind = TokKind::Number;
				r.text = std::to_string(t.loc.line);
				r.loc = t.loc;
				buf[i].tok = r;
			}
			else if (t.text == "__DATE__" || t.text == "__TIME__")
			{
				Token r;
				r.kind = TokKind::String;
				r.text = t.text == "__DATE__" ? "\"Jan  1 2026\"" : "\"00:00:00\"";
				r.loc = t.loc;
				buf[i].tok = r;
			}
			else if (t.text == "__STDC__" || t.text == "__STDC_HOSTED__")
			{
				Token r;
				r.kind = TokKind::Number;
				r.text = "1";
				r.loc = t.loc;
				buf[i].tok = r;
			}
			else if (t.text == "__STDC_VERSION__")
			{
				Token r;
				r.kind = TokKind::Number;
				r.text = "202311L";
				r.loc = t.loc;
				buf[i].tok = r;
			}
			else
			{
				pseudo = false;
			}
			if (pseudo)
			{
				++i;
				continue;
			}
			auto it = macros_.find(t.text);
			if (it != macros_.end() && hide.find(t.text) == hide.end())
			{
				const Macro &m = it->second;
				if (!m.is_function_like)
				{
					// 对象式：宏体字面 token hide = 本 token hide ∪ {name}；就地 splice、驻 i 重扫
					std::unordered_set<std::string> newHide = hide;
					newHide.insert(t.text);
					std::vector<PEntry> body;
					body.reserve(m.body.size());
					for (const Token &bt : m.body)
					{
						Token c = bt;
						c.loc = t.loc; // 缺陷 #26：体内 __FILE__/__LINE__ 取调用点
						body.push_back(PEntry{c, newHide});
					}
					buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(i));
					buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(i), body.begin(), body.end());
					continue; // i 不变，重扫 splice 内容
				}
				// 函数式：仅当紧邻 '(' 才构成调用，否则宏名原样保留
				const bool invoked =
					(i + 1 < buf.size() && buf[i + 1].tok.kind == TokKind::Punct && buf[i + 1].tok.text == "(");
				if (invoked)
				{
					std::vector<std::vector<Token>> args;
					std::size_t end = 0;
					if (collect_args(buf, i + 1, args, end))
					{
						const std::size_t np = m.params.size();
						const bool arity_ok = m.is_variadic ? (args.size() >= np) : (args.size() == np);
						if (!arity_ok)
						{
							errors_.push_back(Error{t.loc.file, Pos{t.loc.line, t.loc.column},
								"macro '" + t.text + "' invoked with wrong number of arguments"});
							++i; // 原样保留宏名，'(' 及内容交由后续正常处理
							continue;
						}
						std::unordered_set<std::string> bodyHide = hide;
						bodyHide.insert(t.text);
						std::vector<PEntry> sub;
						substitute(m, args, sub, t.loc, in_if, bodyHide);
						buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(i),
							buf.begin() + static_cast<std::ptrdiff_t>(end));
						buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(i), sub.begin(), sub.end());
						continue; // i 不变，重扫替换结果（可跨越其后原文 '(' 构成新调用）
					}
					// 实参括号未闭合：宏名原样保留，'(' 及内容交由后续正常处理
				}
			}
		}
		++i;
	}
	// 追加产出（out 为跨逻辑行累积的输出流，务必 append、不得 clear）
	out.reserve(out.size() + buf.size());
	for (PEntry &pe : buf)
	{
		out.push_back(pe.tok);
	}
}

// 从 buf[open]=='(' 起按顶层逗号切分实参；括号嵌套计数保护内层逗号。空实参表 `()` → 零实参。
// 作用于展开工作序列 buf（P174）：实参可跨越此前已 splice 拼回的宏体 token（与 clang 一致）。
bool Preprocessor::collect_args(const std::vector<PEntry> &buf, std::size_t open,
	std::vector<std::vector<Token>> &args, std::size_t &end) const
{
	const std::size_t n = buf.size();
	std::size_t i = open + 1;
	if (i < n && buf[i].tok.kind == TokKind::Punct && buf[i].tok.text == ")")
	{
		args.clear();
		end = i + 1;
		return true; // 空实参表
	}
	std::vector<Token> cur;
	int depth = 0;
	for (; i < n; ++i)
	{
		const Token &t = buf[i].tok;
		const bool punct = (t.kind == TokKind::Punct);
		if (punct && t.text == "(")
		{
			++depth;
			cur.push_back(t);
		}
		else if (punct && t.text == ")")
		{
			if (depth == 0)
			{
				args.push_back(cur);
				end = i + 1;
				return true;
			}
			--depth;
			cur.push_back(t);
		}
		else if (depth == 0 && punct && t.text == ",")
		{
			args.push_back(cur);
			cur.clear();
		}
		else
		{
			cur.push_back(t);
		}
	}
	end = n;
	return false; // 括号未闭合
}

namespace {
// C23 §6.10.3.4：处理函数式宏体中的 __VA_OPT__(content)。
// 一次宏调用内所有 __VA_OPT__ 组共享同一 __VA_ARGS__ 的空/非空判定：
//   va 非空 → 整组展开为 content（content 内嵌套 __VA_OPT__ 递归同样展开）；
//   va 为空 → 整组（连同内层）删除。
// 产出的 resolved 体再交给常规流程做 # 字符串化 / ## 粘贴 / 形参代入。
void resolve_va_opt(const std::vector<Token> &body, std::vector<Token> &out,
	const std::vector<Token> &va)
{
	const std::size_t n = body.size();
	for (std::size_t i = 0; i < n; ++i)
	{
		const Token &t = body[i];
		const bool is_kw = (t.kind == TokKind::Identifier || t.kind == TokKind::Keyword);
		if (is_kw && t.text == "__VA_OPT__" && i + 1 < n &&
			body[i + 1].kind == TokKind::Punct && body[i + 1].text == "(")
		{
			// 定位与 '__VA_OPT__(' 匹配的右括号（depth 从 1 起）
			std::size_t depth = 0;
			std::size_t j = i + 1;
			for (; j < n; ++j)
			{
				if (body[j].kind != TokKind::Punct) continue;
				if (body[j].text == "(") ++depth;
				else if (body[j].text == ")")
				{
					--depth;
					if (depth == 0) break;
				}
			}
			if (!va.empty())
			{
				// content = body[i+2 .. j-1]，递归处理内层 __VA_OPT__
				std::vector<Token> content;
				if (j < n && j > i + 2)
					content.assign(body.begin() + static_cast<std::ptrdiff_t>(i + 2),
						body.begin() + static_cast<std::ptrdiff_t>(j));
				std::vector<Token> sub;
				resolve_va_opt(content, sub, va);
				out.insert(out.end(), sub.begin(), sub.end());
			}
			// va 为空：整组丢弃（含内层）
			i = j; // 跳到右括号索引；外层 ++i 后至 j+1
			continue;
		}
		out.push_back(t);
	}
}
} // namespace

// 函数式宏体替换：产出逐 token 带隐藏集的序列（P174）——普通（非 #/## 操作数）参数的实参先独立
// 完整宏展开再代入（hide=∅，C23 §6.10.3.1），# 字符串化用原样实参（终态 String），
// ## 粘贴以原样实参左结合合并（连续 ## 归为一个粘贴 run，产物 hide=∅ 重置），宏体字面 token 取 bodyHide。
void Preprocessor::substitute(const Macro &m, const std::vector<std::vector<Token>> &args,
	std::vector<PEntry> &out, const SourceLoc &at, bool in_if,
	const std::unordered_set<std::string> &bodyHide) const
{
	static const std::vector<Token> k_empty;
	static const std::unordered_set<std::string> k_nohide;
	// 以指定隐藏集把一串 token 追加到 out（实参代入/粘贴产物用 k_nohide，宏体字面用 bodyHide）。
	auto push_with = [&](const std::vector<Token> &v, const std::unordered_set<std::string> &h) {
		for (const Token &x : v)
		{
			out.push_back(PEntry{x, h});
		}
	};

	// 变参：第 params.size() 个之后的实参用逗号拼成 __VA_ARGS__
	std::vector<Token> va;
	if (m.is_variadic)
	{
		for (std::size_t a = m.params.size(); a < args.size(); ++a)
		{
			if (a > m.params.size())
			{
				Token comma;
				comma.kind = TokKind::Punct;
				comma.text = ",";
				comma.loc = at;
				va.push_back(comma);
			}
			va.insert(va.end(), args[a].begin(), args[a].end());
		}
	}

	auto arg_for = [&](const std::string &name, const std::vector<Token> *&res) -> bool {
		for (std::size_t p = 0; p < m.params.size(); ++p)
		{
			if (m.params[p] == name)
			{
				res = (p < args.size()) ? &args[p] : &k_empty;
				return true;
			}
		}
		if (m.is_variadic && name == "__VA_ARGS__")
		{
			res = &va;
			return true;
		}
		return false;
	};
	auto is_param_name = [&](const Token &x) {
		return x.kind == TokKind::Identifier || x.kind == TokKind::Keyword;
	};
	// 操作数展开：形参→原样实参 token；否则单 token 自身
	auto operand = [&](const Token &x) -> std::vector<Token> {
		const std::vector<Token> *a = nullptr;
		if (is_param_name(x) && arg_for(x.text, a))
		{
			return *a;
		}
		return std::vector<Token>{x};
	};

	// C23 §6.10.3.1：普通（非 #/## 操作数）参数在代入前，实参先作独立序列完整宏展开
	//（空蓝铅笔集——与当前宏的展开链无关），展开结果用于普通代入；#/## 仍取原样实参。
	std::vector<std::vector<Token>> ex_args(args.size());
	for (std::size_t p = 0; p < args.size(); ++p)
	{
		expand_seq(args[p], ex_args[p], std::unordered_set<std::string>{}, in_if);
	}
	std::vector<Token> ex_va;
	if (m.is_variadic)
	{
		expand_seq(va, ex_va, std::unordered_set<std::string>{}, in_if);
	}
	auto expanded_for = [&](const std::string &name, const std::vector<Token> *&res) -> bool {
		for (std::size_t p = 0; p < m.params.size(); ++p)
		{
			if (m.params[p] == name)
			{
				res = (p < ex_args.size()) ? &ex_args[p] : &k_empty;
				return true;
			}
		}
		if (m.is_variadic && name == "__VA_ARGS__")
		{
			res = &ex_va;
			return true;
		}
		return false;
	};

	// C23 __VA_OPT__：变参宏先据 va 空/非空展开/删除各组，再进入常规替换流
	std::vector<Token> resolved;
	if (m.is_variadic)
		resolve_va_opt(m.body, resolved, va);
	else
		resolved = m.body;

	const std::vector<Token> &B = resolved;
	std::size_t k = 0;
	while (k < B.size())
	{
		const Token &bt = B[k];
		// # 字符串化：'#' 紧邻形参
		if (bt.kind == TokKind::Punct && bt.text == "#" && k + 1 < B.size())
		{
			const std::vector<Token> *a = nullptr;
			if (is_param_name(B[k + 1]) && arg_for(B[k + 1].text, a))
			{
				out.push_back(PEntry{stringize_token(*a, at), k_nohide});
				k += 2;
				continue;
			}
		}
		// 粘贴 run：bt ## x [## y ...]
		if (k + 1 < B.size() && B[k + 1].kind == TokKind::Punct && B[k + 1].text == "##")
		{
			std::vector<Token> acc = operand(bt);
			std::size_t j = k + 2;
			for (;;)
			{
				if (j >= B.size())
				{
					break;
				}
				std::vector<Token> pasted;
				paste_tokens(acc, operand(B[j]), pasted);
				acc.swap(pasted);
				if (j + 1 < B.size() && B[j + 1].kind == TokKind::Punct && B[j + 1].text == "##")
				{
					j += 2;
					continue;
				}
				j += 1;
				break;
			}
			push_with(acc, k_nohide); // ## 粘贴产物：hide 重置为 ∅（00201 末尾 CAT 得以重展开的关键）
			k = j;
			continue;
		}
		// 普通形参代入：用已展开实参（§6.10.3.1；非 #/## 操作数）
		{
			const std::vector<Token> *e = nullptr;
			if (is_param_name(bt) && expanded_for(bt.text, e))
			{
				push_with(*e, k_nohide); // 普通形参代入预展开实参：hide=∅
				++k;
				continue;
			}
		}
		// 缺陷 #26：替换体字面 token（如 __FILE__/__LINE__）归位到调用点（实参代入的 token 保留其自身位置）
		Token rb = bt;
		rb.loc = at;
		out.push_back(PEntry{rb, bodyHide});
		++k;
	}
}

void Preprocessor::expand_line(const std::vector<Token> &line, std::vector<Token> &out) const
{
	expand_seq(line, out, std::unordered_set<std::string>{}, false);
}

bool Preprocessor::eval_expr(const std::vector<Token> &line, std::size_t from,
	std::intmax_t &result)
{
	// §6.10.1：求值前先展开表达式中的宏（defined 操作数除外），剩余未展开标识符按 0
	const std::vector<Token> expr(line.begin() + static_cast<std::ptrdiff_t>(from), line.end());
	std::vector<Token> expanded;
	expand_seq(expr, expanded, std::unordered_set<std::string>{}, true);

	const std::string file = line.empty() ? opts_.source_path : line[0].loc.file;
	const Pos at = (from < line.size()) ? Pos{line[from].loc.line, line[from].loc.column} : Pos{};

	auto is_defined = [this](const std::string &name) {
		if (macros_.count(name) > 0) return true;
		// 内置伪宏始终视为已定义
		return name == "__FILE__" || name == "__LINE__" || name == "__DATE__" ||
			name == "__TIME__" || name == "__STDC__" || name == "__STDC_VERSION__" ||
			name == "__STDC_HOSTED__";
	};
	auto has_include = [this](const std::string &hdr) -> bool {
		// 解析头文件名并搜索 include_dirs
		std::string name = hdr;
		bool angle = false;
		if (name.size() >= 2 && name[0] == '<' && name.back() == '>')
		{
			name = name.substr(1, name.size() - 2);
			angle = true;
		}
		// 搜索当前文件目录 + include_dirs
		std::vector<std::string> dirs;
		if (!angle && !opts_.source_path.empty())
			dirs.push_back(path_dirname(opts_.source_path));
		for (const auto &d : opts_.include_dirs)
			dirs.push_back(d);
		for (const auto &d : dirs)
		{
			std::string full = path_join(d, name);
			std::ifstream test(full, std::ios::binary);
			if (test) return true;
		}
		return false;
	};
	auto has_c_attribute = [](const std::string &name) -> bool {
		// C23 §6.7.11 标准属性 + Co 层新定义属性 lib（P214）；作用域名(::)与未知名一律 false
		return name == "deprecated" || name == "maybe_unused" || name == "nodiscard" ||
		       name == "lib";
	};
	CondExpr ce(expanded, 0, expanded.size(), is_defined, has_include, has_c_attribute);
	const std::intmax_t v = ce.eval();
	if (!ce.ok())
	{
		errors_.push_back(Error{file, at, ce.error()});
		return false;
	}
	if (!ce.consumed())
	{
		const Token &trailing = expanded[ce.index()];
		errors_.push_back(Error{trailing.loc.file, Pos{trailing.loc.line, trailing.loc.column},
			"trailing tokens in #if expression"});
		return false;
	}
	result = v;
	return true;
}

// —— 指令处理 ——
bool Preprocessor::handle_if(const std::vector<Token> &line)
{
	const bool parent = is_active();
	bool active = false;
	if (parent)
	{
		if (line.size() < 3)
		{
			errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
				"#if requires an expression"});
			return false;
		}
		std::intmax_t v = 0;
		if (!eval_expr(line, 2, v))
		{
			return false;
		}
		active = (v != 0);
	}
	cond_stack_.push_back(CondState{parent, parent && active, parent ? active : true});
	return true;
}

bool Preprocessor::handle_ifdef(const std::vector<Token> &line, bool negate)
{
	const bool parent = is_active();
	bool active = false;
	if (parent)
	{
		if (line.size() < 3 || line[2].kind != TokKind::Identifier)
		{
			errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
				negate ? "#ifndef requires an identifier" : "#ifdef requires an identifier"});
			return false;
		}
		const bool defined = macros_.count(line[2].text) > 0;
		active = negate ? !defined : defined;
	}
	cond_stack_.push_back(CondState{parent, parent && active, parent ? active : true});
	return true;
}

bool Preprocessor::handle_elif(const std::vector<Token> &line)
{
	if (cond_stack_.empty())
	{
		errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
			"#elif without #if"});
		return false;
	}
	CondState &cs = cond_stack_.back();
	if (!cs.parent_active)
	{
		return true; // 外层已假，本组整体保持不活跃
	}
	if (cs.any_taken)
	{
		cs.branch_active = false;
		return true;
	}
	if (line.size() < 3)
	{
		errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
			"#elif requires an expression"});
		return false;
	}
	std::intmax_t v = 0;
	if (!eval_expr(line, 2, v))
	{
		return false;
	}
	cs.branch_active = (v != 0);
	cs.any_taken = (v != 0);
	return true;
}

// C23 #elifdef / #elifndef（§6.10.1）：等价于“条件是标识符是否已定义”的 #elif。
// 与其他 elif/ifdef 一样无条件分派（内部自行处理父级不活跃 / any_taken）。
bool Preprocessor::handle_elifdef(const std::vector<Token> &line, bool negate)
{
	if (cond_stack_.empty())
	{
		errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
			negate ? "#elifndef without #if" : "#elifdef without #if"});
		return false;
	}
	CondState &cs = cond_stack_.back();
	if (!cs.parent_active)
	{
		return true; // 外层已假，本组整体保持不活跃
	}
	if (cs.any_taken)
	{
		cs.branch_active = false;
		return true;
	}
	if (line.size() < 3 || line[2].kind != TokKind::Identifier)
	{
		errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
			negate ? "#elifndef requires an identifier" : "#elifdef requires an identifier"});
		return false;
	}
	const bool defined = macros_.count(line[2].text) > 0;
	const bool active = negate ? !defined : defined;
	cs.branch_active = active;
	cs.any_taken = active;
	return true;
}

bool Preprocessor::handle_else(const std::vector<Token> &line)
{
	if (cond_stack_.empty())
	{
		errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
			"#else without #if"});
		return false;
	}
	CondState &cs = cond_stack_.back();
	if (!cs.parent_active)
	{
		cs.branch_active = false;
		return true;
	}
	if (cs.any_taken)
	{
		cs.branch_active = false;
		return true;
	}
	cs.branch_active = true;
	cs.any_taken = true;
	return true;
}

bool Preprocessor::handle_endif(const std::vector<Token> &line)
{
	if (cond_stack_.empty())
	{
		errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
			"#endif without #if"});
		return false;
	}
	cond_stack_.pop_back();
	return true;
}

bool Preprocessor::handle_define(const std::vector<Token> &line)
{
	if (line.size() < 3 || line[2].kind != TokKind::Identifier)
	{
		errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
			"#define requires a macro name"});
		return false;
	}
	// 函数式判定：宏名紧邻 '('（无空白）；否则为对象式。
	const std::string &name = line[2].text;
	Macro m;
	const bool fn = (line.size() > 3 && line[3].kind == TokKind::Punct &&
		line[3].text == "(" && !line[3].had_space);
	if (!fn)
	{
		m.body.assign(line.begin() + 3, line.end());
		macros_[name] = std::move(m);
		return true;
	}
	m.is_function_like = true;
	// 解析形参表：line[3]=='('，逐项 形参名 / ',' / '...'，至 ')' 结束。
	std::size_t k = 4;
	const auto punct_at = [&](std::size_t idx, const char *p) {
		return idx < line.size() && line[idx].kind == TokKind::Punct && line[idx].text == p;
	};
	if (punct_at(k, ")"))
	{
		// F()：无固定形参
	}
	else
	{
		for (;;)
		{
			if (punct_at(k, "..."))
			{
				m.is_variadic = true;
				++k;
				if (!punct_at(k, ")"))
				{
					errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
						"'...' must be last in macro parameter list"});
					return false;
				}
				break;
			}
			if (k >= line.size() || (line[k].kind != TokKind::Identifier && line[k].kind != TokKind::Keyword))
			{
				errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
					"macro parameter must be an identifier"});
				return false;
			}
			m.params.push_back(line[k].text);
			++k;
			if (punct_at(k, ","))
			{
				++k;
				continue;
			}
			if (punct_at(k, ")"))
			{
				break;
			}
			errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
				"expected ',' or ')' in macro parameter list"});
			return false;
		}
	}
	// k 落在 ')'；替换体为其后全部 token
	m.body.assign(line.begin() + static_cast<std::ptrdiff_t>(k + 1), line.end());
	macros_[name] = std::move(m);
	return true;
}

bool Preprocessor::handle_undef(const std::vector<Token> &line)
{
	if (line.size() < 3 || line[2].kind != TokKind::Identifier)
	{
		errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
			"#undef requires a macro name"});
		return false;
	}
	macros_.erase(line[2].text);
	return true;
}

bool Preprocessor::handle_include(const std::vector<Token> &line)
{
	const SourceLoc &at = line[0].loc;
	if (line.size() < 3)
	{
		errors_.push_back(Error{at.file, Pos{at.line, at.column}, "#include requires a header name"});
		return false;
	}
	// 头名与搜索形式：引号 "..."（含当前文件目录优先）或尖括号 <...>（仅 -I 目录）
	const Token &h = line[2];
	std::string name;
	bool quote_form = false;
	if (h.kind == TokKind::String)
	{
		if (h.text.size() >= 2)
		{
			name = h.text.substr(1, h.text.size() - 2); // 去首尾双引号
		}
		quote_form = true;
	}
	else if (h.kind == TokKind::Punct && h.text == "<")
	{
		bool closed = false;
		for (std::size_t i = 3; i < line.size(); ++i)
		{
			if (line[i].kind == TokKind::Punct && line[i].text == ">")
			{
				closed = true;
				break;
			}
			name += line[i].text;
		}
		if (!closed)
		{
			errors_.push_back(Error{at.file, Pos{at.line, at.column}, "missing closing '>' in #include"});
			return false;
		}
	}
	else
	{
		// P1.6 暂不支持经宏展开得到的头名
		errors_.push_back(Error{at.file, Pos{at.line, at.column}, "#include expects \"FILE\" or <FILE>"});
		return false;
	}
	if (name.empty())
	{
		errors_.push_back(Error{at.file, Pos{at.line, at.column}, "empty #include file name"});
		return false;
	}
	// 嵌套深度护栏：防无守卫的循环 include 递归
	if (src_stack_.size() >= 256)
	{
		errors_.push_back(Error{at.file, Pos{at.line, at.column}, "#include nested too deeply"});
		return false;
	}
	// 搜索序列：user 形式先查当前文件目录；再查 -I 目录；user 额外回退裸名（相对 cwd）
	std::vector<std::string> candidates;
	if (quote_form)
	{
		candidates.push_back(path_join(path_dirname(at.file), name));
	}
	for (const std::string &dir : opts_.include_dirs)
	{
		candidates.push_back(path_join(dir, name));
	}
	if (quote_form)
	{
		candidates.push_back(name);
	}
	for (const std::string &cand : candidates)
	{
		std::string text;
		if (read_whole_file(cand, text))
		{
			// #pragma once：已标记过的文件不再展开
			if (pragma_once_.count(normalize_path(cand)))
			{
				return true;
			}
			src_stack_.push_back(std::make_unique<Src>(cand, std::move(text)));
			if (opts_.rc_dialect) // RC 方言：新压入源按 rc.exe 规则切串
				src_stack_.back()->lexer.set_rc_string_mode(true);
			return true; // 新源已压栈，由主循环从栈顶继续拉取
		}
	}
	errors_.push_back(Error{at.file, Pos{at.line, at.column}, "#include file not found: " + name});
	return false;
}

// —— #embed（C23 §6.10.4）——
// 语法：#embed "file" 或 #embed <file>  后接零或多个 (embed-param)。
// 展开：每个字节产一个 0xNN 十六进制 Number token，字节间以逗号分隔（无尾逗号）；
// 支持 limit(pp-number)/prefix·suffix·tail(字符串字面量)/if-empty({ 替换序列 })。
// map-c-char / map-to-* 等字符映射参数暂不实现（报错）。
bool Preprocessor::handle_embed(const std::vector<Token> &line, std::vector<Token> &out)
{
	const SourceLoc &at = line[0].loc;
	auto emit_err = [&](const std::string &msg) {
		errors_.push_back(Error{at.file, Pos{at.line, at.column}, msg});
	};
	auto mk_comma = [&]() {
		Token t;
		t.kind = TokKind::Punct;
		t.text = ",";
		t.loc = at;
		return t;
	};
	auto mk_number = [&](const std::string &sp) {
		Token t;
		t.kind = TokKind::Number;
		t.text = sp;
		t.loc = at;
		return t;
	};

	if (line.size() < 3)
	{
		emit_err("#embed requires a header name");
		return false;
	}
	// 1) 解析 header-name（引号形式或尖括号形式），idx 停在 header-name 之后
	const Token &h = line[2];
	std::string name;
	bool quote_form = false;
	std::size_t idx = 0;
	if (h.kind == TokKind::String)
	{
		name = string_literal_inner(h.text);
		quote_form = true;
		idx = 3;
	}
	else if (h.kind == TokKind::Punct && h.text == "<")
	{
		bool closed = false;
		std::size_t i = 3;
		for (; i < line.size(); ++i)
		{
			if (line[i].kind == TokKind::Punct && line[i].text == ">")
			{
				++i;
				closed = true;
				break;
			}
			name += line[i].text;
		}
		if (!closed)
		{
			emit_err("missing closing '>' in #embed");
			return false;
		}
		idx = i;
	}
	else
	{
		emit_err("#embed expects \"FILE\" or <FILE>");
		return false;
	}
	if (name.empty())
	{
		emit_err("empty #embed file name");
		return false;
	}

	// 2) 解析 embed-param 序列
	bool has_limit = false;
	std::size_t limit = 0;
	std::vector<Token> prefix_tok, suffix_tok, tail_tok, ifempty_tok;
	while (idx < line.size())
	{
		if (!(line[idx].kind == TokKind::Punct && line[idx].text == "("))
		{
			emit_err("trailing tokens after #embed header");
			return false;
		}
		// 定位匹配的 ')'（括号计数），组内 token 为 [start, close)
		int depth = 0;
		std::size_t closepos = line.size();
		for (std::size_t j = idx; j < line.size(); ++j)
		{
			if (line[j].kind == TokKind::Punct && line[j].text == "(")
			{
				++depth;
			}
			else if (line[j].kind == TokKind::Punct && line[j].text == ")")
			{
				--depth;
				if (depth == 0)
				{
					closepos = j;
					break;
				}
			}
		}
		if (closepos >= line.size())
		{
			emit_err("unterminated '(' in #embed parameter");
			return false;
		}
		std::vector<Token> grp(line.begin() + static_cast<std::ptrdiff_t>(idx + 1),
			line.begin() + static_cast<std::ptrdiff_t>(closepos));
		idx = closepos + 1;
		if (grp.empty())
		{
			emit_err("empty #embed parameter");
			return false;
		}
		// 重组参数名：连续 Identifier/Keyword 与 '-' 拼为一名（如 if-empty、map-c-char）
		std::string pname;
		std::size_t p = 0;
		for (; p < grp.size(); ++p)
		{
			const Token &g = grp[p];
			if (g.kind == TokKind::Identifier || g.kind == TokKind::Keyword)
			{
				pname += g.text;
			}
			else if (g.kind == TokKind::Punct && g.text == "-")
			{
				pname += "-";
			}
			else
			{
				break;
			}
		}
		std::vector<Token> rest(grp.begin() + static_cast<std::ptrdiff_t>(p), grp.end());
		if (pname == "limit")
		{
			if (rest.size() != 1 || rest[0].kind != TokKind::Number)
			{
				emit_err("#embed 'limit' requires a single pp-number");
				return false;
			}
			std::intmax_t v = 0;
			if (!parse_pp_integer(rest[0].text, v) || v < 0)
			{
				emit_err("#embed 'limit' requires a non-negative integer");
				return false;
			}
			limit = static_cast<std::size_t>(v);
			has_limit = true;
		}
		else if (pname == "prefix" || pname == "suffix" || pname == "tail")
		{
			if (rest.size() != 1 || rest[0].kind != TokKind::String)
			{
				emit_err("#embed '" + pname + "' requires a single string literal");
				return false;
			}
			std::vector<Token> re;
			relex_tokens(string_literal_inner(rest[0].text), re);
			if (pname == "prefix")
			{
				prefix_tok = std::move(re);
			}
			else if (pname == "suffix")
			{
				suffix_tok = std::move(re);
			}
			else
			{
				tail_tok = std::move(re);
			}
		}
		else if (pname == "if-empty")
		{
			// 一个或多个 { pp-token-sequence }；多个时以逗号连接。取花括号内 token 原样。
			std::size_t q = 0;
			if (rest.empty())
			{
				emit_err("#embed 'if-empty' requires a replacement list");
				return false;
			}
			while (q < rest.size())
			{
				if (!(rest[q].kind == TokKind::Punct && rest[q].text == "{"))
				{
					emit_err("#embed 'if-empty' expects '{ ... }'");
					return false;
				}
				std::size_t r = q + 1;
				while (r < rest.size() && !(rest[r].kind == TokKind::Punct && rest[r].text == "}"))
				{
					++r;
				}
				if (r >= rest.size())
				{
					emit_err("missing '}' in #embed 'if-empty'");
					return false;
				}
				if (!ifempty_tok.empty())
				{
					ifempty_tok.push_back(mk_comma());
				}
				for (std::size_t k = q + 1; k < r; ++k)
				{
					ifempty_tok.push_back(rest[k]);
				}
				q = r + 1;
			}
		}
		else
		{
			emit_err("#embed parameter '" + pname + "' is not implemented yet");
			return false;
		}
	}

	// 3) 定位并读入文件（原始字节，不剥 BOM）
	std::string path;
	std::string data;
	bool found = resolve_header_path(name, quote_form, at.file, opts_, path);
	if (found && !read_whole_file_raw(path, data))
	{
		found = false;
	}
	if (!found && ifempty_tok.empty())
	{
		emit_err("#embed file not found: " + name);
		return false;
	}
	std::size_t nbytes = found ? data.size() : 0;
	if (has_limit && limit < nbytes)
	{
		nbytes = limit;
	}

	// 4) 发射 0xNN token 序列
	if (nbytes == 0)
	{
		out.insert(out.end(), ifempty_tok.begin(), ifempty_tok.end());
		return true;
	}
	static const char kHex[] = "0123456789abcdef";
	for (std::size_t b = 0; b < nbytes; ++b)
	{
		if (b > 0)
		{
			out.push_back(mk_comma());
		}
		out.insert(out.end(), prefix_tok.begin(), prefix_tok.end());
		const unsigned char v = static_cast<unsigned char>(data[b]);
		std::string sp = "0x";
		sp += kHex[(v >> 4) & 0xF];
		sp += kHex[v & 0xF];
		out.push_back(mk_number(sp));
		out.insert(out.end(), suffix_tok.begin(), suffix_tok.end());
	}
	if (!tail_tok.empty())
	{
		out.push_back(mk_comma());
		out.insert(out.end(), tail_tok.begin(), tail_tok.end());
	}
	return true;
}

bool Preprocessor::handle_error(const std::vector<Token> &line)
{
	const SourceLoc &at = line[0].loc;
	std::string msg = gather_msg(line, 2);
	errors_.push_back(Error{at.file, Pos{at.line, at.column}, "#error " + msg, false});
	return false; // #error 停止编译
}

bool Preprocessor::handle_warning(const std::vector<Token> &line)
{
	const SourceLoc &at = line[0].loc;
	std::string msg = gather_msg(line, 2);
	errors_.push_back(Error{at.file, Pos{at.line, at.column}, "#warning " + msg, true});
	return true; // #warning 不停止
}

bool Preprocessor::handle_pragma(const std::vector<Token> &line)
{
	// line = ['#', "pragma", type, ...]
	if (line.size() < 3)
	{
		return true; // 空 pragma，忽略
	}
	const std::string &ptype = line[2].text;
	if (ptype == "once")
	{
		pragma_once_.insert(normalize_path(line[0].loc.file));
		return true;
	}
	// #pragma push_macro("name") / #pragma pop_macro("name")（clang/gcc 扩展，修 plct 00206）
	//   handle_directive 传入的是未宏展开的原始 line，故本行中的 push_macro/pop_macro
	//   不会被同名 #define 影响（正合 00206 顶部 “must not affect how #pragma ... works” 的约束）。
	if (ptype == "push_macro" || ptype == "pop_macro")
	{
		// 参数须为括号包裹的字符串字面量：line[3]=='(' line[4]=String。
		if (line.size() < 5 || !(line[3].kind == TokKind::Punct && line[3].text == "(") ||
			line[4].kind != TokKind::String)
		{
			errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
				"#pragma " + ptype + " requires a string-literal argument"});
			return true; // 容错：不中断预处理
		}
		std::string mname;
		if (line[4].text.size() >= 2)
		{
			mname = line[4].text.substr(1, line[4].text.size() - 2); // 去首尾双引号
		}
		if (ptype == "push_macro")
		{
			auto it = macros_.find(mname);
			push_macro_stack_[mname].push_back(
				it != macros_.end() ? std::optional<Macro>(it->second) : std::nullopt);
		}
		else // pop_macro
		{
			auto sit = push_macro_stack_.find(mname);
			if (sit == push_macro_stack_.end() || sit->second.empty())
			{
				return true; // 空栈：clang 为无操作
			}
			std::optional<Macro> saved = sit->second.back();
			sit->second.pop_back();
			if (saved)
			{
				macros_[mname] = *saved;
			}
			else
			{
				macros_.erase(mname);
			}
		}
		return true;
	}
	// 其他 pragma（mark/pack/GCC/...）：静默忽略
	return true;
}

bool Preprocessor::handle_line(const std::vector<Token> &line)
{
	// #line <number> ["filename"]
	// §6.10.3.4：#line 的参数行须先做宏替换（如 `#define line 1000` 后 `#line line`）。
	// c-testsuite 00152。
	const SourceLoc &at = line[0].loc;
	if (line.size() < 3)
	{
		errors_.push_back(Error{at.file, Pos{at.line, at.column}, "#line requires an integer"});
		return false;
	}
	std::vector<Token> args(line.begin() + 2, line.end());
	std::vector<Token> exp;
	expand_seq(args, exp, std::unordered_set<std::string>{}, false);
	if (exp.empty() || exp[0].kind != TokKind::Number)
	{
		errors_.push_back(Error{at.file, Pos{at.line, at.column}, "#line requires an integer"});
		return false;
	}
	const std::size_t new_line = static_cast<std::size_t>(std::stoul(exp[0].text));
	std::string new_file;
	if (exp.size() >= 2 && exp[1].kind == TokKind::String)
	{
		new_file = exp[1].text;
		if (new_file.size() >= 2)
		{
			new_file = new_file.substr(1, new_file.size() - 2);
		}
	}
	// 设置当前源 Lexer 的行号/可选文件名
	if (!src_stack_.empty())
	{
		Src &s = *src_stack_.back();
		s.lexer.set_line(new_line);
		if (!new_file.empty())
		{
			s.lexer.set_file(new_file);
		}
		// 修正前瞻 token（已由扫描器赋予旧位置）
		if (s.have_look)
		{
			s.look.loc.line = new_line;
			if (!new_file.empty())
			{
				s.look.loc.file = new_file;
			}
		}
	}
	return true;
}

bool Preprocessor::handle_directive(const std::vector<Token> &line)
{
	if (line.size() < 2)
	{
		errors_.push_back(Error{line[0].loc.file, Pos{line[0].loc.line, line[0].loc.column},
			"invalid preprocessing directive"});
		return false;
	}
	const Token &name_tok = line[1];
	if (name_tok.kind != TokKind::Identifier && name_tok.kind != TokKind::Keyword)
	{
		errors_.push_back(
			Error{name_tok.loc.file, Pos{name_tok.loc.line, name_tok.loc.column}, "invalid preprocessing directive name"});
		return false;
	}
	const std::string &name = name_tok.text;
	if (name == "if")
	{
		return handle_if(line);
	}
	if (name == "ifdef")
	{
		return handle_ifdef(line, false);
	}
	if (name == "ifndef")
	{
		return handle_ifdef(line, true);
	}
	if (name == "elif")
	{
		return handle_elif(line);
	}
	if (name == "elifdef")
	{
		return handle_elifdef(line, false);
	}
	if (name == "elifndef")
	{
		return handle_elifdef(line, true);
	}
	if (name == "else")
	{
		return handle_else(line);
	}
	if (name == "endif")
	{
		return handle_endif(line);
	}
	if (name == "define")
	{
		return is_active() ? handle_define(line) : true; // 非活跃分支不注册宏
	}
	if (name == "undef")
	{
		return is_active() ? handle_undef(line) : true;
	}
	if (name == "include")
	{
		return is_active() ? handle_include(line) : true; // 非活跃分支不包含
	}
	if (name == "error")
	{
		return is_active() ? handle_error(line) : true;
	}
	if (name == "warning")
	{
		return is_active() ? handle_warning(line) : true;
	}
	if (name == "pragma")
	{
		return is_active() ? handle_pragma(line) : true;
	}
	if (name == "line")
	{
		return is_active() ? handle_line(line) : true;
	}
	// 未知指令：仅活跃分支报错，否则静默忽略
	if (!is_active())
	{
		return true;
	}
	errors_.push_back(Error{name_tok.loc.file, Pos{name_tok.loc.line, name_tok.loc.column},
		"preprocessing directive '" + name + "' is not implemented yet"});
	return false;
}

bool Preprocessor::process_line(const std::vector<Token> &line, std::vector<Token> &out)
{
	if (line.empty())
	{
		return true;
	}
	const Token &head = line[0];
	if (head.kind == TokKind::Punct && head.text == "#")
	{
		// #embed 需直接向输出流发射 token（而非压新源/改状态），单独分派；非活跃分支跳过。
		if (line.size() >= 2 && (line[1].kind == TokKind::Identifier || line[1].kind == TokKind::Keyword) &&
			line[1].text == "embed")
		{
			return is_active() ? handle_embed(line, out) : true;
		}
		return handle_directive(line); // 指令行恒解析（条件栈须全程跟踪）；副作用指令内部按 is_active 门控
	}
	if (is_active())
	{
		expand_line(line, out); // 对象式宏展开后转发（P1.4）
	}
	return true;
}

// 合并 token 流中相邻的字符串字面量（翻译阶段 6）。String token 的 text 形如
// [前缀]"内容"（含引号、转义原样保留）。将 left 去掉结尾引号 + right 去掉至（含）
// 起始引号的前部拼为单个字面量，位置归 left。
static void concat_adjacent_strings(std::vector<Token> &out)
{
	std::vector<Token> merged;
	merged.reserve(out.size());
	for (std::size_t i = 0; i < out.size(); ++i)
	{
		Token t = out[i];
		if (t.kind == TokKind::String)
		{
			// 累积向后紧邻的所有 String token
			while (i + 1 < out.size() && out[i + 1].kind == TokKind::String)
			{
				const std::string &right = out[i + 1].text;
				const std::size_t rq = right.find('"'); // 右字面量起始引号
				std::string body = (rq == std::string::npos) ? std::string() : right.substr(rq + 1);
				if (!t.text.empty() && t.text.back() == '"')
				{
					t.text.pop_back(); // 去左字面量结尾引号
				}
				t.text += body; // 接回右字面量（含其自身结尾引号）
				++i;
			}
		}
		merged.push_back(t);
	}
	out.swap(merged);
}

// 自动加载：解析并把 colang.h 压入源栈。压栈范式同 handle_include（angle 形式→仅搜
// include_dirs；normalize_path + pragma_once_ 去重），区别在找不到时静默跳过、不报错，
// 且仅由 run() 在压入主源后调用一次。
void Preprocessor::push_builtin_include(const std::string &name)
{
	std::string cand;
	if (!resolve_header_path(name, false, std::string(), opts_, cand))
	{
		return; // 未随编译器附带本头：静默跳过、不影响既有行为
	}
	std::string text;
	if (!read_whole_file(cand, text))
	{
		return;
	}
	if (pragma_once_.count(normalize_path(cand)))
	{
		return; // 防御：colang.h 现用包含守卫而非 #pragma once，此分支当前恒不触发
	}
	src_stack_.push_back(std::make_unique<Src>(cand, std::move(text)));
	if (opts_.rc_dialect) // RC 方言：内建包含源同样按 rc.exe 规则切串
		src_stack_.back()->lexer.set_rc_string_mode(true);
}

bool Preprocessor::run(std::vector<Token> &out)
{
	src_stack_.push_back(std::make_unique<Src>(opts_.source_path, opts_.source_text));
	if (opts_.rc_dialect) // RC 方言：主源（.rc）按 rc.exe 规则切串
		src_stack_.back()->lexer.set_rc_string_mode(true);
	if (opts_.preload_colang_h)
	{
		// 再压 → colang.h 位于主源之上、先被拉取展开，EOF 后弹回主源；本头仅定义宏→零输出 token
		push_builtin_include("colang.h");
	}
	for (;;)
	{
		std::vector<Token> line;
		const LineResult lr = read_logical_line(line);
		if (lr == LineResult::Error)
		{
			return false;
		}
		if (lr == LineResult::Eof)
		{
			break;
		}
		if (!process_line(line, out))
		{
			return false;
		}
	}
	if (!cond_stack_.empty())
	{
		errors_.push_back(Error{opts_.source_path, Pos{1, 1}, "unterminated conditional directive"});
		return false;
	}
	// 阶段 6：相邻字符串字面量拼接（§6.4.5/6.7.9）。宏展开后 token 流中严格相邻的
	// String token 必为拼接（逗号/分号/运算符均会隔开），故在此单点合并即可让下游
	// （parser/sema/codegen/全局初始化折叠）一致视单个字面量。c-testsuite 00115。
	// RC 方言例外：.rc 合法允许一行并列多个字符串 token（名/类型/文件），不做此合并。
	if (!opts_.rc_dialect)
		concat_adjacent_strings(out);
	return true;
}

// ============================================================
// 驱动管线入口
// ============================================================
bool preprocess(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors)
{
	Preprocessor pp(opts, errors);
	return pp.run(state.tokens);
}

}  // namespace colang
