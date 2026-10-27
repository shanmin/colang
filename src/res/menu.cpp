// ============================================================
// menu.cpp — MENU 块 → MENUTEMPLATE（RT_MENU=4）资源数据
//
// 真值探针（_tmp/menu_matrix.rc → rc.exe /fo menu_matrix.res → hexdump 逐 WORD 解码）坐实：
//   · 经典 MENUTEMPLATE（非 MENUEXTEMPLATE），RT_MENU 类型 ID = 4。
//   · 头 4 字节：WORD idMenu(=0) + WORD dwHelpId(=0)。
//   · POPUP 项：WORD mfOption(MF_POPUP=0x10 | style | 0x80_if_last_at_level) + WCHAR[] text\0。
//     无 mfID 字段。
//   · MENUITEM 项：WORD mfOption(style | 0x80_if_last_at_level) + WORD mfID + WCHAR[] text\0。
//   · SEPARATOR 项：WORD mfOption(0 | 0x80_if_last_at_level) + WORD mfID(=0) + WORD 0。
//     注：rc.exe 并不设 MF_SEPARATOR(0x800) 位，以 id=0 text="" 编码。
//   · 0x80 标志 = 当前级别最后一项（隐式 POPUPEND 替代，编码由 level 序列推算）。
//   · 字符串为 UTF-16LE + null WORD，无长度前缀、紧接下一字段。
//   · 无数组间 padding、无对齐要求（整个 data blob 紧排）。
// ============================================================

#include <cstdint>
#include <vector>

#include "colang/common.h"
#include "colang/winres.h"
#include "rc_util.h"

namespace colang
{

// 判断 items[idx] 是否为其所在 level 的最后一项。
// 算法：从 idx+1 向后扫描，跳过 level > items[idx].level 的后代项，
//   遇到首个 level ≤ 当前项的项 j：
//     - level[j] < level[idx] → 父级恢复，当前为 last → true
//     - level[j] == level[idx] → 同级兄弟存在 → false
//   若扫描到末尾无命中（含仅后代的情况）→ true。
static bool is_last_at_level(const std::vector<MenuItemInput> &items, std::size_t idx)
{
	const int L = items[idx].level;
	for (std::size_t j = idx + 1; j < items.size(); ++j)
	{
		if (items[j].level <= L)
			return items[j].level < L;
		// items[j].level > L → 后代（属于此 POPUP 子树或其他嵌套），跳过
	}
	return true; // 无后续同级/上级项 → 此即 last
}

bool rc_encode_menu(const MenuInput &menu, ResItem &out, std::vector<Error> &errors)
{
	(void)errors; // 合法性（文本/id 范围）在 parser 已校验，此处无新增诊断

	out.type_is_string = false;
	out.type_id = 4; // RT_MENU

	std::vector<uint8_t> data;

	// 头：WORD idMenu(=0) + WORD dwHelpId(=0)
	rcutil::put_u16(data, 0); // idMenu
	rcutil::put_u16(data, 0); // dwHelpId

	const std::size_t n = menu.items.size();
	for (std::size_t i = 0; i < n; ++i)
	{
		const MenuItemInput &mi = menu.items[i];
		const bool last = is_last_at_level(menu.items, i);
		uint16_t mfOpt = mi.flags;

		if (mi.is_popup)
		{
			mfOpt |= 0x0010; // MF_POPUP
			if (last)
				mfOpt |= 0x0080;
			rcutil::put_u16(data, mfOpt);
			// POPUP 无 mfID，直接写文本
			for (const char16_t ch : mi.text)
				rcutil::put_u16(data, static_cast<uint16_t>(ch));
			rcutil::put_u16(data, 0); // null terminator
		}
		else if (mi.is_separator)
		{
			if (last)
				mfOpt |= 0x0080;
			rcutil::put_u16(data, mfOpt); // flags 不含 MF_SEPARATOR（rc.exe 实测）
			rcutil::put_u16(data, 0);     // mfID = 0
			rcutil::put_u16(data, 0);     // 空串（仅 null WORD）
		}
		else // 普通 MENUITEM
		{
			if (last)
				mfOpt |= 0x0080;
			rcutil::put_u16(data, mfOpt);
			rcutil::put_u16(data, mi.id);
			for (const char16_t ch : mi.text)
				rcutil::put_u16(data, static_cast<uint16_t>(ch));
			rcutil::put_u16(data, 0); // null terminator
		}
	}

	out.data = std::move(data);
	return true;
}

} // namespace colang
