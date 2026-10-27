// ============================================================
// stringtable.cpp — STRINGTABLE/STRING 累积项 → RT_STRING（type 6）资源
//
// rc.exe 编码规律（真值探针 _tmp/stbl 坐实，并经 A/B 逐字节修正）：
//   · 分组：slot = id % 16（0..15，id 直接取模，非 id-1）；group(0-based) = id / 16，
//     资源名 = group + 1（1-based 序数）；type 恒 6。每 slot0 恒空（id×16 与 id0 均不存）。
//   · 每组恰存 16 槽（id×16 .. id×16+15）：组内空洞与尾随空槽均写 len=0；无任何字节 padding
//     （size 恒为 16 槽原始字节和，可为 4k+2，rc.exe 不做 4 对齐）。
//   · 槽编码：u16 字符数（UTF-16 码元个数）+ 逐码元 UTF-16，无 null 终止符。
//   · 合并：同一 language 的多个 STRINGTABLE 块（乃至跨语句）合并进同一 (language,group) 桶，
//     桶内同 id 以最后出现者为准；不同 language 各自独立成桶、互不合并。
//   · id 边界：id 须落在 1..0xFFFF（否则 group/name 序数无法表示），越界报错。
// ============================================================

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "colang/common.h"
#include "colang/winres.h"
#include "rc_util.h"

namespace colang
{

bool rc_encode_stringtable(const std::vector<StringEntry> &entries, std::vector<ResItem> &out,
                           std::vector<Error> &errors)
{
	bool ok = true;

	// 按 (language, group) 分桶；组内 slot(0..15) → UTF-16 文本。std::map 保证按 (lang,group) 升序发射。
	// 同槽后到覆盖先到（对齐 rc.exe 桶内同 id 取末值）。
	std::map<std::tuple<uint16_t, uint32_t>, std::vector<std::u16string>> buckets;

	for (const StringEntry &e : entries)
	{
		if (e.id < 1 || e.id > 0xFFFF)
		{
			errors.push_back(Error{e.loc.file, Pos{e.loc.line, e.loc.column},
			                       "STRING id out of range (must be 1..65535)"});
			ok = false;
			continue;
		}
		const uint32_t group = e.id / 16;   // 0-based 组号；name = group + 1（id×16 为组基）
		const uint32_t slot = e.id % 16;   // 组内 0-based 槽（id 直接取模）
		std::vector<std::u16string> &slots = buckets[std::make_tuple(e.language, group)];
		if (slots.empty())
			slots.resize(16);   // 恒 16 槽，缺省皆空串（len=0）
		slots[slot] = e.text;
	}

	if (!ok)
		return false;

	for (auto &kv : buckets)
	{
		const uint16_t lang = std::get<0>(kv.first);
		const uint32_t group = std::get<1>(kv.first);
		const std::vector<std::u16string> &slots = kv.second;

		ResItem it;
		it.type_is_string = false;
		it.type_id = 6;                 // RT_STRING
		it.name_is_string = false;
		it.name_id = static_cast<uint16_t>(group + 1);   // 1-based 序数名
		it.language = lang;
		it.loc = entries.empty() ? SourceLoc{} : entries.front().loc;

		std::vector<uint8_t> data;
		// 恰存 16 槽（空洞/尾随均 len=0），无字节 padding，与 rc.exe 逐字节一致。
		for (int s = 0; s < 16; ++s)
		{
			const std::u16string &str = slots[s];
			rcutil::put_u16(data, static_cast<uint16_t>(str.size()));   // 字符数（码元个数）
			for (char16_t c : str)
				rcutil::put_u16(data, static_cast<uint16_t>(c));        // 逐码元，无 null 终止
		}
		it.data = std::move(data);
		out.push_back(std::move(it));
	}

	return true;
}

}  // namespace colang
