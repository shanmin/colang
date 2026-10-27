// ============================================================
// version_info.cpp — VERSIONINFO 块 → VS_VERSION_INFO 资源数据
//
// VS_VERSION_INFO 是嵌套、4 字节（DWORD）对齐的结构族。每个节点：
//   {u16 wLength, u16 wValueLength, u16 wType} + szKey(UTF-16,null)
//   + pad→4 + Value + pad→4 + Children…；wLength = 本节点逻辑字节（含子块，但**不含尾随对齐 pad**，对齐 rc.exe）。
//   wType：0=binary，1=text（值以 UTF-16 存）；text 的 wValueLength 计 WCHAR 数（含 null），
//   binary 的 wValueLength 计字节数。
// 顶层 Value = VS_FIXEDFILEINFO（13×u32=52B）；子树 StringFileInfo→StringTable→VALUE(文本)，
//   VarFileInfo→Translation(每入口 = u16 lang + u16 charset)。
// ============================================================

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "colang/common.h"
#include "colang/winres.h"
#include "rc_util.h"

namespace colang
{

namespace
{

using rcutil::pad4;
using rcutil::put_u16;
using rcutil::put_u32;
using rcutil::utf8_to_u16;

// 写节点头（wLength 占位，末尾由 patch_len 回填），返回节点起始偏移。
std::size_t write_header(std::vector<uint8_t> &out, uint16_t value_len, uint16_t type)
{
	std::size_t start = out.size();
	put_u16(out, 0); // wLength 占位
	put_u16(out, value_len);
	put_u16(out, type);
	return start;
}

// 写 szKey（UTF-16 + null）并 pad→4。
void write_key(std::vector<uint8_t> &out, const std::string &key)
{
	for (char16_t c : utf8_to_u16(key))
		put_u16(out, static_cast<uint16_t>(c));
	put_u16(out, 0);
	pad4(out);
}

// 结束节点：回填 wLength 为**逻辑长度**（从节点起始到当前末尾、不含尾随对齐 pad）。
//   不在此补 pad：兄弟间的 4 对齐 pad 由下一个兄弟「前置」pad4 发出（位置与旧写法相同，
//   物理字节流不变），从而不会被计入本节点/父节点的 wLength——对齐 rc.exe。
void patch_len(std::vector<uint8_t> &out, std::size_t start)
{
	const uint16_t len = static_cast<uint16_t>(out.size() - start); // 逻辑长（不含尾随 pad）
	out[start] = static_cast<uint8_t>(len & 0xFF);
	out[start + 1] = static_cast<uint8_t>((len >> 8) & 0xFF);
}

// 从 8 位十六进制串 "040904b0" 解析 lang/charset。
void parse_lang_charset(const std::string &s, uint16_t &lang, uint16_t &charset)
{
	lang = 0x0409;
	charset = 0x04b0;
	if (s.size() >= 8)
	{
		lang = static_cast<uint16_t>(std::strtoul(s.substr(0, 4).c_str(), nullptr, 16));
		charset = static_cast<uint16_t>(std::strtoul(s.substr(4, 4).c_str(), nullptr, 16));
	}
}

}  // namespace

bool rc_build_version_info(const VersionInfoInput &vi, std::vector<uint8_t> &out,
                           std::vector<Error> & /*errors*/)
{
	out.clear();

	const std::size_t root = write_header(out, 52, 0); // VS_FIXEDFILEINFO 二进制
	write_key(out, vi.key.empty() ? std::string("VS_VERSION_INFO") : vi.key);

	// —— VS_FIXEDFILEINFO（13×u32 = 52 字节）——
	const auto mkword = [](uint16_t hi, uint16_t lo) -> uint32_t {
		return (static_cast<uint32_t>(hi) << 16) | static_cast<uint32_t>(lo);
	};
	put_u32(out, 0xFEEF04BD); // dwSignature
	put_u32(out, 0x00010000); // dwStrucVersion
	put_u32(out, mkword(vi.fileversion[0], vi.fileversion[1])); // dwFileVersionMS
	put_u32(out, mkword(vi.fileversion[2], vi.fileversion[3])); // dwFileVersionLS
	put_u32(out, mkword(vi.productversion[0], vi.productversion[1])); // dwProductVersionMS
	put_u32(out, mkword(vi.productversion[2], vi.productversion[3])); // dwProductVersionLS
	put_u32(out, vi.fileflagsmask); // dwFileFlagsMask
	put_u32(out, vi.fileflags);     // dwFileFlags
	put_u32(out, vi.fileos);          // dwFileOS（对齐 rc.exe：省略 FILEOS 默认 0）
	put_u32(out, vi.filetype);      // dwFileType
	put_u32(out, vi.filesubtype);   // dwFileSubtype
	put_u32(out, vi.datedateMS);    // dwFileDateMS
	put_u32(out, vi.datedateLS);    // dwFileDateLS
	pad4(out);

	// —— StringFileInfo（仅当有字符串项）——
	if (!vi.strings.empty())
	{
		const std::size_t sfi = write_header(out, 0, 1); // wType=1（rc.exe 实测容器节点）
		write_key(out, "StringFileInfo");

		const std::size_t st = write_header(out, 0, 1); // wType=1（rc.exe 实测容器节点）
		write_key(out, vi.lang_charset.empty() ? std::string("040904b0") : vi.lang_charset);

		for (const auto &kv : vi.strings)
		{
			pad4(out); // 前置对齐：将上一叶子的尾随 pad 排除在其 wLength 之外
			const std::u16string uv = utf8_to_u16(kv.second);
			// rc.exe 语义：源串末尾已为 0x0000（即显式写了 `\0`）时，视为自带 null 终结符，
			//   wValueLength 取 uv.size() 不再追加；否则补一个 null，wValueLength=uv.size()+1。
			//   （历史无脑 +1 使带尾 \0 的 VALUE 多一个 WCHAR null，与 rc.exe 不同步，已修。）
			const bool tail_null = !uv.empty() && uv.back() == 0;
			const uint16_t vlen = static_cast<uint16_t>(uv.size() + (tail_null ? 0u : 1u));
			const std::size_t vn = write_header(out, vlen, 1); // text
			write_key(out, kv.first);
			for (char16_t c : uv)
				put_u16(out, static_cast<uint16_t>(c));
			if (!tail_null)
				put_u16(out, 0); // 仅在源串未自带 null 时补终结符
			patch_len(out, vn); // 文本叶子 wLength 记逻辑长（不含尾随 pad）
		}
		patch_len(out, st);
		patch_len(out, sfi);
	}

	// —— VarFileInfo → Translation ——
	{
		pad4(out); // 前置对齐（与 StringFileInfo 为兄弟）
		const std::size_t vfi = write_header(out, 0, 1); // wType=1（rc.exe 实测容器节点）
		write_key(out, "VarFileInfo");

		std::vector<uint32_t> trans = vi.translation;
		if (trans.empty())
		{
			uint16_t lang, charset;
			parse_lang_charset(vi.lang_charset, lang, charset);
			trans.push_back(mkword(lang, charset));
		}

		const std::size_t tr = write_header(out, static_cast<uint16_t>(trans.size() * 4), 0);
		write_key(out, "Translation");
		for (uint32_t t : trans)
		{
			put_u16(out, static_cast<uint16_t>(t >> 16));      // lang
			put_u16(out, static_cast<uint16_t>(t & 0xFFFF));   // charset
		}
		patch_len(out, tr);
		patch_len(out, vfi);
	}

	patch_len(out, root);
	return true;
}

}  // namespace colang
