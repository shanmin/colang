// ============================================================
// common.h — 跨层公共设施：源文件位置 + 统一诊断结构 + 语言标准档
// ============================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace colang
{

// 源文件中的位置；行列均从 1 起，列按 UTF-8 码点计（续字节不计列）
struct Pos
{
	std::size_t line = 1;
	std::size_t column = 1;
};

// 统一诊断信息；用户可见文本一律英文，输出格式固定为
//   文件:行:列: error|warning: 消息
struct Error
{
	std::string file;
	Pos pos;
	std::string message;
	bool is_warning = false; // #warning 等非致命诊断
};

// token 的源位置：文件 + 行列（行列从 1 起，列按 UTF-8 码点计）。
// 预处理会把多个文件（#include）拼成一个流，故每个 token 自带文件名，
// 下游诊断据此回溯到真实出处。
struct SourceLoc
{
	std::string file;
	std::size_t line = 1;
	std::size_t column = 1;
};

// 语言标准档已取消：本项只实现单一 “C 超集方言”，不按 -std 做版本门控/诊断。
// （最终 Co 语言会彻底消除该规则；C89→C23 仅作为施工顺序，非对外开关。）

// 字符字面量解码（跨层单一真值源）：raw 为词元原文，形如 'A' / '\n' / '\x41'（含首尾单引号）。
// 覆盖常见转义与 \x 十六进制 / \NNN 八进制；无法识别时取首字节。parser 在生成
// CharLiteral 时即写入 int_val，供 eval_const_int/静态初始化/codegen 统一取值。
inline unsigned char co_decode_char(const std::string &raw)
{
	std::string s = raw;
	// 先去字符编码前缀 u8'/u'/U'/L'（词法器将前缀并入 text），保证含前缀时仍能正确剥引号
	if (!s.empty() && s.front() != '\'')
	{
		const std::size_t q = s.find('\'');
		if (q != std::string::npos)
		{
			const std::string pre = s.substr(0, q);
			if (pre == "u8" || pre == "u" || pre == "U" || pre == "L")
				s = s.substr(q); // 从前引号起截取
		}
	}
	if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
		s = s.substr(1, s.size() - 2); // 去首尾单引号
	if (s.empty()) return 0;
	if (s[0] != '\\') return static_cast<unsigned char>(s[0]);
	if (s.size() < 2) return static_cast<unsigned char>(s[0]);
	char e = s[1];
	switch (e)
	{
	case 'n': return '\n';
	case 't': return '\t';
	case 'r': return '\r';
	case 'a': return '\a';
	case 'b': return '\b';
	case 'f': return '\f';
	case 'v': return '\v';
	case 'e': return 0x1B;
	case '\\': return '\\';
	case '\'': return '\'';
	case '"': return '"';
	case '?': return '?';
	case 'x': case 'X':
	{
		int v = 0; std::size_t i = 2;
		while (i < s.size())
		{
			char c = s[i]; int d;
			if (c >= '0' && c <= '9') d = c - '0';
			else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
			else break;
			v = v * 16 + d; ++i;
			if (v > 0xFF) v = 0xFF; // 溢出钳位（超字节 char 属边界，暂按 255）
		}
		return static_cast<unsigned char>(v);
	}
	default:
		if (e >= '0' && e <= '7') // 八进制
		{
			int v = e - '0'; std::size_t i = 2;
			while (i < s.size())
			{
				char c = s[i];
				if (c < '0' || c > '7') break;
				v = v * 8 + (c - '0'); ++i;
				if (v > 0xFF) v = 0xFF;
			}
			return static_cast<unsigned char>(v);
		}
		return static_cast<unsigned char>(e);
	}
}

// 字符字面量编码前缀（跨层单一真值源）：raw 形如 L'A' / u'A' / U'A' / u8'A' / 'A'（前缀与引号均在其内）。
// 返回 "u8" / "u" / "U" / "L" / ""（无前缀）。前缀决定字面量类型（ISO C §6.4.4.4）：
//   L→wchar_t、u→char16_t、U→char32_t、u8→char8_t、无前缀→int。
inline std::string co_char_lit_prefix(const std::string &raw)
{
	if (raw.empty() || raw.front() == '\'') return std::string();
	const std::size_t q = raw.find('\'');
	if (q == std::string::npos) return std::string();
	const std::string pre = raw.substr(0, q);
	if (pre == "u8" || pre == "u" || pre == "U" || pre == "L") return pre;
	return std::string();
}

// ---- 通用字符名 UCN（ISO C §6.4.3：\uXXXX / \UXXXXXXXX）----
// 跨层单一真值源：parser 用 co_ucn_validate 报诊断；各解码器入口先用 co_expand_ucn 把 UCN
//   就地展开为 UTF-8 字节，随后沿用既有「按元素宽度/编码分派」（窄字节、UTF-16、UTF-32），
//   不必在每种编码里各写一份 UCN 逻辑。当前范围＝**字面量内**（字符/字符串常量）；
//   标识符里的 UCN（`int \u00e9;`）尚未支持。
// 语法约束（§6.4.3p1）：\u 固定 4 位十六进制、\U 固定 8 位。

// 从 raw[i] 起解析一个 UCN：是 UCN 则返回消耗字节数并写 cp，否则返回 0（含位数/字符不足）。
inline std::size_t co_ucn_parse(const std::string &raw, std::size_t i, uint32_t &cp)
{
	if (i + 1 >= raw.size() || raw[i] != '\\') return 0;
	const char k = raw[i + 1];
	const std::size_t digits = (k == 'u') ? 4 : (k == 'U') ? 8 : 0;
	if (digits == 0 || i + 2 + digits > raw.size()) return 0;
	uint32_t v = 0;
	for (std::size_t j = 0; j < digits; ++j)
	{
		const char c = raw[i + 2 + j];
		unsigned d;
		if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
		else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
		else return 0;
		v = v * 16 + d;
	}
	cp = v;
	return 2 + digits;
}

// UCN 码点合法性：合法返回空串，否则返回英文诊断文本（与 clang 同文）。
//   口径取 **C23**（本项目「单一 C 超集方言、倾向 C23」、不按 -std 门控）：
//   代理区 D800–DFFF 与 > 0x10FFFF 仍为错；C11/C17 额外的「除 $ @ ` 外不得用 UCN 表示
//   < 0xA0 字符」已被 C23 放宽，本实现随 C23 接受（clang：-std=c11 报 character 'A'
//   cannot be specified …，-std=c23 则接受）。
inline std::string co_ucn_check(uint32_t cp)
{
	if (cp > 0x10FFFF) return "invalid universal character name: code point out of range";
	if (cp >= 0xD800 && cp <= 0xDFFF) return "invalid universal character name: surrogate code point";
	return std::string();
}

// 扫描字面量原文中的 UCN：返回首个诊断文本（无问题→空串）。普通转义对（\n、\\ 等）整体跳过，
//   故 `"\\u4e2d"`（反斜杠 + u4e2d 字面文本）不会被误当作 UCN。
inline std::string co_ucn_validate(const std::string &raw)
{
	for (std::size_t i = 0; i + 1 < raw.size(); ++i)
	{
		if (raw[i] != '\\') continue;
		uint32_t cp = 0;
		const std::size_t len = co_ucn_parse(raw, i, cp);
		if (len == 0)
		{
			const char k = raw[i + 1];
			if (k == 'u' || k == 'U') return "incomplete universal character name";
			++i; // 普通转义：跳过被转义字符
			continue;
		}
		const std::string why = co_ucn_check(cp);
		if (!why.empty()) return why;
		i += len - 1;
	}
	return std::string();
}

// 取字面量原文中首个 UCN 的码点（供窄字符常量判定「是否超出 char 可表示范围」）。
inline bool co_ucn_first(const std::string &raw, uint32_t &cp)
{
	for (std::size_t i = 0; i + 1 < raw.size(); ++i)
	{
		if (raw[i] != '\\') continue;
		uint32_t v = 0;
		const std::size_t len = co_ucn_parse(raw, i, v);
		if (len == 0) { ++i; continue; }
		cp = v;
		return true;
	}
	return false;
}

// UTF-8 追加一个码点（co_expand_ucn 专用，与 co_decode_string 的读向互逆）。
inline void co_utf8_append(std::string &out, uint32_t cp)
{
	if (cp < 0x80) { out += static_cast<char>(cp); return; }
	if (cp < 0x800)
	{
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
		return;
	}
	if (cp < 0x10000)
	{
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
		return;
	}
	out += static_cast<char>(0xF0 | (cp >> 18));
	out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
	out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
	out += static_cast<char>(0x80 | (cp & 0x3F));
}

// 把字面量原文中的合法 UCN 就地展开为 UTF-8 字节（其余字节原样保留）。
//   非法 UCN 亦原样保留——非法情形由 co_ucn_validate 在编译期报错，此处只求不崩。
//   非 UCN 的转义对整体搬运（`\X` 两字节），避免 `\\u4e2d` 的后半段被误展开。
inline std::string co_expand_ucn(const std::string &raw)
{
	std::string out;
	out.reserve(raw.size());
	for (std::size_t i = 0; i < raw.size(); )
	{
		if (raw[i] == '\\' && i + 1 < raw.size())
		{
			uint32_t cp = 0;
			const std::size_t len = co_ucn_parse(raw, i, cp);
			if (len > 0 && co_ucn_check(cp).empty())
			{
				co_utf8_append(out, cp);
				i += len;
				continue;
			}
			out += raw[i];
			out += raw[i + 1];
			i += 2;
			continue;
		}
		out += raw[i];
		++i;
	}
	return out;
}

// 带前缀字符常量解码为完整码点（跨层单一真值源，供 L/u/U 前缀）。
//   非转义内容按 UTF-8 变长解码取首码点（源码以 UTF-8 读入）；转义按 ISO C 语义求值，
//   此处 \x / \NNN 不再钳到 0xFF，而是钳到 Unicode 码点上限 0x10FFFF。
// 与 co_decode_char 的分工：后者维持「窄字符取首字节、\x 钳 0xFF」的历史口径不动（窄前缀
// 仍走它、逐字零变化），本函数只服务需要完整码点的宽前缀，避免动到已稳固的窄路径。
inline uint32_t co_decode_char_wide(const std::string &raw)
{
	std::string s = co_expand_ucn(raw); // UCN（\u/\U）先展开为 UTF-8，再走下方既有的变长解码
	const std::string pre = co_char_lit_prefix(raw);
	if (!pre.empty()) s = s.substr(pre.size());
	if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
		s = s.substr(1, s.size() - 2); // 去首尾单引号
	if (s.empty()) return 0;
	if (s[0] != '\\')
	{
		const unsigned char b0 = static_cast<unsigned char>(s[0]);
		const auto cont = [&](std::size_t k) {
			return k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
		};
		if (b0 < 0x80) return b0;
		if ((b0 & 0xE0) == 0xC0 && cont(1))
			return ((b0 & 0x1FU) << 6) | (static_cast<unsigned char>(s[1]) & 0x3FU);
		if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2))
			return ((b0 & 0x0FU) << 12) | ((static_cast<unsigned char>(s[1]) & 0x3FU) << 6) |
				(static_cast<unsigned char>(s[2]) & 0x3FU);
		if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3))
			return ((b0 & 0x07U) << 18) | ((static_cast<unsigned char>(s[1]) & 0x3FU) << 12) |
				((static_cast<unsigned char>(s[2]) & 0x3FU) << 6) |
				(static_cast<unsigned char>(s[3]) & 0x3FU);
		return b0; // 截断/非法续字节：按原字节兜底（不崩）
	}
	if (s.size() < 2) return static_cast<unsigned char>(s[0]);
	const char e = s[1];
	switch (e)
	{
	case 'n': return '\n';
	case 't': return '\t';
	case 'r': return '\r';
	case 'a': return '\a';
	case 'b': return '\b';
	case 'f': return '\f';
	case 'v': return '\v';
	case 'e': return 0x1B;
	case '\\': return '\\';
	case '\'': return '\'';
	case '"': return '"';
	case '?': return '?';
	case 'x': case 'X':
	{
		uint32_t v = 0; std::size_t i = 2;
		while (i < s.size())
		{
			const char c = s[i]; unsigned d;
			if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
			else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
			else break;
			v = v * 16 + d; ++i;
			if (v > 0x10FFFF) v = 0x10FFFF; // 码点上限钳位
		}
		return v;
	}
	default:
		if (e >= '0' && e <= '7') // 八进制：ISO C 至多 3 位
		{
			uint32_t v = static_cast<uint32_t>(e - '0'); std::size_t i = 2; int n = 1;
			while (i < s.size() && n < 3)
			{
				const char c = s[i];
				if (c < '0' || c > '7') break;
				v = v * 8 + static_cast<uint32_t>(c - '0'); ++i; ++n;
			}
			return v;
		}
		return static_cast<unsigned char>(e);
	}
}

