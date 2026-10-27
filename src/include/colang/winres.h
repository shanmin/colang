// ============================================================
// winres.h — 内置 RC 资源编译器（PE 资源档③）对外接口
//
// 职责：把 .rc 脚本编译为 .res 字节（资源数据库 flat 节点序列），
//   .res → .rsrc 的转换交 lld（档②已实测、argv 已吃 .res）。
// 仅 Windows 有意义（ELF 无 PE 资源概念）；由 main.cpp 在 --resource
//   传入 .rc 时调用，产出临时 .res 塞入 opts.res_files。
//
// .res 节点布局对齐 llvm/include/llvm/Object/WindowsResource.h：
//   WinResHeaderPrefix{u32 DataSize,u32 HeaderSize} + Type/Name（各
//   「0xFFFF+u16 序数」或「UTF-16 串扫到 null」）+ WinResHeaderSuffix
//   {u32 DataVersion,u16 MemoryFlags,u16 Language,u32 Version,u32
//   Characteristics} + data；头/数据各按 4 对齐。文件以 32 字节 null
//   头起始（与 rc.exe 逐字节一致，黄金样本已坐实）。
// ============================================================
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "colang/common.h"   // Error, Pos, SourceLoc

namespace colang
{

struct CompileOptions;   // 前置声明（driver.h），仅取 include_dirs/preload 等

// —— 单个资源项：Type/Name 各「序数或串」二态；data 为最终字节 ——
struct ResItem
{
	bool type_is_string = false;
	uint16_t type_id = 0;
	std::u16string type_str;
	bool name_is_string = false;
	uint16_t name_id = 0;
	std::u16string name_str;
	uint16_t language = 0x0409;   // 默认 en-US（RC 惯例）
	std::vector<uint8_t> data;
	SourceLoc loc;                 // 诊断回溯（.rc 出处）
};

// —— VERSIONINFO 块的解析中间描述（parser 填、version_info 编码消费）——
struct VersionInfoInput
{
	bool has_fileversion = false;
	uint16_t fileversion[4] = {0, 0, 0, 0};    // MS(maj,min) LS(build,qr)
	bool has_productversion = false;
	uint16_t productversion[4] = {0, 0, 0, 0};
	uint32_t fileflagsmask = 0;   // rc.exe 实测：.rc 省略 FILEFLAGSMASK 时默认 0（指定则覆盖）
	uint32_t fileflags = 0;
	uint32_t fileos = 0;          // rc.exe 实测：.rc 省略 FILEOS 时默认 0（指定则覆盖，非硬编 VOS_NT_WINDOWS32）
	uint32_t filetype = 0;         // 0=UNKNOWN,1=APP,2=DLL,3=DRV,4=VXD,5=FONT,7=STATICLIB
	uint32_t filesubtype = 0;
	uint32_t datedateMS = 0, datedateLS = 0;
	std::string key = "VS_VERSION_INFO";
	std::string lang_charset = "040904b0";      // StringTable 键
	std::vector<std::pair<std::string, std::string>> strings;   // name -> value
	std::vector<uint32_t> translation;          // (lang<<16|charset) 序列
	SourceLoc loc;
};

// 内置资源关键字（大写）→ RT_* 序数；非内置返回 0。
//   CURSOR=1 BITMAP=2 ICON=3 MENU=4 DIALOG=5 STRINGTABLE=6 FONTDIR=7 FONT=8
//   ACCELERATOR=9 RCDATA=10 MESSAGETABLE=11 GROUP_CURSOR=12 GROUP_ICON=14
//   VERSION=16 DLGINCLUDE=17 PLUGPLAY=19 VXD=20 ANICURSOR=21 ANIICON=22 HTML=23
//   MANIFEST=24。
uint16_t rc_builtin_type_id(const std::string &upper);

// 主入口：读 rc_path 指向的 .rc，预处理→解析→编码，产出 .res 字节到 out_res。
// 成功返回 true；失败追加诊断到 errors 并返回 false。base 提供 include_dirs。
bool rc_compile(const std::string &rc_path, const CompileOptions &base,
                std::vector<uint8_t> &out_res, std::vector<Error> &errors);

// —— 子件（供 rc_compile 组合；亦利单测）——

// rc_parser：预处理 .rc 得 token 流并解析为资源项序列（含图标拆分/位图裁剪/版本编码）。
bool rc_collect_items(const std::string &rc_path, const CompileOptions &base,
                      std::vector<ResItem> &out, std::vector<Error> &errors);

// res_emit：资源项序列 → .res 字节（32 字节 null 头 + 各节点）。
void rc_emit_res(const std::vector<ResItem> &items, std::vector<uint8_t> &out_res);

// icon_split：单个 .ico（ICONDIR）拆成 N 个 RT_ICON（type 3，名=全局递增序数）+
//   生成 RT_GROUP_ICON（type 14，名=ICON 语句的资源名，两态：group_name_is_string
//   决定用 group_name_str（串名）还是 group_name_id（数字名））。RT_ICON 名从
//   next_icon_id 起分配并递增回写，由调用方跨 ICON 语句持有同一计数器，杜绝多
//   语句各自从 1 起撞名；子图标序数恒为数字（与 group 名是否为串无关，对齐 rc.exe）。
bool rc_encode_icon(const std::string &ico_path, bool group_name_is_string,
                    uint16_t group_name_id, const std::u16string &group_name_str,
                    uint16_t lang, std::vector<ResItem> &out_rticon, ResItem &out_group,
                    std::vector<Error> &errors, const SourceLoc &loc, uint16_t &next_icon_id);

// bitmap_strip：.bmp 去 14 字节 BITMAPFILEHEADER 得 DIB 作 RT_BITMAP 数据。
bool rc_strip_bitmap(const std::string &bmp_path, std::vector<uint8_t> &dib,
                     std::vector<Error> &errors, const SourceLoc &loc);

// version_info：VersionInfoInput → VS_VERSION_INFO 资源数据字节。
bool rc_build_version_info(const VersionInfoInput &vi, std::vector<uint8_t> &out,
                           std::vector<Error> &errors);

// —— ACCELERATORS 块的单条加速键（parser 逐条填、accelerators 编码消费）——
//   真值坐实（rc.exe）：每元素 8 字节紧排 = fVirt(u8) + 0(u8) + key(u16 LE) +
//   cmd(u16 LE) + 0(u16)，无数组间填充、无终止哨兵项；块内最后一条的 fVirt 额外置
//   bit7(0x80) 作结束标记（rc.exe 实测，非标准 ACCEL 语义，A/B 逐字节对齐所需）。
//   fVirt 低 5 位＝VIRTKEY=0x01 / NOINVERT=0x02 / SHIFT=0x04 / CONTROL=0x08 / ALT=0x10。
//   key：带引号单字符＝其码元值（大小写保留）；裸 VK_*＝查表得虚拟键码（须伴随 VIRTKEY）。
//   cmd：宏展开后的数值（WORD）。
struct AccelEntry
{
	uint8_t fVirt = 0;      // 选项位累计（位或）；编码时末条再 |= 0x80
	uint16_t key = 0;       // 键值（字符码或 VK 码）
	uint16_t cmd = 0;       // 命令 id
	SourceLoc loc;          // .rc 出处（诊断回溯）
};

// accelerators：AccelEntry 序列 → ACCELERATOR（type 9）资源数据字节（8 字节/条，末条 fVirt|=0x80）。
//   填充传入 out 的 type（9）与 data；out 的 name/language/loc 由调用方预置。空块产 0 字节 data。
bool rc_encode_accelerators(const std::vector<AccelEntry> &accels, ResItem &out,
                            std::vector<Error> &errors);

// —— STRINGTABLE/STRING 块的累积项（parser 跨块收集、stringtable 全局编码消费）——
//   真值坐实（rc.exe）：同语言的多个 STRINGTABLE 块合并进同一 (language,group) 表，
//   slot=id%16、group=id/16+1（1-based 序数名）；每组恰存 16 槽（空洞/尾随均 len=0，无字节 padding）；
//   不同语言各自成组。
//   故必须跨语句按 language 累积、末尾统一分桶编码，不能逐块独立发射。
struct StringEntry
{
	uint16_t language = 0x0409;         // 语句解析时的当前 LANGUAGE（MAKELANGID）
	uint32_t id = 0;                     // 字符串 id（须 1..0xFFFF）
	std::u16string text;                 // 已解码的 UTF-16 串内容
	SourceLoc loc;                        // .rc 出处（诊断回溯）
};

// stringtable：累积的字符串项 → 按 (language, group) 分桶的 RT_STRING（type 6）ResItem 序列。
//   每桶产一个 ResItem（名=group 1-based 序数，data=16 槽：u16 字符数 + UTF-16 码元，无 null 终止；
//   组内空洞与尾随槽写 len=0）。同桶内同 id 以最后出现者为准（对齐 rc.exe）。越界 id 报错返回 false。
bool rc_encode_stringtable(const std::vector<StringEntry> &entries, std::vector<ResItem> &out,
                           std::vector<Error> &errors);

// —— DIALOGEX（DLGTEMPLATEEX / DLGITEMTEMPLATEEX）——
//   真值探针（_tmp/dlg dlg4~dlg8）逐字节坐实 rc.exe 编码：
//   · dialog 定长头 26 字节 = WORD wVersion(=1) + WORD wSignature(=0xFFFF) + DWORD helpID
//     + DWORD dwExtendedStyle + DWORD style + WORD cdit + SHORT x,y,cx,cy。
//     style：有 STYLE 子句则取其值，否则默认 0x80880000(WS_POPUP|WS_BORDER|WS_GROUP)；
//     有 CAPTION 额外并 WS_CAPTION(0x00C00000)；有 FONT 额外并 DS_SETFONT(0x00000040)。
//   · 头后依序三「version-0 三态」尾随字段：menu / class / title（皆缺省则各写 0x0000）。
//   · DS_SETFONT 时其后写：WORD pointSize; WORD weight; BYTE italic; BYTE charset(默认 1=DEFAULT_CHARSET);
//     WCHAR typeface[]（null 结尾）。FONT 参序＝pt, typeface[, weight[, italic[, charset]]]。
//   · 每条控件 DLGITEMTEMPLATEEX 起始补至 4 字节（DWORD）对齐；定长部分 22 字节 =
//     DWORD helpID + DWORD dwExtendedStyle + DWORD style + SHORT x,y,cx,cy + WORD id，
//     其后 version-0 三态：恒空 menu(0x0000) / class / text，再 WORD cbItemExtra + 可选创建数据。
//     控件 style = 关键字内置默认 | 位置参给定样式（纯按位或）；CONTROL 默认基 = WS_CHILD|WS_VISIBLE。
//   version-0 三态：空→WORD 0x0000；序数/原子→WORD 0xFFFF + WORD 值；串→逐 UTF-16 码元 + null 词（无长度前缀）。
struct DialogRef
{
	enum Kind { Empty, Ordinal, String } kind = Empty;
	uint16_t ordinal = 0;        // Ordinal（原子/序数）
	std::u16string str;          // String（UTF-16 内容，编码时补 null 词）
};

struct DialogItemInput
{
	uint32_t helpid = 0;
	uint32_t exstyle = 0;
	uint32_t style = 0;          // 最终样式（parser 已并入关键字默认）
	int16_t x = 0, y = 0, cx = 0, cy = 0;
	uint16_t id = 0;
	DialogRef menu;              // 恒空（保留槽，编码写 0x0000）
	DialogRef cls;               // 类：预定义原子或自定义串
	DialogRef text;              // 文本：串或空（EDITTEXT/LISTBOX/COMBOBOX/SCROLLBAR 无文本→空）
	std::vector<uint8_t> extra;  // 创建数据（cbItemExtra；一般空）
	SourceLoc loc;
};

struct DialogexInput
{
	uint32_t helpid = 0;
	uint32_t exstyle = 0;
	uint32_t style = 0;          // 最终样式（已含默认/WS_CAPTION/DS_SETFONT）
	int16_t x = 0, y = 0, cx = 0, cy = 0;
	DialogRef menu;
	DialogRef cls;
	DialogRef title;
	bool has_font = false;
	uint16_t font_point = 0;
	uint16_t font_weight = 0;
	uint8_t font_italic = 0;
	uint8_t font_charset = 1;    // DEFAULT_CHARSET
	std::u16string font_face;
	std::vector<DialogItemInput> items;
	SourceLoc loc;
};

// dialog：DialogexInput → DLGTEMPLATEEX 资源数据字节（RT_DIALOG，type 5）。
//   填充传入 out 的 type（5）与 data；out 的 name/language/loc 由调用方预置。
bool rc_encode_dialogex(const DialogexInput &dx, ResItem &out, std::vector<Error> &errors);

// —— MENU（MENUTEMPLATE / RT_MENU=4）——
//   真值探针（_tmp/menu_matrix.rc → rc.exe → hexdump）逐字节坐实：
//   · 经典 MENUTEMPLATE（非 MENUEXTEMPLATE），无数组对齐/padding，紧排 WORD 字段。
//   · 头 4 字节：WORD idMenu(=0) + WORD dwHelpId(=0)。
//   · POPUP 项：WORD mfOption(MF_POPUP=0x10 | style_bits | 0x80_if_last) + WCHAR[] text\0（无 mfID）。
//   · MENUITEM 项：WORD mfOption(style_bits | 0x80_if_last) + WORD mfID + WCHAR[] text\0。
//   · SEPARATOR 项：WORD mfOption(0 | 0x80_if_last) + WORD mfID(=0) + WORD 0（空串）。
//   · 0x80 标志：当前级别最后一项（隐式 POPUPEND 替代，编码时由 level 序列推算）。
struct MenuItemInput
{
	uint16_t flags = 0;          // 样式位（GRAYED=1, DISABLED=2, CHECKED=8…），不含 MF_POPUP/0x80
	uint16_t id = 0;             // 命令 ID（POPUP/SEPARATOR 为 0）
	int      level = 0;          // 嵌套深度（0=顶级 POPUP、1=子项、2=孙项…）
	bool     is_popup = false;   // POPUP 条目标记
	bool     is_separator = false; // MENUITEM SEPARATOR 标记
	std::u16string text;         // 菜单文本（SEPARATOR 时空）
	SourceLoc loc;
};

struct MenuInput
{
	std::vector<MenuItemInput> items;  // 展平序列（level 编码嵌套深度）
	SourceLoc loc;
};

// menu：MenuInput → MENUTEMPLATE 资源数据字节（RT_MENU，type 4）。
//   填充传入 out 的 type（4）与 data；out 的 name/language/loc 由调用方预置。
bool rc_encode_menu(const MenuInput &menu, ResItem &out, std::vector<Error> &errors);

}  // namespace colang
