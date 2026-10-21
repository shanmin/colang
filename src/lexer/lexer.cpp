// ============================================================
// lexer.cpp — 预处理扫描器实现（P1.1 + P1.2；P1.3 起提供 next() 拉取）
//
// 翻译阶段 2–4：行拼接、注释替换为空白、单遍最长匹配切 pp-token。
// 不做宏展开/指令处理（预处理器职责，P1.3+）。
// ============================================================

#include "colang/lexer.h"

#include <unordered_set>

namespace colang
{

namespace
{

// —— 字符分类（按字节；UTF-8 高位字节 >= 0x80 视为 nondigit，允许出现在标识符中）——
inline bool is_utf8_cont(unsigned char b)
{
	return b >= 0x80 && b <= 0xBF;
}
inline bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}
inline bool is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline bool is_ident_nondigit(char c)
{
	return c == '_' || is_alpha(c) || static_cast<unsigned char>(c) >= 0x80;
}
inline bool is_hspace(char c)
{
	return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

// —— 运算符/标点表：由长到短，命中即最长匹配 ——
// src = 源码中的原始拼写；srclen = 原始字节数；out = 归一后主形式（nullptr 表示同 src）。
struct Punct
{
	const char *src;
	unsigned char srclen;
	const char *out; // digraph 归一时给出主形式
};

const Punct punct_table[] = {
	// 4 字节 digraph
	{"%:%:", 4, "##"},
	// 3 字节
	{"<<=", 3, nullptr},
	{">>=", 3, nullptr},
	{"...", 3, nullptr},
	{"<::", 3, "[["},
	// 2 字节
	{"->", 2, nullptr},
	{"++", 2, nullptr},
	{"--", 2, nullptr},
	{"<<", 2, nullptr},
	{">>", 2, nullptr},
	{"<=", 2, nullptr},
	{">=", 2, nullptr},
	{"==", 2, nullptr},
	{"!=", 2, nullptr},
	{"&&", 2, nullptr},
	{"||", 2, nullptr},
	{"*=", 2, nullptr},
	{"/=", 2, nullptr},
	{"%=", 2, nullptr},
	{"+=", 2, nullptr},
	{"-=", 2, nullptr},
	{"&=", 2, nullptr},
	{"|=", 2, nullptr},
	{"^=", 2, nullptr},
	{"##", 2, nullptr},
	{"<:", 2, "["},  // digraph
	{":>", 2, "]"},  // digraph
	{"%:", 2, "#"},  // digraph
	// 1 字节
	{"(", 1, nullptr},
	{")", 1, nullptr},
	{"[", 1, nullptr},
	{"]", 1, nullptr},
	{"{", 1, nullptr},
	{"}", 1, nullptr},
	{";", 1, nullptr},
	{",", 1, nullptr},
	{":", 1, nullptr},
	{".", 1, nullptr},
	{"&", 1, nullptr},
	{"!", 1, nullptr},
	{"~", 1, nullptr},
	{"-", 1, nullptr},
	{"+", 1, nullptr},
	{"*", 1, nullptr},
	{"/", 1, nullptr},
	{"%", 1, nullptr},
	{"<", 1, nullptr},
	{">", 1, nullptr},
	{"^", 1, nullptr},
	{"|", 1, nullptr},
	{"?", 1, nullptr},
	{"=", 1, nullptr},
	{"#", 1, nullptr},
};

}  // namespace

// ============================================================
// 关键字表（覆盖到 C23；不分版本档，无 -std 门控；具体方言基线由 sema/parser 定）
// ============================================================
bool is_keyword(const std::string &word)
{
	static const std::unordered_set<std::string> keywords = {
		// C89 32
		"auto", "break", "case", "char", "const", "continue", "default", "do",
		"double", "else", "enum", "extern", "float", "for", "goto", "if",
		"inline", "int", "long", "register", "return", "short", "signed",
		"sizeof", "struct", "switch", "typedef", "union", "unsigned",
		"void", "volatile", "while",
		// C99
		"_Bool", "_Complex", "_Imaginary", "restrict",
		// 标准类型名（C 里由 <stddef.h>/<wchar.h> 提供 typedef；此处按内置类型名识别，
		// 以便给出与宿主 CRT 一致的确定宽度，见 ADR-104 裁定）
		"wchar_t",
		// charN_t：C11 的 char16_t/char32_t、C23 的 char8_t 均为保留关键字且恒无符号
		"char8_t", "char16_t", "char32_t",
		// C11
		"_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn",
		"_Static_assert", "_Thread_local",
		// C23
		"alignas", "alignof", "asm", "bool", "constexpr", "false", "nullptr",
		"static_assert", "thread_local", "true", "typeof", "typeof_unqual",
		"_BitInt",
	};
	return keywords.count(word) != 0;
}

// ============================================================
// Lexer 基础前进原语
// ============================================================
Lexer::Lexer(std::string file, std::string text)
	: file_(std::move(file)), text_(std::move(text))
{
}

char Lexer::peek(std::size_t off) const
{
	return (pos_ + off < text_.size()) ? text_[pos_ + off] : '\0';
}

SourceLoc Lexer::here() const
{
	return SourceLoc{file_, line_, col_};
}

char Lexer::take()
{
	const char c = text_[pos_++];
	if (c == '\n')
	{
		++line_;
		col_ = 1;
	}
	else if (!is_utf8_cont(static_cast<unsigned char>(c)))
	{
		++col_; // 码点起算列：UTF-8 续字节不占列
	}
	return c;
}

bool Lexer::at_line_splice() const
{
	return peek(0) == '\\' && (peek(1) == '\n' || peek(1) == '\r');
}

void Lexer::skip_line_splice()
{
	take(); // 反斜杠
	if (peek(0) == '\r')
	{
		take();
	}
	if (peek(0) == '\n')
	{
		take();
	}
}

// ============================================================
// 各类 token 扫描
// ============================================================
void Lexer::scan_number(Token &t)
{
	std::string num;
	for (;;)
	{
		const char n = peek();
		// 指数/幂的带符号形式（须先于普通标识符字符判定，否则 '+' 处断链）
		if ((n == 'e' || n == 'E' || n == 'p' || n == 'P') &&
			(peek(1) == '+' || peek(1) == '-') && is_digit(peek(2)))
		{
			num += take();
			num += take();
			continue;
		}
		if (is_digit(n) || is_ident_nondigit(n) || n == '.')
		{
			num += take();
			continue;
		}
		break;
	}
	t.kind = TokKind::Number;
	t.text = std::move(num);
}

bool Lexer::scan_literal(Token &t, char quote, const std::string &prefix)
{
	t.kind = (quote == '\'') ? TokKind::Char : TokKind::String;
	t.text = prefix;
	t.text += take(); // 起始引号
	bool terminated = false;
	while (!eof())
	{
		if (at_line_splice())
		{
			skip_line_splice(); // 字面量内续行：phase 2 先删除，不断行
			continue;
		}
		const char c = peek();
		if (c == '\n' || c == '\r')
		{
			break; // 字面量不得跨真实换行
		}
		take();
		t.text += c;
		if (c == quote)
		{
			terminated = true;
			break;
		}
		if (c == '\\')
		{
			if (!eof() && peek() != '\n' && peek() != '\r')
			{
				t.text += take(); // 转义序列：连同被转义字符原样保留
			}
		}
	}
	return terminated;
}

bool Lexer::scan_punct(Token &t)
{
	for (const Punct &p : punct_table)
	{
		if (pos_ + p.srclen > text_.size())
		{
			continue;
		}
		bool hit = true;
		for (unsigned char k = 0; k < p.srclen; ++k)
		{
			if (text_[pos_ + k] != p.src[k])
			{
				hit = false;
				break;
			}
		}
		if (!hit)
		{
			continue;
		}
		t.kind = TokKind::Punct;
		t.text = p.out ? p.out : std::string(p.src, p.srclen);
		for (unsigned char k = 0; k < p.srclen; ++k)
		{
			take();
		}
		return true;
	}
	return false;
}

// 消费空白 / 换行 / 注释；期间遇到真实换行则置 pending_bol_。
// 返回 false 表示出错（未终止块注释），err 已填。
bool Lexer::skip_blanks(Error &err)
{
	for (;;)
	{
		if (at_line_splice())
		{
			skip_line_splice(); // 续行不算逻辑行结束
			continue;
		}
		if (eof())
		{
			return true;
		}
		const char c = peek();
		if (is_hspace(c))
		{
			take();
			pending_space_ = true; // 水平空白计入 token 间邻接
			continue;
		}
		if (c == '\r' || c == '\n')
		{
			take();
			pending_bol_ = true;
			pending_space_ = false; // 换行重置邻接（行首 token 无前置）
			continue;
		}
		if (c == '/' && peek(1) == '/')
		{
			pending_space_ = true; // 注释等同一个空格（phase 3）
			take();
			take();
			while (!eof() && peek() != '\n' && peek() != '\r')
			{
				if (at_line_splice())
				{
					skip_line_splice(); // 续行延长行注释
					continue;
				}
				take();
			}
			continue;
		}
		if (c == '/' && peek(1) == '*')
		{
			pending_space_ = true; // 注释等同一个空格（phase 3）
			const SourceLoc start = here();
			take();
			take();
			bool closed = false;
			while (!eof())
			{
				if (at_line_splice())
				{
					skip_line_splice();
					continue;
				}
				if (peek() == '*' && peek(1) == '/')
				{
					take();
					take();
					closed = true;
					break;
				}
				take();
			}
			if (!closed)
			{
				err = Error{file_, Pos{start.line, start.column}, "unterminated block comment"};
				return false;
			}
			continue;
		}
		return true;
	}
}

// ============================================================
// 单步拉取
// ============================================================
LexResult Lexer::next(Token &out, Error &err)
{
	if (!skip_blanks(err))
	{
		return LexResult::Error;
	}

	if (eof())
	{
		out = Token{};
		out.kind = TokKind::EndOfFile;
		out.loc = here();
		out.bol = pending_bol_;
		return LexResult::Eof;
	}

	Token t;
	t.loc = here();
	t.bol = pending_bol_;
	t.had_space = !pending_bol_ && pending_space_; // 行首不算邻接
	pending_bol_ = false;
	pending_space_ = false; // 已消费本段间隔，下段重新累计
	const char c = peek();

	if (is_ident_nondigit(c))
	{
		std::string word;
		while (!eof() && (is_ident_nondigit(peek()) || is_digit(peek())))
		{
			word += take();
		}
		// 前缀紧贴 ' 或 " → 带前缀的字符/字符串常数（如 u8"x" / L'y'）
		if (peek() == '\'' || peek() == '"')
		{
			const char quote = peek();
			if (!scan_literal(t, quote, word))
			{
				err = Error{file_, Pos{t.loc.line, t.loc.column},
					"unterminated character or string literal"};
				return LexResult::Error;
			}
			out = std::move(t);
			return LexResult::Token;
		}
		t.kind = is_keyword(word) ? TokKind::Keyword : TokKind::Identifier;
		t.text = std::move(word);
		out = std::move(t);
		return LexResult::Token;
	}

	if (is_digit(c) || (c == '.' && is_digit(peek(1))))
	{
		scan_number(t);
		out = std::move(t);
		return LexResult::Token;
	}

	if (c == '\'' || c == '"')
	{
		if (!scan_literal(t, c, std::string()))
		{
			err = Error{file_, Pos{t.loc.line, t.loc.column},
				"unterminated character or string literal"};
			return LexResult::Error;
		}
		out = std::move(t);
		return LexResult::Token;
	}

	if (!scan_punct(t))
	{
		err = Error{file_, Pos{t.loc.line, t.loc.column},
			std::string("stray '") + c + "' in program"};
		return LexResult::Error;
	}
	out = std::move(t);
	return LexResult::Token;
}

// ============================================================
// 整缓冲切分（薄封装，--dump-tokens 用）
// ============================================================
bool Lexer::tokenize(std::vector<Token> &out, std::vector<Error> &errors)
{
	for (;;)
	{
		Token t;
		Error err;
		const LexResult r = next(t, err);
		if (r == LexResult::Error)
		{
			errors.push_back(std::move(err));
			return false;
		}
		out.push_back(std::move(t)); // Token 或 EndOfFile 均入列
		if (r == LexResult::Eof)
		{
			return true;
		}
	}
}

}  // namespace colang
