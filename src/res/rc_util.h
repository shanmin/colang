// ============================================================
// rc_util.h — 内置 RC 编译器内部工具（仅 src/res/*.cpp 包含，不对外）
//
// 集中放：整文件读字节 / 小端读写 / UTF-8→UTF-16 / 4 字节对齐填充。
// 皆为 inline，避免为纯工具函数单开 TU；调用点局限于资源子系统。
// ============================================================
#pragma once

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace colang
{
namespace rcutil
{

// 整文件读入字节向量；失败返回 false（不抛）。以二进制打开，逐字节保真。
inline bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return f.bad() ? false : true;
}

// 小端写入。
inline void put_u16(std::vector<uint8_t> &v, uint16_t x)
{
	v.push_back(static_cast<uint8_t>(x & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
inline void put_u32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back(static_cast<uint8_t>(x & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

// 当前长度补零至 4 字节对齐（WIN_RES_HEADER_ALIGNMENT / WIN_RES_DATA_ALIGNMENT）。
inline void pad4(std::vector<uint8_t> &v)
{
	while (v.size() % 4 != 0)
		v.push_back(0);
}

// 对齐后的尺寸（不落字节，供计算 Length 字段）。
inline std::size_t align4(std::size_t n) { return (n + 3) & ~static_cast<std::size_t>(3); }

// 小端读取（越界返回 0，调用方应先校验长度）。
inline uint16_t rd_u16(const std::vector<uint8_t> &b, std::size_t off)
{
	if (off + 2 > b.size())
		return 0;
	return static_cast<uint16_t>(b[off]) | static_cast<uint16_t>(b[off + 1] << 8);
}
inline uint32_t rd_u32(const std::vector<uint8_t> &b, std::size_t off)
{
	if (off + 4 > b.size())
		return 0;
	return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
		(static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

// UTF-8 字节串 → UTF-16 码元序列（标准变长解码；非法字节按原值零扩展兜底，不崩）。
inline std::u16string utf8_to_u16(const std::string &s)
{
	std::u16string out;
	const std::size_t n = s.size();
	const auto cont = [&](std::size_t k) {
		return k < n && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
	};
	for (std::size_t i = 0; i < n;)
	{
		const unsigned char b0 = static_cast<unsigned char>(s[i]);
		uint32_t cp = b0;
		std::size_t used = 1;
		if (b0 < 0x80) { cp = b0; used = 1; }
		else if ((b0 & 0xE0) == 0xC0 && cont(i + 1))
		{
			cp = ((b0 & 0x1FU) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3FU);
			used = 2;
		}
		else if ((b0 & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2))
		{
			cp = ((b0 & 0x0FU) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 6) |
				(static_cast<unsigned char>(s[i + 2]) & 0x3FU);
			used = 3;
		}
		else if ((b0 & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3))
		{
			cp = ((b0 & 0x07U) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 12) |
				((static_cast<unsigned char>(s[i + 2]) & 0x3FU) << 6) |
				(static_cast<unsigned char>(s[i + 3]) & 0x3FU);
			used = 4;
		}
		i += used;
		if (cp <= 0xFFFF)
		{
			out.push_back(static_cast<char16_t>(cp));
		}
		else
		{
			cp -= 0x10000;
			out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
			out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
		}
	}
	return out;
}

// rc.exe 字符串字面量解码（PE 资源档③ · RC 方言，经真值探针钉死，与 C co_decode_string 有分歧）。
// 前提：rc-mode 词法器已按 rc.exe 规则切串——"" 双写原样保留、反斜杠不在词法层遮蔽、
//       续行(\+换行)已删。此处只做语义解码：
//   剥编码前缀 u8/u/U/L + 首尾双引号 →
//   ""（双写）→ 单个 " ；
//   识别 \a→0x08（注意：非 C 的 0x07）\n→0x0A \r→0x0D \t→0x09 \\→0x5C；
//   八进制 \<0-7>{1,3} → 低字节（≤3 位定宽、非贪心）；十六进制 \x<hex>{1,2} → 低字节（≤2 位定宽）；
//   其余 \X → 字面保留反斜杠 + X（rc.exe 不识别即原样，与 C 丢反斜杠相反）。
// 边界：> 0x7F 字节 / \777 等走 rc.exe 码页映射（属 #pragma code_page/多字符集后置范畴，本片不追）。
inline std::string rc_decode_string(const std::string &raw)
{
	std::string s = raw;
	// 剥编码前缀（词法器把前缀并入 text），保证含前缀时仍能正确剥首尾双引号
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
		if (c == '"' && i + 1 < n && s[i + 1] == '"')
		{
			out += '"'; i += 2; continue; // ""：双写折为单个引号
		}
		if (c == '\\' && i + 1 < n)
		{
			const char e = s[i + 1];
			if (e == 'a') { out += static_cast<char>(0x08); i += 2; continue; } // rc.exe 特异：\a=0x08
			if (e == 'n') { out += '\n'; i += 2; continue; }
			if (e == 'r') { out += '\r'; i += 2; continue; }
			if (e == 't') { out += '\t'; i += 2; continue; }
			if (e == '\\') { out += '\\'; i += 2; continue; }
			if (e == 'x') // 十六进制：最多 2 位
			{
				std::size_t j = i + 2; int v = 0; int d = 0;
				while (j < n && d < 2)
				{
					const char h = s[j]; int dig;
					if (h >= '0' && h <= '9') dig = h - '0';
					else if (h >= 'a' && h <= 'f') dig = h - 'a' + 10;
					else if (h >= 'A' && h <= 'F') dig = h - 'A' + 10;
					else break;
					v = v * 16 + dig; ++j; ++d;
				}
				if (d == 0) { out += '\\'; ++i; continue; } // \x 后无有位：字面保留
				out += static_cast<char>(static_cast<unsigned char>(v & 0xFF)); i = j; continue;
			}
			if (e >= '0' && e <= '7') // 八进制：最多 3 位
			{
				int v = e - '0'; std::size_t j = i + 2; int digits = 1;
				while (j < n && digits < 3 && s[j] >= '0' && s[j] <= '7')
				{
					v = v * 8 + (s[j] - '0'); ++j; ++digits;
				}
				out += static_cast<char>(static_cast<unsigned char>(v & 0xFF)); i = j; continue;
			}
			out += '\\'; out += e; i += 2; continue; // 未识别：字面保留反斜杠+字符
		}
		out += c; ++i;
	}
	return out;
}

}  // namespace rcutil
}  // namespace colang