// 字符串字面量解码（跨层单一真值源）：raw 为词元原文，形如 "abc" / "a\nb" / L"x"（含首尾双
// 引号及可能的编码前缀 u8"/u"/U"/L"）。返回解码后的内容字节序列（不含末尾 '\0'）。
// 转义覆盖与 co_decode_char 对齐（\n \t \r \a \b \f \v \e \\ \' \" \? \xHH / \NNN 八进制）。
// 调用方按需自行补 '\0'（如 codegen 表达式路径）或用返回值长度推导数组大小（如 parser）。
inline std::string co_decode_string(const std::string &raw)
{
	std::string s = co_expand_ucn(raw); // UCN（\u/\U）先展开为 UTF-8，供下游按元素宽度重编码
	// 剥编码前缀 u8/u/U/L（词法器将前缀并入 text），保证含前缀时仍能正确剥双引号
	if (!s.empty() && s.front() != '"')
	{
		const std::size_t q = s.find('"');
		if (q != std::string::npos)
		{
			const std::string pre = s.substr(0, q);
			if (pre == "u8" || pre == "u" || pre == "U" || pre == "L")
				s = s.substr(q);
		}
	}
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		s = s.substr(1, s.size() - 2); // 去首尾双引号
	std::string out;
	const std::size_t n = s.size();
	for (std::size_t i = 0; i < n; )
	{
		const char c = s[i];
		if (c == '\\' && i + 1 < n)
		{
			++i;
			const char e = s[i];
			switch (e)
			{
			case 'n': out += '\n'; ++i; break;
			case 't': out += '\t'; ++i; break;
			case 'r': out += '\r'; ++i; break;
			case 'a': out += '\a'; ++i; break;
			case 'b': out += '\b'; ++i; break;
			case 'f': out += '\f'; ++i; break;
			case 'v': out += '\v'; ++i; break;
			case 'e': out += static_cast<char>(0x1B); ++i; break;
			case '\\': out += '\\'; ++i; break;
			case '\'': out += '\''; ++i; break;
			case '"': out += '"'; ++i; break;
			case '?': out += '?'; ++i; break;
			case 'x': case 'X':
			{
				++i;
				int v = 0;
				while (i < n)
				{
					const char h = s[i]; int d;
					if (h >= '0' && h <= '9') d = h - '0';
					else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
					else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
					else break;
					v = v * 16 + d; ++i;
					if (v > 0xFF) v = 0xFF;
				}
				out += static_cast<char>(static_cast<unsigned char>(v));
				break;
			}
			default:
				if (e >= '0' && e <= '7') // 八进制
				{
					int v = e - '0'; ++i;
					while (i < n)
					{
						const char o = s[i];
						if (o < '0' || o > '7') break;
						v = v * 8 + (o - '0'); ++i;
						if (v > 0xFF) v = 0xFF;
					}
					out += static_cast<char>(static_cast<unsigned char>(v));
				}
				else
				{
					out += e; ++i; // 未识别转义：原样保留被转义字符
				}
				break;
			}
		}
		else
		{
			out += c; ++i;
		}
	}
	return out;
}

