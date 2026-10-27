// ============================================================
// res_emit.cpp — ResItem[] → .res 字节（PE 资源档③ 发射器）
//
// 布局逐字节对齐 SDK rc.exe（黄金样本 _tmp/rcgold 坐实）与
//   llvm/include/llvm/Object/WindowsResource.h 的解析口径：
//   文件首 32 字节 null 头；每节点 =
//     Prefix{u32 DataSize, u32 HeaderSize}
//     Type  = 0xFFFF+u16 序数  或  null 结尾 UTF-16 串（无长度前缀）
//     Name  = 同上
//     pad-to-4（WIN_RES_HEADER_ALIGNMENT）
//     Suffix{u32 DataVersion, u16 MemoryFlags=0x0030, u16 Language,
//            u32 Version, u32 Characteristics}
//     data，再 pad-to-4（WIN_RES_DATA_ALIGNMENT）
//   HeaderSize = 8 + align4(len(Type)+len(Name)) + 16。
// lld 只用解析出的 Type/Name/Suffix/Data 重建 .rsrc，对 HeaderSize
//   仅校验 >= MIN_HEADER_SIZE(32)；此处仍精确写以与 rc.exe 逐字节一致。
// ============================================================

#include <cstdint>
#include <vector>

#include "colang/winres.h"
#include "rc_util.h"

namespace colang
{

namespace
{

using rcutil::pad4;
using rcutil::put_u16;
using rcutil::put_u32;

// 写一个 Type/Name 引用（序数或 null 结尾 UTF-16 串）。返回所写字节数。
std::size_t encode_ref(std::vector<uint8_t> &out, bool is_string, uint16_t id,
                       const std::u16string &str)
{
	if (!is_string)
	{
		put_u16(out, 0xFFFF); // 序数哨兵
		put_u16(out, id);
		return 4;
	}
	// 串：逐 UTF-16 码元 + 末尾 null 词（readWideString 扫到 0x0000 为止）。
	std::size_t n = 0;
	for (char16_t c : str)
	{
		put_u16(out, static_cast<uint16_t>(c));
		n += 2;
	}
	put_u16(out, 0x0000); // 终结 null
	n += 2;
	return n;
}

}  // namespace

void rc_emit_res(const std::vector<ResItem> &items, std::vector<uint8_t> &out_res)
{
	out_res.clear();

	// —— 文件首 32 字节 null 头（DataSize=0,HeaderSize=32,序数0/序数0,后缀全零）——
	put_u32(out_res, 0);        // DataSize
	put_u32(out_res, 32);       // HeaderSize
	put_u16(out_res, 0xFFFF);   // Type 哨兵
	put_u16(out_res, 0);        // Type 序数 0
	put_u16(out_res, 0xFFFF);   // Name 哨兵
	put_u16(out_res, 0);        // Name 序数 0
	for (int i = 0; i < 16; ++i) // Suffix 全零（含 MemoryFlags/Language）
		out_res.push_back(0);

	// —— 各资源节点 ——
	for (const ResItem &it : items)
	{
		// 先算 Type+Name 区的字节长度以求 HeaderSize（对齐前）。
		// Type/Name 串内容 = rc.exe 原样（含引号，由 parser 保证 token 拼写直存）。
		std::size_t type_len = it.type_is_string ? (it.type_str.size() * 2 + 2) : 4;
		std::size_t name_len = it.name_is_string ? (it.name_str.size() * 2 + 2) : 4;
		std::size_t tn = type_len + name_len;
		std::size_t tn_aligned = (tn + 3) & ~static_cast<std::size_t>(3);
		uint32_t header_size = static_cast<uint32_t>(8 + tn_aligned + 16);

		// Prefix
		put_u32(out_res, static_cast<uint32_t>(it.data.size())); // DataSize
		put_u32(out_res, header_size);

		// Type + Name
		encode_ref(out_res, it.type_is_string, it.type_id, it.type_str);
		encode_ref(out_res, it.name_is_string, it.name_id, it.name_str);

		// pad-to-4（相对节点起点；节点起点恒 4 对齐，故等价于全局对齐）
		pad4(out_res);

		// Suffix
		put_u32(out_res, 0);                 // DataVersion
		put_u16(out_res, 0x0030);            // MemoryFlags = WIN_RES_PURE_MOVEABLE
		put_u16(out_res, it.language);       // Language（默认 0x0409）
		put_u32(out_res, 0);                 // Version
		put_u32(out_res, 0);                 // Characteristics

		// Data
		for (uint8_t b : it.data)
			out_res.push_back(b);
		pad4(out_res);
	}
}

}  // namespace colang
