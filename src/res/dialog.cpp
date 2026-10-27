// ============================================================
// dialog.cpp — DIALOGEX（DLGTEMPLATEEX / DLGITEMTEMPLATEEX）→ RT_DIALOG（type 5）资源数据
//
// 编码规律（真值探针 _tmp/dlg dlg4~dlg8 逐字节坐实，A/B 校准）：
//   · dialog 定长头（26 字节）：wVersion=1 / wSignature=0xFFFF / helpID(DWORD) /
//     dwExtendedStyle(DWORD) / style(DWORD) / cdit(WORD) / x,y,cx,cy(SHORT×4)。
//     style 的取值（默认 0x80880000 / WS_CAPTION / DS_SETFONT 并入）全在 parser，此处纯装配。
//   · 头后依序三 version-0 尾随字段：menu / class / title。
//   · has_font 时其后写字体块：pointSize(WORD) weight(WORD) italic(BYTE) charset(BYTE)
//     typeface(WCHAR[] null 结尾)。
//   · 每条控件起始补至 4 字节（DWORD）对齐；定长 22 字节 = helpID / dwExtendedStyle / style
//     / x,y,cx,cy / id，其后 version-0：恒空 menu / class / text，再 cbItemExtra(WORD) + 创建数据。
//   version-0 三态：空→WORD 0x0000；原子/序数→WORD 0xFFFF + WORD 值；串→UTF-16 码元 + null 词。
// ============================================================

#include <cstdint>
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

// version-0 三态尾随字段编码：空 / 0xFFFF+序数 / null 结尾 UTF-16 串。
void emit_ref(std::vector<uint8_t> &data, const DialogRef &r)
{
	switch (r.kind)
	{
		case DialogRef::Ordinal:
			put_u16(data, 0xFFFF);
			put_u16(data, r.ordinal);
			break;
		case DialogRef::String:
			for (char16_t c : r.str)
				put_u16(data, static_cast<uint16_t>(c));
			put_u16(data, 0x0000); // 终结 null 词
			break;
		case DialogRef::Empty:
		default:
			put_u16(data, 0x0000); // 空槽
			break;
	}
}

}  // namespace

bool rc_encode_dialogex(const DialogexInput &dx, ResItem &out, std::vector<Error> &errors)
{
	(void)errors; // 合法性（样式/控件名/几何）均在 parser 判定，此处纯字节装配

	out.type_is_string = false;
	out.type_id = 5; // RT_DIALOG

	std::vector<uint8_t> data;

	// —— dialog 定长头 ——
	put_u16(data, 1);                    // wVersion
	put_u16(data, 0xFFFF);               // wSignature
	put_u32(data, dx.helpid);            // helpID
	put_u32(data, dx.exstyle);           // dwExtendedStyle
	put_u32(data, dx.style);             // style（已含默认/WS_CAPTION/DS_SETFONT）
	put_u16(data, static_cast<uint16_t>(dx.items.size())); // cdit
	put_u16(data, static_cast<uint16_t>(dx.x));
	put_u16(data, static_cast<uint16_t>(dx.y));
	put_u16(data, static_cast<uint16_t>(dx.cx));
	put_u16(data, static_cast<uint16_t>(dx.cy));

	// —— 尾随 version-0：menu / class / title ——
	emit_ref(data, dx.menu);
	emit_ref(data, dx.cls);
	emit_ref(data, dx.title);

	// —— 字体块（DS_SETFONT）——
	if (dx.has_font)
	{
		put_u16(data, dx.font_point);
		put_u16(data, dx.font_weight);
		data.push_back(dx.font_italic);  // BYTE
		data.push_back(dx.font_charset); // BYTE
		for (char16_t c : dx.font_face)
			put_u16(data, static_cast<uint16_t>(c));
		put_u16(data, 0x0000); // typeface 终结 null 词
	}

	// —— 控件条目：每条起始补至 4 字节（DWORD）对齐 ——
	for (const DialogItemInput &it : dx.items)
	{
		pad4(data);
		put_u32(data, it.helpid);        // helpID（探针：位置参 9，缺省 0）
		put_u32(data, it.exstyle);       // dwExtendedStyle
		put_u32(data, it.style);         // style
		put_u16(data, static_cast<uint16_t>(it.x));
		put_u16(data, static_cast<uint16_t>(it.y));
		put_u16(data, static_cast<uint16_t>(it.cx));
		put_u16(data, static_cast<uint16_t>(it.cy));
		put_u16(data, it.id);
		emit_ref(data, it.menu);         // 恒空（0x0000）
		emit_ref(data, it.cls);
		emit_ref(data, it.text);
		put_u16(data, static_cast<uint16_t>(it.extra.size())); // cbItemExtra
		for (uint8_t b : it.extra)
			data.push_back(b);
	}

	out.data = std::move(data);
	return true;
}

}  // namespace colang