// 字符串字面量前缀 → 元素字节宽度（跨层单一真值源，与 co_decode_string 的剥前缀口径完全一致）。
// 前缀集对齐 co_decode_string/co_decode_char：u8→1、u→2、U→4、L→2（本机 wchar_t=2 字节 UTF-16，
// ADR-104）、无前缀→1。识别方式同 co_decode_string：取首个双引号前的子串判定。供 codegen 右值
// StringLiteral 按 C §6.4.5「前缀决定字面量元素类型」三路编码发射与类型解析据此分派。
inline int co_str_lit_width(const std::string &raw)
{
	if (!raw.empty() && raw.front() != '"')
	{
		const std::size_t q = raw.find('"');
		if (q != std::string::npos)
		{
			const std::string pre = raw.substr(0, q);
			if (pre == "u8") return 1;
			if (pre == "u")  return 2;
			if (pre == "U")  return 4;
			if (pre == "L")  return 2; // 本机 wchar_t = 2 字节 UTF-16（ADR-104）
		}
	}
	return 1; // 无前缀：char 数组（UTF-8 字节）
}

// 宽字符串字面量解码为 UTF-16 码元（跨层单一真值源，供 wchar_t[] 初始化；parser/codegen 共用）。
// 先经 co_decode_string 取剥前缀/解转义后的 UTF-8 内容字节，再按标准 UTF-8 变长解码得码点，
// 重编为 UTF-16：码点 <= 0xFFFF 直接一个码元；> 0xFFFF 生成高低代理对（0xD800..0xDFFF）。
// 非法/截断字节按原值零扩展兜底（不崩）。本机 wchar_t = 2 字节 UTF-16（P9.18/ADR-104）。
inline std::vector<uint16_t> co_decode_utf16(const std::string &raw)
{
	const std::string bytes = co_decode_string(raw); // UTF-8 内容字节（不含 null）
	std::vector<uint16_t> units;
	const std::size_t n = bytes.size();
	const auto cont = [&](std::size_t k) { return k < n && (static_cast<unsigned char>(bytes[k]) & 0xC0) == 0x80; };
	for (std::size_t i = 0; i < n; )
	{
		const unsigned char b0 = static_cast<unsigned char>(bytes[i]);
		unsigned int cp = b0;
		std::size_t used = 1;
		if (b0 < 0x80) { cp = b0; used = 1; }
		else if ((b0 & 0xE0) == 0xC0 && cont(i + 1))
		{
			cp = ((b0 & 0x1FU) << 6) | (static_cast<unsigned char>(bytes[i + 1]) & 0x3FU);
			used = 2;
		}
		else if ((b0 & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2))
		{
			cp = ((b0 & 0x0FU) << 12) | ((static_cast<unsigned char>(bytes[i + 1]) & 0x3FU) << 6) |
				(static_cast<unsigned char>(bytes[i + 2]) & 0x3FU);
			used = 3;
		}
		else if ((b0 & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3))
		{
			cp = ((b0 & 0x07U) << 18) | ((static_cast<unsigned char>(bytes[i + 1]) & 0x3FU) << 12) |
				((static_cast<unsigned char>(bytes[i + 2]) & 0x3FU) << 6) |
				(static_cast<unsigned char>(bytes[i + 3]) & 0x3FU);
			used = 4;
		}
		i += used;
		if (cp <= 0xFFFF)
		{
			units.push_back(static_cast<uint16_t>(cp));
		}
		else
		{
			cp -= 0x10000;
			units.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
			units.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
		}
	}
	return units;
}

