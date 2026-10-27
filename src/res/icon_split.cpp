// ============================================================
// icon_split.cpp — 单个 .ico（ICONDIR）→ N 个 RT_ICON + 1 个 RT_GROUP_ICON
//
// .ico 文件布局：
//   ICONDIR{u16 reserved=0, u16 type(1=icon), u16 count}
//   count × ICONDIRENTRY(16B){u8 w,u8 h,u8 colorCount,u8 reserved,
//            u16 planes,u16 bitCount,u32 bytesInRes,u32 imageOffset}
//   各 image blob 位于 imageOffset、长 bytesInRes（BMP/DIB 或 PNG）。
// RT_ICON 数据 = 该 image blob 逐字节原样（PNG-in-ICO 亦通吃，无需特判）。
// RT_GROUP_ICON 数据 = GRPICONDIR{u16 0,u16 1,u16 count} +
//   count × GRPICONDIRENTRY(14B)＝ICONDIRENTRY 去 imageOffset、末换 u16 nID；
//   其中 planes 恒规整为 1、bitCount：BMP/DIB 取内嵌 BITMAPINFOHEADER.biBitCount
//   （image 偏移 14），PNG 保留目录 bitCount（对齐 rc.exe，与 RT_ICON blob 无关）。
// RT_ICON 名从 next_icon_id 起逐个递增分配（计数器由调用方跨 ICON 语句持有，
//   避免多语句各自从 1 起撞名）；group 名 = .rc 里 ICON 语句的资源名，可为数字
//   序数或字符串（两态由 group_name_is_string 决定），子图标序数恒为数字。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

#include "colang/common.h"
#include "colang/winres.h"
#include "rc_util.h"

namespace colang
{

namespace
{
Error ico_err(const SourceLoc &loc, const std::string &msg)
{
	return Error{loc.file, Pos{loc.line, loc.column}, msg};
}
}  // namespace

bool rc_encode_icon(const std::string &ico_path, bool group_name_is_string,
                    uint16_t group_name_id, const std::u16string &group_name_str,
                    uint16_t lang, std::vector<ResItem> &out_rticon, ResItem &out_group,
                    std::vector<Error> &errors, const SourceLoc &loc, uint16_t &next_icon_id)
{
	std::vector<uint8_t> ico;
	if (!rcutil::read_file_bytes(ico_path, ico))
	{
		errors.push_back(ico_err(loc, "cannot open icon file: " + ico_path));
		return false;
	}
	if (ico.size() < 6)
	{
		errors.push_back(ico_err(loc, "icon file too small (no ICONDIR): " + ico_path));
		return false;
	}
	const uint16_t reserved = rcutil::rd_u16(ico, 0);
	const uint16_t type = rcutil::rd_u16(ico, 2);
	const uint16_t count = rcutil::rd_u16(ico, 4);
	if (reserved != 0 || type != 1)
	{
		errors.push_back(ico_err(loc, "not an ICO (ICONDIR reserved!=0 or type!=1): " + ico_path));
		return false;
	}
	if (count == 0)
	{
		errors.push_back(ico_err(loc, "icon has zero images: " + ico_path));
		return false;
	}
	if (ico.size() < static_cast<std::size_t>(6) + static_cast<std::size_t>(count) * 16)
	{
		errors.push_back(ico_err(loc, "icon directory truncated: " + ico_path));
		return false;
	}
	// 全局 id 空间不足（u16 序数将绕回）：及早报错，避免静默撞名。
	if (static_cast<uint32_t>(next_icon_id) + count - 1u > 0xFFFFu)
	{
		errors.push_back(ico_err(loc, "RT_ICON id space exhausted (u16 overflow): " + ico_path));
		return false;
	}

	// group 数据：GRPICONDIR 头
	std::vector<uint8_t> grp;
	rcutil::put_u16(grp, 0);   // idReserved
	rcutil::put_u16(grp, 1);   // idType = icon
	rcutil::put_u16(grp, count); // idCount

	for (uint16_t i = 0; i < count; ++i)
	{
		const std::size_t e = 6 + static_cast<std::size_t>(i) * 16; // ICONDIRENTRY 起点
		const uint8_t w = ico[e];
		const uint8_t h = ico[e + 1];
		const uint8_t cc = ico[e + 2];
		const uint8_t rev = ico[e + 3];
		const uint16_t bitcount = rcutil::rd_u16(ico, e + 6);
		const uint32_t bytesinres = rcutil::rd_u32(ico, e + 8);
		const uint32_t imgoff = rcutil::rd_u32(ico, e + 12);

		if (static_cast<std::size_t>(imgoff) + bytesinres > ico.size())
		{
			errors.push_back(ico_err(loc, "icon image data out of range: " + ico_path));
			return false;
		}

		// GRPICONDIRENTRY 的 planes/bitCount 规整（对齐 rc.exe，实测真值）：
		//   planes 恒写 1（覆盖源目录值）；bitCount：BMP/DIB 取内嵌 BITMAPINFOHEADER
		//   .biBitCount（image 偏移 14），PNG（magic 判别）或过短 DIB 保留目录 bitCount。
		//   注：仅影响 group 条目，RT_ICON blob 仍逐字节原样。
		const std::size_t off = static_cast<std::size_t>(imgoff);
		const bool is_png =
			bytesinres >= 8 &&
			ico[off] == 0x89 && ico[off + 1] == 0x50 && ico[off + 2] == 0x4E &&
			ico[off + 3] == 0x47 && ico[off + 4] == 0x0D && ico[off + 5] == 0x0A &&
			ico[off + 6] == 0x1A && ico[off + 7] == 0x0A;
		const uint16_t g_planes = 1;
		const uint16_t g_bitcount =
			(!is_png && bytesinres >= 16) ? rcutil::rd_u16(ico, off + 14) : bitcount;

		// RT_ICON：名 = 全局递增序数，数据 = image blob 原样
		const uint16_t icon_id = next_icon_id++; // 跨语句共享计数器
		ResItem ri;
		ri.type_is_string = false;
		ri.type_id = 3; // RT_ICON
		ri.name_is_string = false;
		ri.name_id = icon_id;
		ri.language = lang;
		ri.data.assign(ico.begin() + imgoff, ico.begin() + imgoff + bytesinres);
		ri.loc = loc;
		out_rticon.push_back(std::move(ri));

		// GRPICONDIRENTRY（14B）
		grp.push_back(w);
		grp.push_back(h);
		grp.push_back(cc);
		grp.push_back(rev);
		rcutil::put_u16(grp, g_planes);
		rcutil::put_u16(grp, g_bitcount);
		rcutil::put_u32(grp, bytesinres);
		rcutil::put_u16(grp, icon_id); // nID ↔ 上面 RT_ICON 名（同一全局序数）
	}

	out_group.type_is_string = false;
	out_group.type_id = 14; // RT_GROUP_ICON
	// group 名两态：串名（裸标识符→大写无引号 / 带引号串→原样，均由 parser 传入）
	// 或数字序数；RT_ICON 子序数不受影响（对齐 rc.exe）。
	out_group.name_is_string = group_name_is_string;
	if (group_name_is_string)
	{
		out_group.name_str = group_name_str;
		out_group.name_id = 0;
	}
	else
	{
		out_group.name_id = group_name_id;
		out_group.name_str.clear();
	}
	out_group.language = lang;
	out_group.data = std::move(grp);
	out_group.loc = loc;
	return true;
}

}  // namespace colang