// 宽字符串字面量解码为 UTF-32 码点（跨层单一真值源，供 char32_t[] 初始化；parser/codegen 共用）。
// 先经 co_decode_string 取剥前缀/解转义后的 UTF-8 内容字节，再按标准 UTF-8 变长解码得码点，
// 一字一码点（BMP 与 Supplementary 均存原码点、无代理）。非法/截断字节按原值零扩展兜底（不崩）。
inline std::vector<uint32_t> co_decode_utf32(const std::string &raw)
{
	const std::string bytes = co_decode_string(raw); // UTF-8 内容字节（不含 null）
	std::vector<uint32_t> cps;
	const std::size_t n = bytes.size();
	const auto cont = [&](std::size_t k) { return k < n && (static_cast<unsigned char>(bytes[k]) & 0xC0) == 0x80; };
	for (std::size_t i = 0; i < n; )
	{
		const unsigned char b0 = static_cast<unsigned char>(bytes[i]);
		unsigned int cp = b0;
		std::size_t used = 1;
		if (b0 < 0x80) { cp = b0; used = 1; }
		else if ((b0 & 0xE0) == 0xC0 && cont(i + 1))
		{
			cp = ((b0 & 0x1FU) << 6) | (static_cast<unsigned char>(bytes[i + 1]) & 0x3FU);
			used = 2;
		}
		else if ((b0 & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2))
		{
			cp = ((b0 & 0x0FU) << 12) | ((static_cast<unsigned char>(bytes[i + 1]) & 0x3FU) << 6) |
				(static_cast<unsigned char>(bytes[i + 2]) & 0x3FU);
			used = 3;
		}
		else if ((b0 & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3))
		{
			cp = ((b0 & 0x07U) << 18) | ((static_cast<unsigned char>(bytes[i + 1]) & 0x3FU) << 12) |
				((static_cast<unsigned char>(bytes[i + 2]) & 0x3FU) << 6) |
				(static_cast<unsigned char>(bytes[i + 3]) & 0x3FU);
			used = 4;
		}
		i += used;
		cps.push_back(static_cast<uint32_t>(cp)); // 直接存码点，不分代理（与 UTF-16 唯一差异）
	}
	return cps;
}

// 字符串字面量作为**数组对象**时的元素个数（含末尾 '\0'，ISO C §6.4.5p6/p7）。
// 与 co_str_lit_width 同源定元素宽度：宽 1（窄/u8）→ UTF-8 字节数、宽 2（L/u）→ UTF-16 码元数、
//   宽 4（U）→ 码点数，各 +1 补终止符。供 sizeof(字面量) 等**不施加数组→指针退化**的语境使用。
inline std::size_t co_str_lit_len(const std::string &raw)
{
	switch (co_str_lit_width(raw))
	{
	case 2: return co_decode_utf16(raw).size() + 1;
	case 4: return co_decode_utf32(raw).size() + 1;
	default: return co_decode_string(raw).size() + 1;
	}
}

}  // namespace colang
