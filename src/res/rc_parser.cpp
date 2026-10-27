// ============================================================
// rc_parser.cpp — 预处理后的 token 流 → ResItem[]（PE 资源档③）
//
// 承接 rc_preprocess 产出的扁平 token（符号 ID 已被宏展开为数字）。
// 语句形制：NAME TYPE [attrs] [LANGUAGE n[,m]] DATA
//   NAME  = 数字（序数 id）或 "串"（串 id）
//   TYPE  = 关键字（ICON/MANIFEST/RCDATA/VERSIONINFO/…→内置序数）
//           或 数字（自定义序数）或 "串"（自定义串型，原样含引号存）
//   DATA  = "文件"（外部文件）或 { 字节… }（内联）
// 关键字→RT_* 序数见 rc_builtin_type_id。ICON 触发拆分为 RT_ICON[]+RT_GROUP_ICON；
//   BITMAP 触发去 BITMAPFILEHEADER；VERSIONINFO 触发 VS_VERSION_INFO 编码；
//   STRINGTABLE/STRING 跨语句按语言累积后触发 RT_STRING 分桶编码。
// 尚不含其余脚本型资源（DIALOG/MENU/ACCELERATORS/CURSOR/GROUP_ICON 手块）
//   —— 遇到给明确「不支持」诊断，不静默误产。
// ============================================================

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "colang/common.h"
#include "colang/driver.h"
#include "colang/lexer.h"
#include "colang/winres.h"
#include "rc_util.h"

namespace colang
{

// rc_preprocess 内部入口（定义于 rc_preprocess.cpp）。
bool rc_preprocess(const std::string &rc_path, const CompileOptions &base,
                   std::vector<Token> &out, std::vector<Error> &errors);

uint16_t rc_builtin_type_id(const std::string &upper)
{
	if (upper == "CURSOR") return 1;
	if (upper == "BITMAP") return 2;
	if (upper == "ICON") return 3;
	if (upper == "MENU") return 4;
	if (upper == "DIALOG" || upper == "DIALOGEX") return 5;
	if (upper == "STRING" || upper == "STRINGTABLE") return 6;
	if (upper == "FONTDIR") return 7;
	if (upper == "FONT") return 8;
	if (upper == "ACCELERATOR" || upper == "ACCELERATORS") return 9;
	if (upper == "RCDATA") return 10;
	if (upper == "MESSAGETABLE") return 11;
	if (upper == "GROUP_CURSOR") return 12;
	if (upper == "GROUP_ICON") return 14;
	if (upper == "VERSION" || upper == "VERSIONINFO") return 16;
	if (upper == "DLGINCLUDE") return 17;
	if (upper == "PLUGPLAY") return 19;
	if (upper == "VXD") return 20;
	if (upper == "ANICURSOR") return 21;
	if (upper == "ANIICON") return 22;
	if (upper == "HTML") return 23;
	if (upper == "MANIFEST") return 24;
	return 0;
}

namespace
{

// —— 通用小工具 ——
std::string to_upper(const std::string &s)
{
	std::string o = s;
	for (char &c : o)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return o;
}

bool is_num(const Token &t) { return t.kind == TokKind::Number; }
bool is_str(const Token &t) { return t.kind == TokKind::String; }
bool is_id(const Token &t) { return t.kind == TokKind::Identifier || t.kind == TokKind::Keyword; }
bool is_punct(const Token &t, const char *p) { return t.kind == TokKind::Punct && t.text == p; }

uint64_t parse_num(const Token &t)
{
	return std::strtoull(t.text.c_str(), nullptr, 0);
}

// v1 不支持的关键字（脚本型/手块）。
bool is_unsupported_type(const std::string &up)
{
	static const char *k[] = {"CURSOR", "DIALOG",
		                      "GROUP_CURSOR", "GROUP_ICON",
		                      "FONT", "FONTDIR", "MESSAGETABLE", "HTML", "ANICURSOR", "ANIICON"};
	for (const char *s : k)
		if (up == s)
			return true;
	return false;
}

// 存储属性关键字（type 与 data 之间可出现，忽略）。
bool is_attr(const std::string &up)
{
	static const char *k[] = {"MOVEABLE", "DISCARDABLE", "PRELOAD", "LOADONCALL", "FIXED",
		                      "PURE", "LOADED", "STUB", "INACTIVE", "VIRTUALLINK", "READAHEAD"};
	for (const char *s : k)
		if (up == s)
			return true;
	return false;
}

// —— ACCELERATORS 内置符号表——
//   rc_preprocess 把 `#include <windows.h>` 整行空白化，故 VK_*/选项不会宏展开、
//   留存为标识符，需 colang 自建表对照 rc.exe（真值探针 _tmp/accel 坐实位值）。
// 选项名 → fVirt 位：VIRTKEY=1 NOINVERT=2 SHIFT=4 CONTROL=8 ALT=16；ASCII=0（默认无位）。
bool accel_option_value(const std::string &up, uint8_t &bits)
{
	if (up == "VIRTKEY") { bits = 0x01; return true; }
	if (up == "NOINVERT") { bits = 0x02; return true; }
	if (up == "SHIFT") { bits = 0x04; return true; }
	if (up == "CONTROL") { bits = 0x08; return true; }
	if (up == "ALT") { bits = 0x10; return true; }
	if (up == "ASCII") { bits = 0x00; return true; }
	return false;
}

// VK_* 键名 → 虚拟键码（标准 winuser.h 宏集）。注意：winuser.h 并不把 VK_A..VK_Z /
//   VK_0..VK_9 定义为宏（注释“同 ASCII”），rc.exe 亦报其为 undefined keyword，故不列。
bool accel_vk_value(const std::string &up, uint16_t &code)
{
	static const struct { const char *n; uint16_t c; } k[] = {
		{"VK_LBUTTON", 0x01}, {"VK_RBUTTON", 0x02}, {"VK_CANCEL", 0x03}, {"VK_MBUTTON", 0x04},
		{"VK_XBUTTON1", 0x05}, {"VK_XBUTTON2", 0x06}, {"VK_BACK", 0x08}, {"VK_TAB", 0x09},
		{"VK_CLEAR", 0x0C}, {"VK_RETURN", 0x0D}, {"VK_SHIFT", 0x10}, {"VK_CONTROL", 0x11},
		{"VK_MENU", 0x12}, {"VK_PAUSE", 0x13}, {"VK_CAPITAL", 0x14}, {"VK_KANA", 0x15},
		{"VK_HANGEUL", 0x15}, {"VK_HANGUL", 0x15}, {"VK_IME_ON", 0x16}, {"VK_JUNJA", 0x17},
		{"VK_FINAL", 0x18}, {"VK_HANJA", 0x19}, {"VK_KANJI", 0x19}, {"VK_IME_OFF", 0x1A},
		{"VK_ESCAPE", 0x1B}, {"VK_CONVERT", 0x1C}, {"VK_NONCONVERT", 0x1D}, {"VK_ACCEPT", 0x1E},
		{"VK_MODECHANGE", 0x1F}, {"VK_SPACE", 0x20}, {"VK_PRIOR", 0x21}, {"VK_NEXT", 0x22},
		{"VK_END", 0x23}, {"VK_HOME", 0x24}, {"VK_LEFT", 0x25}, {"VK_UP", 0x26},
		{"VK_RIGHT", 0x27}, {"VK_DOWN", 0x28}, {"VK_SELECT", 0x29}, {"VK_PRINT", 0x2A},
		{"VK_EXECUTE", 0x2B}, {"VK_SNAPSHOT", 0x2C}, {"VK_INSERT", 0x2D}, {"VK_DELETE", 0x2E},
		{"VK_HELP", 0x2F}, {"VK_LWIN", 0x5B}, {"VK_RWIN", 0x5C}, {"VK_APPS", 0x5D},
		{"VK_SLEEP", 0x5F}, {"VK_NUMPAD0", 0x60}, {"VK_NUMPAD1", 0x61}, {"VK_NUMPAD2", 0x62},
		{"VK_NUMPAD3", 0x63}, {"VK_NUMPAD4", 0x64}, {"VK_NUMPAD5", 0x65}, {"VK_NUMPAD6", 0x66},
		{"VK_NUMPAD7", 0x67}, {"VK_NUMPAD8", 0x68}, {"VK_NUMPAD9", 0x69}, {"VK_MULTIPLY", 0x6A},
		{"VK_ADD", 0x6B}, {"VK_SEPARATOR", 0x6C}, {"VK_SUBTRACT", 0x6D}, {"VK_DECIMAL", 0x6E},
		{"VK_DIVIDE", 0x6F}, {"VK_F1", 0x70}, {"VK_F2", 0x71}, {"VK_F3", 0x72}, {"VK_F4", 0x73},
		{"VK_F5", 0x74}, {"VK_F6", 0x75}, {"VK_F7", 0x76}, {"VK_F8", 0x77}, {"VK_F9", 0x78},
		{"VK_F10", 0x79}, {"VK_F11", 0x7A}, {"VK_F12", 0x7B}, {"VK_F13", 0x7C}, {"VK_F14", 0x7D},
		{"VK_F15", 0x7E}, {"VK_F16", 0x7F}, {"VK_F17", 0x80}, {"VK_F18", 0x81}, {"VK_F19", 0x82},
		{"VK_F20", 0x83}, {"VK_F21", 0x84}, {"VK_F22", 0x85}, {"VK_F23", 0x86}, {"VK_F24", 0x87},
		{"VK_NUMLOCK", 0x90}, {"VK_SCROLL", 0x91}, {"VK_LSHIFT", 0xA0}, {"VK_RSHIFT", 0xA1},
		{"VK_LCONTROL", 0xA2}, {"VK_RCONTROL", 0xA3}, {"VK_LMENU", 0xA4}, {"VK_RMENU", 0xA5},
		{"VK_BROWSER_BACK", 0xA6}, {"VK_BROWSER_FORWARD", 0xA7}, {"VK_BROWSER_REFRESH", 0xA8},
		{"VK_BROWSER_STOP", 0xA9}, {"VK_BROWSER_SEARCH", 0xAA}, {"VK_BROWSER_FAVORITES", 0xAB},
		{"VK_BROWSER_HOME", 0xAC}, {"VK_VOLUME_MUTE", 0xAD}, {"VK_VOLUME_DOWN", 0xAE},
		{"VK_VOLUME_UP", 0xAF}, {"VK_MEDIA_NEXT_TRACK", 0xB0}, {"VK_MEDIA_PREV_TRACK", 0xB1},
		{"VK_MEDIA_STOP", 0xB2}, {"VK_MEDIA_PLAY_PAUSE", 0xB3}, {"VK_LAUNCH_MAIL", 0xB4},
		{"VK_LAUNCH_MEDIA_SELECT", 0xB5}, {"VK_LAUNCH_APP1", 0xB6}, {"VK_LAUNCH_APP2", 0xB7},
		{"VK_OEM_1", 0xBA}, {"VK_OEM_PLUS", 0xBB}, {"VK_OEM_COMMA", 0xBC}, {"VK_OEM_MINUS", 0xBD},
		{"VK_OEM_PERIOD", 0xBE}, {"VK_OEM_2", 0xBF}, {"VK_OEM_3", 0xC0},
	};
	for (const auto &e : k)
		if (up == e.n) { code = e.c; return true; }
	return false;
}

// —— DIALOGEX 内置样式位表（WS_/DS_/WS_EX_/BS_/ES_/SS_/LBS_/CBS_/SBS_/PB_ 常用集）——
//   同 accel_option/accel_vk：系统头被空白化，样式宏留存为标识符，需自建表对照 rc.exe。
//   表未列的罕见样式位后置（对齐 ACCEL「标准集优先、罕见后置」纪律）。
bool dialog_style_value(const std::string &up, uint32_t &val)
{
	static const struct { const char *n; uint32_t v; } k[] = {
		// WS_ 窗口样式
		{"WS_POPUP", 0x80000000u}, {"WS_CHILD", 0x40000000u}, {"WS_MINIMIZE", 0x20000000u},
		{"WS_VISIBLE", 0x10000000u}, {"WS_DISABLED", 0x08000000u}, {"WS_SYSMENU", 0x00080000u},
		{"WS_THICKFRAME", 0x00040000u}, {"WS_SIZEBOX", 0x00040000u}, {"WS_MINIMIZEBOX", 0x00020000u},
		{"WS_MAXIMIZEBOX", 0x00010000u}, {"WS_GROUP", 0x00020000u}, {"WS_TABSTOP", 0x00010000u},
		{"WS_CAPTION", 0x00C00000u}, {"WS_BORDER", 0x00800000u}, {"WS_DLGFRAME", 0x00400000u},
		{"WS_VSCROLL", 0x00200000u}, {"WS_HSCROLL", 0x00100000u}, {"WS_MAXIMIZE", 0x01000000u},
		{"WS_CLIPCHILDREN", 0x02000000u}, {"WS_EXSTYLE", 0x00000000u}, {"WS_OVERLAPPED", 0x00000000u},
		// DS_ 对话框样式
		{"DS_ABSALIGN", 0x00000001u}, {"DS_SYSMODAL", 0x00000002u}, {"DS_3DLOOK", 0x00000004u},
		{"DS_FIXEDSYS", 0x00000008u}, {"DS_NOFAILCREATE", 0x00000010u}, {"DS_CONTROL", 0x00000400u},
		{"DS_SETFONT", 0x00000040u}, {"DS_MODALFRAME", 0x00000080u}, {"DS_NOIDLEMSG", 0x00000100u},
		{"DS_SHELLFONT", 0x00000048u}, {"DS_CENTER", 0x00000800u}, {"DS_CENTERMOUSE", 0x00001000u},
		{"DS_CONTEXTHELP", 0x00002000u}, {"DS_LOCALEDIT", 0x00000020u},
		// WS_EX_ 扩展样式
		{"WS_EX_DLGMODALFRAME", 0x00000001u}, {"WS_EX_NOPARENTNOTIFY", 0x00000004u},
		{"WS_EX_TOPMOST", 0x00000008u}, {"WS_EX_ACCEPTFILES", 0x00000010u},
		{"WS_EX_TRANSPARENT", 0x00000020u}, {"WS_EX_MDICHILD", 0x00000040u},
		{"WS_EX_CLIENTEDGE", 0x00000200u}, {"WS_EX_CONTEXTHELP", 0x00000400u},
		{"WS_EX_RIGHT", 0x00001000u}, {"WS_EX_LEFT", 0x00000000u}, {"WS_EX_LTRREADING", 0x00000000u},
		{"WS_EX_STATICEDGE", 0x00020000u}, {"WS_EX_WINDOWEDGE", 0x00000100u},
		{"WS_EX_APPWINDOW", 0x00040000u}, {"WS_EX_LAYERED", 0x00080000u},
		// BS_ 按钮样式
		{"BS_PUSHBUTTON", 0x00000000u}, {"BS_DEFPUSHBUTTON", 0x00000001u}, {"BS_CHECKBOX", 0x00000002u},
		{"BS_AUTOCHECKBOX", 0x00000003u}, {"BS_RADIOBUTTON", 0x00000004u}, {"BS_3STATE", 0x00000005u},
		{"BS_GROUPBOX", 0x00000007u}, {"BS_AUTORADIOBUTTON", 0x00000009u}, {"BS_OWNERDRAW", 0x0000000Bu},
		{"BS_LEFTTEXT", 0x00000020u}, {"BS_LEFT", 0x00000100u}, {"BS_RIGHT", 0x00000200u},
		{"BS_CENTER", 0x00000300u}, {"BS_TOP", 0x00000400u}, {"BS_BOTTOM", 0x00000800u},
		{"BS_VCENTER", 0x00000C00u}, {"BS_PUSHLIKE", 0x00001000u}, {"BS_MULTILINE", 0x00002000u},
		// ES_ 编辑框样式
		{"ES_LEFT", 0x00000000u}, {"ES_CENTER", 0x00000001u}, {"ES_RIGHT", 0x00000002u},
		{"ES_MULTILINE", 0x00000004u}, {"ES_UPPERCASE", 0x00000008u}, {"ES_LOWERCASE", 0x00000010u},
		{"ES_PASSWORD", 0x00000020u}, {"ES_AUTOVSCROLL", 0x00000040u}, {"ES_AUTOHSCROLL", 0x00000080u},
		{"ES_READONLY", 0x00000800u}, {"ES_WANTRETURN", 0x00001000u}, {"ES_NUMBER", 0x00004000u},
		// SS_ 静态控件样式
		{"SS_LEFT", 0x00000000u}, {"SS_CENTER", 0x00000001u}, {"SS_RIGHT", 0x00000002u},
		{"SS_ICON", 0x00000003u}, {"SS_BLACKRECT", 0x00000004u}, {"SS_GRAYRECT", 0x00000005u},
		{"SS_WHITERECT", 0x00000006u}, {"SS_BLACKFRAME", 0x00000007u}, {"SS_GRAYFRAME", 0x00000008u},
		{"SS_WHITEFRAME", 0x00000009u}, {"SS_SIMPLE", 0x0000000Bu}, {"SS_LEFTNOWORDWRAP", 0x0000000Cu},
		{"SS_BITMAP", 0x0000000Eu}, {"SS_ENHMETAFILE", 0x0000000Fu}, {"SS_ETCHEDFRAME", 0x00000010u},
		{"SS_ETCHEDHORZ", 0x00000011u}, {"SS_ETCHEDVERT", 0x00000012u}, {"SS_ETCHEDFRAME2", 0x00000010u},
		// LBS_/CBS_/SBS_ 列表/组合/滚动条样式
		{"LBS_NOTIFY", 0x00000001u}, {"LBS_SORT", 0x00000002u}, {"LBS_NOREDRAW", 0x00000004u},
		{"LBS_MULTIPLESEL", 0x00000008u}, {"LBS_OWNERDRAWFIXED", 0x00000010u},
		{"LBS_HASSTRINGS", 0x00000040u}, {"LBS_USETABSTOPS", 0x00000080u},
		{"LBS_NOINTEGRALHEIGHT", 0x00000100u}, {"LBS_NOSCROLL", 0x00002000u},
		{"CBS_SIMPLE", 0x00000001u}, {"CBS_DROPDOWN", 0x00000002u}, {"CBS_DROPDOWNLIST", 0x00000003u},
		{"CBS_AUTOHSCROLL", 0x00000040u}, {"CBS_NOINTEGRALHEIGHT", 0x00000100u},
		{"CBS_HASSTRINGS", 0x00000200u}, {"CBS_OWNERDRAWFIXED", 0x00000400u},
		{"SBS_HORZ", 0x00000000u}, {"SBS_VERT", 0x00000001u}, {"SBS_TOPALIGN", 0x00000002u},
		{"SBS_LEFTALIGN", 0x00000002u}, {"SBS_BOTTOMALIGN", 0x00000004u}, {"SBS_RIGHTALIGN", 0x00000004u},
		{"SBS_DISABLENOSCROLL", 0x00000040u},
	};
	for (const auto &e : k)
		if (up == e.n) { val = e.v; return true; }
	return false;
}

// 预定义控件类序数（MAKEINTRESOURCE 原子值）。
enum DialogPredefinedClass : uint16_t {
	DPC_BUTTON = 128, DPC_EDIT = 129, DPC_STATIC = 130,
	DPC_LISTBOX = 131, DPC_SCROLLBAR = 132, DPC_COMBOBOX = 133,
};

// 标准窗口类名 → 预定义类原子（CONTROL 语句的类名参数命中则存原子而非串，对齐 rc.exe）。
bool dialog_class_name_to_atom(const std::u16string &name, uint16_t &atom)
{
	auto eq = [&](const char *ascii) {
		std::u16string u = rcutil::utf8_to_u16(ascii);
		return u == name;
	};
	if (eq("BUTTON")) { atom = DPC_BUTTON; return true; }
	if (eq("Edit")) { atom = DPC_EDIT; return true; }
	if (eq("Static")) { atom = DPC_STATIC; return true; }
	if (eq("ListBox")) { atom = DPC_LISTBOX; return true; }
	if (eq("ScrollBar")) { atom = DPC_SCROLLBAR; return true; }
	if (eq("ComboBox")) { atom = DPC_COMBOBOX; return true; }
	return false;
}

// 控件关键字 → 内置类原子 + 默认样式 + 是否带文本首参（真值探针 _tmp/dlg 坐实，位置无关）。
struct DialogControlInfo
{
	uint16_t class_atom;    // 预定义类原子（#128..）
	uint32_t default_style; // rc.exe 该关键字默认样式（最终与位置参按位或）
	bool has_text;          // 语句首参是否为文本（EDITTEXT/LISTBOX/COMBOBOX/SCROLLBAR 无）
};
bool dialog_control_info(const std::string &up, DialogControlInfo &out)
{
	static const struct { const char *n; uint16_t cls; uint32_t st; bool txt; } k[] = {
		{"PUSHBUTTON", DPC_BUTTON, 0x50010000u, true},
		{"DEFPUSHBUTTON", DPC_BUTTON, 0x50010001u, true},
		{"CHECKBOX", DPC_BUTTON, 0x50010002u, true},
		{"RADIOBUTTON", DPC_BUTTON, 0x50000004u, true},
		{"GROUPBOX", DPC_BUTTON, 0x50000007u, true},
		{"AUTOCHECKBOX", DPC_BUTTON, 0x50010003u, true},
		{"AUTORADIOBUTTON", DPC_BUTTON, 0x50000009u, true},
		{"LTEXT", DPC_STATIC, 0x50020000u, true},
		{"RTEXT", DPC_STATIC, 0x50020002u, true},
		{"CTEXT", DPC_STATIC, 0x50020001u, true},
		{"ICON", DPC_STATIC, 0x50000003u, true},
		{"EDITTEXT", DPC_EDIT, 0x50810000u, false},
		{"LISTBOX", DPC_LISTBOX, 0x50800001u, false},
		{"COMBOBOX", DPC_COMBOBOX, 0x50000000u, false},
		{"SCROLLBAR", DPC_SCROLLBAR, 0x50000000u, false},
	};
	for (const auto &e : k)
		if (up == e.n) { out.class_atom = e.cls; out.default_style = e.st; out.has_text = e.txt; return true; }
	return false;
}

std::string rc_dirname(const std::string &path)
{
	const std::size_t pos = path.find_last_of("/\\");
	return pos == std::string::npos ? std::string() : path.substr(0, pos + 1);
}

std::string rc_join(const std::string &dir, const std::string &name)
{
	if (dir.empty())
		return name;
	const char t = dir.back();
	return (t == '/' || t == '\\') ? dir + name : dir + "/" + name;
}

bool file_readable(const std::string &p)
{
	std::ifstream f(p, std::ios::binary);
	return static_cast<bool>(f);
}

// 解析资源文件名 → 真实路径（先相对 .rc 目录，再相对 cwd）。
bool resolve_path(const std::string &dir, const std::string &fname, std::string &out)
{
	const std::string a = rc_join(dir, fname);
	if (file_readable(a))
	{
		out = a;
		return true;
	}
	if (file_readable(fname))
	{
		out = fname;
		return true;
	}
	out = a; // 报错回显用
	return false;
}

struct Ref
{
	bool is_string = false;
	uint16_t id = 0;
	std::u16string str;
};

class RcParser
{
public:
	RcParser(const std::string &rc_path, const std::vector<Token> &toks, std::vector<Error> &errors)
		: rc_(rc_path), dir_(rc_dirname(rc_path)), t_(toks), errors_(errors)
	{
	}

	bool collect(std::vector<ResItem> &items)
	{
		while (more())
		{
			const Token &tk = cur();
			if (is_id(tk) && to_upper(tk.text) == "LANGUAGE")
			{
				// 全局 LANGUAGE 指令
				advance();
				if (!parse_language_into(cur_lang_))
					return false;
				continue;
			}
			if (is_id(tk))
			{
				const std::string up = to_upper(tk.text);
				if (up == "STRINGTABLE" || up == "STRING")
				{
					// 裸 STRINGTABLE/STRING 块（无前置资源名）；跨语句累积，末尾统一编码。
					const SourceLoc loc{tk.loc.file.empty() ? rc_ : tk.loc.file, tk.loc.line,
					                    tk.loc.column};
					if (!parse_stringtable(loc))
						return false;
					continue;
				}
			}
			if (!parse_statement(items))
				return false;
		}
		// 所有语句后：把累积的 STRINGTABLE 项按 (language,group) 分桶发射为 RT_STRING。
		if (!pending_strings_.empty())
		{
			if (!rc_encode_stringtable(pending_strings_, items, errors_))
				return false;
			pending_strings_.clear();
		}
		return !failed_;
	}

private:
	bool more() const { return i_ < t_.size(); }
	const Token &cur() const { return t_[i_]; }
	void advance() { ++i_; }
	bool failed_ = false;

	void err(const Token &t, const std::string &msg)
	{
		errors_.push_back(Error{t.loc.file.empty() ? rc_ : t.loc.file, Pos{t.loc.line, t.loc.column}, msg});
		failed_ = true;
	}

	// LANGUAGE n[, m]（primary 可省 sublang）
	bool parse_language_into(uint16_t &lang)
	{
		if (!more() || !is_num(cur()))
		{
			err(more() ? cur() : back_tok(), "LANGUAGE expects a primary language id");
			return false;
		}
		uint32_t primary = static_cast<uint32_t>(parse_num(cur()));
		advance();
		uint32_t sub = 0;
		if (more() && is_punct(cur(), ","))
		{
			advance();
			if (more() && is_num(cur()))
			{
				sub = static_cast<uint32_t>(parse_num(cur()));
				advance();
			}
		}
		lang = static_cast<uint16_t>(primary | (sub << 10)); // MAKELANGID
		return true;
	}

	Token back_tok() const { return t_.empty() ? Token{} : t_.back(); }

	// 解析一条资源语句（非 LANGUAGE）。
	bool parse_statement(std::vector<ResItem> &items)
	{
		const Token &name_tok = cur();
		// NAME：数字 / 带引号串 / 裸标识符（对齐 rc.exe：未定义标识符→大写无引号字符串名）
		if (!is_num(name_tok) && !is_str(name_tok) && !is_id(name_tok))
		{
			err(name_tok, "expected resource name/id (number, \"string\", or identifier)");
			return false;
		}
		Ref name = parse_ref(name_tok);
		const SourceLoc loc{name_tok.loc.file.empty() ? rc_ : name_tok.loc.file, name_tok.loc.line,
		                    name_tok.loc.column};
		advance();

		if (!more())
		{
			err(name_tok, "expected resource type after name");
			return false;
		}
		const Token &type_tok = cur();
		Ref type;
		bool is_version = false;
		if (is_id(type_tok))
		{
			const std::string up = to_upper(type_tok.text);
			if (up == "VERSIONINFO" || up == "VERSION")
			{
				is_version = true;
				type.is_string = false;
				type.id = 16;
			}
			else if (is_unsupported_type(up))
			{
				err(type_tok, "resource type '" + up + "' is not supported in this build (v1)");
				return false;
			}
			else
			{
				const uint16_t id = rc_builtin_type_id(up);
				if (id == 0)
				{
					// 非内置/非禁用关键词 → 自定义字符串类型（大写、无引号），对齐 rc.exe
					type.is_string = true;
					type.str = rcutil::utf8_to_u16(up);
				}
				else
				{
					type.is_string = false;
					type.id = id;
				}
			}
			advance();
		}
		else if (is_num(type_tok) || is_str(type_tok))
		{
			type = parse_ref(type_tok);
			advance();
		}
		else
		{
			err(type_tok, "expected resource type (keyword, number, or \"string\")");
			return false;
		}

		// VERSIONINFO：块语法，单独处理
		if (is_version)
			return parse_versioninfo(name, loc, items);

		// DIALOGEX（type 5）：块语法，类型解析后立即接管——头部可选子句
		//   （STYLE/EXSTYLE/CAPTION/MENU/CLASS/FONT/helpID）出现在类型之后、BEGIN 之前，
		//   形态同 VERSIONINFO，非 ACCELERATORS 的 DATA 位置拦截。（经典 DIALOG 已在上方报不支持。）
		if (!type.is_string && type.id == 5)
			return parse_dialogex(name, loc, items);

		// 带前置资源名的 STRINGTABLE：rc.exe 拒（named STRINGTABLEs are not allowed）。
		//   裸 STRINGTABLE/STRING 已由 collect() 分派，不会走到此处的 type.id==6。
		if (!type.is_string && type.id == 6)
		{
			err(type_tok, "named STRINGTABLE/STRING is not allowed");
			return false;
		}

		// 跳过存储属性关键字
		while (more() && is_id(cur()) && is_attr(to_upper(cur().text)))
			advance();

		// 可选 per-item LANGUAGE
		uint16_t lang = cur_lang_;
		if (more() && is_id(cur()) && to_upper(cur().text) == "LANGUAGE")
		{
			advance();
			if (!parse_language_into(lang))
				return false;
		}

		// ACCELERATORS（type 9）：BEGIN…END 块形态，独立解析并编码（前置 attrs/LANGUAGE 已处理）
		if (!type.is_string && type.id == 9)
			return parse_accelerators(name, lang, loc, items);

		// MENU（type 4，MENUTEMPLATE）：BEGIN…END 块语法，接管式解析。
		if (!type.is_string && type.id == 4)
			return parse_menu(name, lang, loc, items);

		// DATA
		if (!more())
		{
			err(loc_tok(), "resource statement missing data (\"file\" or { bytes })");
			return false;
		}
		const Token &d = cur();
		if (is_punct(d, "{"))
		{
			std::vector<uint8_t> data;
			if (!parse_inline_bytes(data))
				return false;
			push_item(items, type, name, lang, std::move(data), loc);
			return !failed_;
		}
		if (is_str(d))
		{
			const std::string fname = rcutil::rc_decode_string(d.text); // 去引号/解转义（rc.exe 规则）
			advance();
			std::string real;
			if (!resolve_path(dir_, fname, real))
			{
				err(d, "resource file not found: " + fname);
				return false;
			}
			// 依类型分派
			if (!type.is_string && type.id == 3) // ICON → 拆分
			{
				// group 名两态透传（数字序数或字符串：裸标识符/带引号串，对齐 rc.exe）
				std::vector<ResItem> rticon;
				ResItem group;
				if (!rc_encode_icon(real, name.is_string, name.id, name.str, lang, rticon, group,
				                    errors_, loc, next_icon_id_))
				{
					failed_ = true;
					return false;
				}
				for (auto &r : rticon)
					items.push_back(std::move(r));
				items.push_back(std::move(group));
				return !failed_;
			}
			if (!type.is_string && type.id == 2) // BITMAP → 裁剪
			{
				std::vector<uint8_t> dib;
				if (!rc_strip_bitmap(real, dib, errors_, loc))
				{
					failed_ = true;
					return false;
				}
				push_item(items, type, name, lang, std::move(dib), loc);
				return !failed_;
			}
			// 其余（MANIFEST/RCDATA/自定义）：文件字节原样
			std::vector<uint8_t> bytes;
			if (!rcutil::read_file_bytes(real, bytes))
			{
				err(d, "cannot read resource file: " + fname);
				return false;
			}
			push_item(items, type, name, lang, std::move(bytes), loc);
			return !failed_;
		}
		err(d, "expected resource data: \"filename\" or { bytes }");
		return false;
	}

	// STRING / STRINGTABLE BEGIN id "str" { id "str" } END —— 裸块（无前置资源名）。
	//   条目累积到 pending_strings_（打当前 language），跨语句合并；实际编码在 collect() 末尾。
	bool parse_stringtable(const SourceLoc &loc)
	{
		advance(); // STRINGTABLE / STRING
		if (!more() || !(is_id(cur()) && to_upper(cur().text) == "BEGIN"))
		{
			err(loc_tok(), "STRINGTABLE missing BEGIN");
			return false;
		}
		advance(); // BEGIN
		while (more())
		{
			if (is_id(cur()) && to_upper(cur().text) == "END")
				break;
			if (is_punct(cur(), ","))
			{
				advance();
				continue;
			}
			if (!is_num(cur()))
			{
				err(cur(), "STRINGTABLE entry expects <id> \"string\"");
				return false;
			}
			const uint64_t id = parse_num(cur());
			advance();
			if (!more() || !is_str(cur()))
			{
				err(loc_tok(), "STRINGTABLE entry missing string value");
				return false;
			}
			StringEntry e;
			e.language = cur_lang_;
			e.id = static_cast<uint32_t>(id);
			e.text = rcutil::utf8_to_u16(rcutil::rc_decode_string(cur().text));
			e.loc = loc;
			pending_strings_.push_back(std::move(e));
			advance();
		}
		if (!more() || !(is_id(cur()) && to_upper(cur().text) == "END"))
		{
			err(loc_tok(), "STRINGTABLE missing END");
			return false;
		}
		advance(); // END
		return !failed_;
	}

	// ACCELERATORS 块：name ACCELERATORS [attrs] [LANGUAGE n] BEGIN key , cmd [, opts] … END
	//   逐条目切分（字段以逗号分隔、选项位于 cmd 之后、可叠加且顺序无关）；
	//   key：带引号单字符→码元值；裸 VK_*→查 accel_vk 得键码且须伴随 VIRTKEY（对齐 rc.exe RC2163）。
	//   cmd：须数值（宏展开后）→ WORD；非数值报错（对齐 rc.exe，不猜值）。
	bool parse_accelerators(const Ref &name, uint16_t lang, const SourceLoc &loc,
	                        std::vector<ResItem> &items)
	{
		if (!more() || !(is_id(cur()) && to_upper(cur().text) == "BEGIN"))
		{
			err(loc_tok(), "ACCELERATORS missing BEGIN");
			return false;
		}
		advance(); // BEGIN
		std::vector<AccelEntry> accels;
		while (more())
		{
			if (is_id(cur()) && to_upper(cur().text) == "END")
				break;
			if (is_punct(cur(), ",")) // 条目间逗号 / 杂散逗号
			{
				advance();
				continue;
			}
			AccelEntry a;
			a.loc = loc;
			const Token &k = cur();
			bool key_is_vk = false;
			if (is_str(k)) // 带引号字符键
			{
				const std::u16string u = rcutil::utf8_to_u16(rcutil::rc_decode_string(k.text));
				if (u.size() != 1)
				{
					err(k, "ACCELERATORS key must be a single character");
					return false;
				}
				a.key = static_cast<uint16_t>(u[0]);
				advance();
			}
			else if (is_id(k)) // 裸 VK_* 虚拟键
			{
				uint16_t code;
				if (!accel_vk_value(to_upper(k.text), code))
				{
					err(k, "unknown accelerator key: " + k.text);
					return false;
				}
				a.key = code;
				key_is_vk = true;
				advance();
			}
			else
			{
				err(k, "ACCELERATORS key must be \"char\" or VK_* name");
				return false;
			}
			// , cmd
			if (!more() || !is_punct(cur(), ","))
			{
				err(loc_tok(), "ACCELERATORS entry expects 'key , cmd'");
				return false;
			}
			advance();
			if (!more() || !is_num(cur()))
			{
				err(loc_tok(), "ACCELERATORS cmd must be a numeric id");
				return false;
			}
			a.cmd = static_cast<uint16_t>(parse_num(cur()));
			advance();
			// [, opts…]：累计属于本条目的选项；遇非选项标识符/串/数/END 即本条目结束
			while (more())
			{
				if (is_punct(cur(), ","))
				{
					advance();
					continue;
				}
				if (is_id(cur()))
				{
					const std::string up = to_upper(cur().text);
					if (up == "END")
						break;
					uint8_t bits = 0;
					if (accel_option_value(up, bits))
					{
						a.fVirt |= bits;
						advance();
						continue;
					}
					break; // 非选项标识符 → 下一条目的 key（VK_*），交由外层循环
				}
				break; // 串/数 → 下一条目
			}
			if (key_is_vk && !(a.fVirt & 0x01)) // 虚拟键须伴随 VIRTKEY（对齐 rc.exe）
			{
				err(k, "virtual key requires VIRTKEY");
				return false;
			}
			accels.push_back(a);
		}
		if (!more() || !(is_id(cur()) && to_upper(cur().text) == "END"))
		{
			err(loc_tok(), "ACCELERATORS missing END");
			return false;
		}
		advance(); // END

		ResItem it;
		it.name_is_string = name.is_string;
		it.name_id = name.id;
		it.name_str = name.str;
		it.language = lang;
		it.loc = loc;
		if (!rc_encode_accelerators(accels, it, errors_))
			return false;
		items.push_back(std::move(it));
		return !failed_;
	}

	// —— DIALOGEX 块（接管式，仿 parse_versioninfo：类型解析后立即接管）——
	//   name DIALOGEX x,y,cx,cy[,helpID] [STYLE e] [EXSTYLE e] [CAPTION "s"]
	//        [MENU n|"s"] [CLASS n|"s"] [FONT pt,"face"[,w[,i[,c]]]] BEGIN item… END
	bool parse_dialogex(const Ref &name, const SourceLoc &loc, std::vector<ResItem> &items)
	{
		DialogexInput dx;
		dx.loc = loc;
		if (!read_dialog_geom(dx))
			return false;
		bool has_style = false;
		bool has_caption = false;
		uint32_t style_val = 0;
		while (more() && !(is_id(cur()) && to_upper(cur().text) == "BEGIN"))
		{
			if (is_punct(cur(), ","))
			{
				advance();
				continue;
			}
			if (!is_id(cur()))
			{
				err(cur(), "unexpected token in DIALOGEX header");
				return false;
			}
			const std::string up = to_upper(cur().text);
			advance();
			if (up == "STYLE")
			{
				if (!read_style_expr(style_val))
					return false;
				has_style = true;
			}
			else if (up == "EXSTYLE")
			{
				if (!read_style_expr(dx.exstyle))
					return false;
			}
			else if (up == "CAPTION")
			{
				if (!read_caption(dx.title.str))
					return false;
				dx.title.kind = DialogRef::String;
				has_caption = true;
			}
			else if (up == "MENU")
			{
				if (!read_ref_operand(dx.menu))
					return false;
			}
			else if (up == "CLASS")
			{
				if (!read_ref_operand(dx.cls))
					return false;
			}
			else if (up == "FONT")
			{
				if (!read_font(dx))
					return false;
			}
			else
			{
				err(loc_tok(), "unknown DIALOGEX header clause: " + up);
				return false;
			}
		}
		if (!more() || !(is_id(cur()) && to_upper(cur().text) == "BEGIN"))
		{
			err(loc_tok(), "DIALOGEX missing BEGIN");
			return false;
		}
		advance(); // BEGIN
		while (more() && !(is_id(cur()) && to_upper(cur().text) == "END"))
		{
			if (is_punct(cur(), ","))
			{
				advance();
				continue; // 杂散/条目间逗号
			}
			if (!parse_dialog_item(dx))
				return false;
		}
		if (!more() || !(is_id(cur()) && to_upper(cur().text) == "END"))
		{
			err(loc_tok(), "DIALOGEX missing END");
			return false;
		}
		advance(); // END
		// 最终 style：STYLE 子句值（缺省则默认 0x80880000），并 CAPTION→WS_CAPTION、FONT→DS_SETFONT
		uint32_t base = has_style ? style_val : 0x80880000u;
		if (has_caption)
			base |= 0x00C00000u;
		if (dx.has_font)
			base |= 0x00000040u;
		dx.style = base;

		ResItem it;
		it.name_is_string = name.is_string;
		it.name_id = name.id;
		it.name_str = name.str;
		it.language = cur_lang_;
		it.loc = loc;
		if (!rc_encode_dialogex(dx, it, errors_))
			return false;
		items.push_back(std::move(it));
		return !failed_;
	}

	// —— MENU 块（接管式，type 4 MENUTEMPLATE / RT_MENU）——
	//   name MENU [helpID] BEGIN popup_or_item* END
	//   POPUP "text" [, style…] BEGIN popup_or_item* END
	//   MENUITEM "text", id [, style…]
	//   MENUITEM SEPARATOR
	//   style := CHECKED | GRAYED | DISABLED | INACTIVE
	bool parse_menu(const Ref &name, uint16_t lang, const SourceLoc &loc,
	                std::vector<ResItem> &items)
	{
		// 可选 helpID（数字，在类型与 BEGIN 之间）
		uint32_t menu_helpid = 0;
		if (more() && is_num(cur()))
		{
			menu_helpid = static_cast<uint32_t>(parse_num(cur()));
			advance();
		}
		(void)menu_helpid; // 经典 MENUTEMPLATE 头 dwHelpId 始终写 0（rc.exe 实测）
		if (!more() || !(is_id(cur()) && to_upper(cur().text) == "BEGIN"))
		{
			err(loc_tok(), "MENU missing BEGIN");
			return false;
		}
		advance(); // BEGIN
		MenuInput menu;
		menu.loc = loc;
		if (!parse_menu_items(menu.items, 0))
			return false;
		if (!more() || !(is_id(cur()) && to_upper(cur().text) == "END"))
		{
			err(loc_tok(), "MENU missing END");
			return false;
		}
		advance(); // END

		ResItem it;
		it.name_is_string = name.is_string;
		it.name_id = name.id;
		it.name_str = name.str;
		it.language = lang;
		it.loc = loc;
		if (!rc_encode_menu(menu, it, errors_))
			return false;
		items.push_back(std::move(it));
		return !failed_;
	}

	// 解析 BEGIN/END 内的菜单项序列（递归处理 POPUP 嵌套）。
	//   调用点：已在 BEGIN 之后、END 之前循环。
	bool parse_menu_items(std::vector<MenuItemInput> &out, int level)
	{
		while (more() && !(is_id(cur()) && to_upper(cur().text) == "END"))
		{
			if (!is_id(cur()))
			{
				err(cur(), "MENU expects POPUP or MENUITEM");
				return false;
			}
			const std::string up = to_upper(cur().text);
			if (up == "POPUP")
			{
				const SourceLoc iloc = loc_token();
				advance(); // POPUP
				// 串文本
				if (!more() || !is_str(cur()))
				{
					err(loc_tok(), "POPUP expects a string");
					return false;
				}
				MenuItemInput mi;
				mi.loc = iloc;
				mi.level = level;
				mi.is_popup = true;
				mi.text = rcutil::utf8_to_u16(rcutil::rc_decode_string(cur().text));
				advance();
				// 可选 style flags（逗号分隔，直到 BEGIN）
				if (!read_menu_style_flags(mi.flags))
					return false;
				out.push_back(std::move(mi));
				// BEGIN 子项 END
				if (!more() || !(is_id(cur()) && to_upper(cur().text) == "BEGIN"))
				{
					err(loc_tok(), "POPUP missing BEGIN");
					return false;
				}
				advance(); // BEGIN
				if (!parse_menu_items(out, level + 1))
					return false;
				if (!more() || !(is_id(cur()) && to_upper(cur().text) == "END"))
				{
					err(loc_tok(), "POPUP missing END");
					return false;
				}
				advance(); // END
			}
			else if (up == "MENUITEM")
			{
				const SourceLoc iloc = loc_token();
				advance(); // MENUITEM
				if (!more())
				{
					err(loc_tok(), "MENUITEM expects string or SEPARATOR");
					return false;
				}
				// SEPARATOR
				if (is_id(cur()) && to_upper(cur().text) == "SEPARATOR")
				{
					MenuItemInput mi;
					mi.loc = iloc;
					mi.level = level;
					mi.is_separator = true;
					advance(); // SEPARATOR
					out.push_back(std::move(mi));
					continue;
				}
				// 普通 MENUITEM "text", id [, style…]
				if (!is_str(cur()))
				{
					err(cur(), "MENUITEM expects a string or SEPARATOR");
					return false;
				}
				MenuItemInput mi;
				mi.loc = iloc;
				mi.level = level;
				mi.text = rcutil::utf8_to_u16(rcutil::rc_decode_string(cur().text));
				advance();
				// , id
				if (!more() || !is_punct(cur(), ","))
				{
					err(loc_tok(), "MENUITEM expects ',' after text");
					return false;
				}
				advance();
				if (!more() || !is_num(cur()))
				{
					err(loc_tok(), "MENUITEM expects numeric id");
					return false;
				}
				mi.id = static_cast<uint16_t>(parse_num(cur()));
				advance();
				// [, style…] 可选尾缀样式
				if (!read_menu_style_flags(mi.flags))
					return false;
				out.push_back(std::move(mi));
			}
			else
			{
				err(cur(), "MENU expects POPUP or MENUITEM, got: " + up);
				return false;
			}
		}
		return true;
	}

	// 读取逗号分隔的菜单样式标志（CHECKED/GRAYED/DISABLED/INACTIVE）并 OR 到 flags。
	//   可消费零或多个逗号+关键字，遇到 BEGIN/END/POPUP/MENUITEM/串/数即停止。
	bool read_menu_style_flags(uint16_t &flags)
	{
		while (more() && is_punct(cur(), ","))
		{
			// 前看：逗号后若是关键字（非 POPUP/MENUITEM/BEGIN/END 这些结构词）则当样式处理
			if (i_ + 1 >= t_.size() || !is_id(t_[i_ + 1]))
				break;
			const std::string up = to_upper(t_[i_ + 1].text);
			if (up == "POPUP" || up == "MENUITEM" || up == "BEGIN" || up == "END")
				break;
			uint16_t bit = 0;
			if (!menu_style_value(up, bit))
				break;
			advance(); // comma
			advance(); // keyword
			flags |= bit;
		}
		return true;
	}

	// 菜单样式关键字 → mfOption 位值（rc.exe 坐实）。
	bool menu_style_value(const std::string &up, uint16_t &bit)
	{
		if (up == "CHECKED")   { bit = 0x0008; return true; }
		if (up == "GRAYED")    { bit = 0x0001; return true; }
		if (up == "DISABLED")  { bit = 0x0002; return true; }
		if (up == "INACTIVE")  { bit = 0x0002; return true; } // rc.exe 等价 DISABLED
		if (up == "BARBREAK")  { bit = 0x0020; return true; }
		if (up == "MENUBREAK") { bit = 0x0040; return true; }
		return false;
	}

	// 位置参 x,y,cx,cy[,helpID]（helpID 为可选第 5 数值；DIALOGEX 语义）。
	bool read_dialog_geom(DialogexInput &dx)
	{
		if (!read_i16(dx.x))
			return false;
		int16_t *rest[3] = {&dx.y, &dx.cx, &dx.cy};
		for (int k = 0; k < 3; ++k)
		{
			if (!expect_comma() || !read_i16(*rest[k]))
				return false;
		}
		if (more() && is_punct(cur(), ",") && i_ + 1 < t_.size() && is_num(t_[i_ + 1]))
		{
			advance();
			dx.helpid = static_cast<uint32_t>(parse_num(cur()));
			advance();
		}
		return true;
	}

	bool expect_comma()
	{
		if (!more() || !is_punct(cur(), ","))
		{
			err(loc_tok(), "expected ','");
			return false;
		}
		advance();
		return true;
	}

	bool read_i16(int16_t &out)
	{
		if (!more() || !is_num(cur()))
		{
			err(loc_tok(), "expected a numeric value");
			return false;
		}
		out = static_cast<int16_t>(static_cast<uint16_t>(parse_num(cur())));
		advance();
		return true;
	}

	bool read_num16(uint16_t &out)
	{
		if (!more() || !is_num(cur()))
		{
			err(loc_tok(), "expected a numeric id");
			return false;
		}
		out = static_cast<uint16_t>(parse_num(cur()));
		advance();
		return true;
	}

	// 样式 operand：数值直取；标识符查内置样式位表（同 accel_option 精神，未列者报错不猜）。
	bool read_style_operand(uint32_t &v)
	{
		if (more() && is_num(cur()))
		{
			v = static_cast<uint32_t>(parse_num(cur()));
			advance();
			return true;
		}
		if (more() && is_id(cur()))
		{
			const std::string up = to_upper(cur().text);
			if (dialog_style_value(up, v))
			{
				advance();
				return true;
			}
			err(cur(), "unknown style keyword: " + up);
			return false;
		}
		err(loc_tok(), "expected a style value");
		return false;
	}

	// 样式表达式：以 `|` 分隔的多操作数按位或。
	bool read_style_expr(uint32_t &out)
	{
		uint32_t v = 0;
		if (!read_style_operand(v))
			return false;
		out = v;
		while (more() && is_punct(cur(), "|"))
		{
			advance();
			if (!read_style_operand(v))
				return false;
			out |= v;
		}
		return true;
	}

	bool read_caption(std::u16string &out)
	{
		if (!more() || !is_str(cur()))
		{
			err(loc_tok(), "CAPTION expects a \"string\"");
			return false;
		}
		out = rcutil::utf8_to_u16(rcutil::rc_decode_string(cur().text));
		advance();
		return true;
	}

	// MENU/CLASS 尾随引用：串 / 序数（0→空，非 0→原子）。
	bool read_ref_operand(DialogRef &ref)
	{
		if (more() && is_str(cur()))
		{
			ref.kind = DialogRef::String;
			ref.str = rcutil::utf8_to_u16(rcutil::rc_decode_string(cur().text));
			advance();
			return true;
		}
		if (more() && is_num(cur()))
		{
			const uint32_t n = static_cast<uint32_t>(parse_num(cur()));
			advance();
			if (n == 0)
				ref.kind = DialogRef::Empty;
			else
			{
				ref.kind = DialogRef::Ordinal;
				ref.ordinal = static_cast<uint16_t>(n);
			}
			return true;
		}
		err(loc_tok(), "MENU/CLASS expects a number or \"string\"");
		return false;
	}

	// FONT pt,"face"[,weight[,italic[,charset]]]：charset 缺省 1（DEFAULT_CHARSET），italic 归 0/1。
	bool read_font(DialogexInput &dx)
	{
		if (!more() || !is_num(cur()))
		{
			err(loc_tok(), "FONT expects a point size");
			return false;
		}
		dx.font_point = static_cast<uint16_t>(parse_num(cur()));
		advance();
		if (!expect_comma())
			return false;
		if (!more() || !is_str(cur()))
		{
			err(loc_tok(), "FONT expects a typeface \"string\"");
			return false;
		}
		dx.font_face = rcutil::utf8_to_u16(rcutil::rc_decode_string(cur().text));
		advance();
		dx.has_font = true;
		for (int s = 0; s < 3; ++s)
		{
			if (more() && is_punct(cur(), ","))
				advance();
			else
				break;
			if (!more() || !is_num(cur()))
			{
				err(loc_tok(), "FONT expects a numeric parameter");
				return false;
			}
			const uint32_t v = static_cast<uint32_t>(parse_num(cur()));
			advance();
			if (s == 0)
				dx.font_weight = static_cast<uint16_t>(v);
			else if (s == 1)
				dx.font_italic = (v != 0) ? 1 : 0;
			else
				dx.font_charset = static_cast<uint8_t>(v);
		}
		return true;
	}

	// 单条控件语句：关键字（或 CONTROL）→ DialogItemInput，追加到 dx.items。
	bool parse_dialog_item(DialogexInput &dx)
	{
		if (!is_id(cur()))
		{
			err(cur(), "expected a control keyword in DIALOGEX body");
			return false;
		}
		const std::string up = to_upper(cur().text);
		const bool is_control = (up == "CONTROL");
		DialogControlInfo info{};
		if (!is_control && !dialog_control_info(up, info))
		{
			err(cur(), "unknown DIALOGEX control: " + up);
			return false;
		}
		const SourceLoc iloc{cur().loc.file.empty() ? rc_ : cur().loc.file, cur().loc.line,
		                     cur().loc.column};
		advance(); // 关键字
		DialogItemInput it;
		it.loc = iloc;
		if (is_control)
		{
			it.style = 0x50000000u; // CONTROL 基：WS_CHILD|WS_VISIBLE
			if (!more() || !is_str(cur()))
			{
				err(loc_tok(), "CONTROL expects text \"string\"");
				return false;
			}
			it.text.kind = DialogRef::String;
			it.text.str = rcutil::utf8_to_u16(rcutil::rc_decode_string(cur().text));
			advance();
			if (!expect_comma() || !read_num16(it.id) || !expect_comma())
				return false;
			// 类：串（命中标准类名→原子，否则存串）或序数
			if (is_str(cur()))
			{
				const std::u16string nm = rcutil::utf8_to_u16(rcutil::rc_decode_string(cur().text));
				uint16_t atom;
				if (dialog_class_name_to_atom(nm, atom))
				{
					it.cls.kind = DialogRef::Ordinal;
					it.cls.ordinal = atom;
				}
				else
				{
					it.cls.kind = DialogRef::String;
					it.cls.str = nm;
				}
				advance();
			}
			else if (is_num(cur()))
			{
				const uint32_t n = static_cast<uint32_t>(parse_num(cur()));
				advance();
				if (n == 0)
					it.cls.kind = DialogRef::Empty;
				else
				{
					it.cls.kind = DialogRef::Ordinal;
					it.cls.ordinal = static_cast<uint16_t>(n);
				}
			}
			else
			{
				err(loc_tok(), "CONTROL expects a class");
				return false;
			}
			if (!expect_comma())
				return false;
			uint32_t sv = 0;
			if (!read_style_operand(sv))
				return false;
			it.style |= sv;
			if (!expect_comma() || !read_i16(it.x))
				return false;
			if (!expect_comma() || !read_i16(it.y))
				return false;
			if (!expect_comma() || !read_i16(it.cx))
				return false;
			if (!expect_comma() || !read_i16(it.cy))
				return false;
			if (more() && is_punct(cur(), ",") && i_ + 1 < t_.size() && is_num(t_[i_ + 1]))
			{
				advance();
				uint32_t ev = 0;
				if (!read_style_operand(ev))
					return false;
				it.exstyle = ev;
			}
			dx.items.push_back(std::move(it));
			return !failed_;
		}
		// 标准关键字控件：类原子 + 默认样式
		it.cls.kind = DialogRef::Ordinal;
		it.cls.ordinal = info.class_atom;
		it.style = info.default_style;
		if (info.has_text)
		{
			if (!more() || !is_str(cur()))
			{
				err(loc_tok(), "control expects text \"string\"");
				return false;
			}
			it.text.kind = DialogRef::String;
			it.text.str = rcutil::utf8_to_u16(rcutil::rc_decode_string(cur().text));
			advance();
			if (!expect_comma() || !read_num16(it.id))
				return false;
		}
		else
		{
			it.text.kind = DialogRef::Empty;
			if (!read_num16(it.id))
				return false; // 无文本控件首参即 id
		}
		if (!expect_comma() || !read_i16(it.x))
			return false;
		if (!expect_comma() || !read_i16(it.y))
			return false;
		if (!expect_comma() || !read_i16(it.cx))
			return false;
		if (!expect_comma() || !read_i16(it.cy))
			return false;
		// 可选尾随数值：style(按位或) , exstyle , helpID（探针坐实此序）
		for (int oi = 0; oi < 3; ++oi)
		{
			if (!(more() && is_punct(cur(), ",")))
				break;
			if (!(i_ + 1 < t_.size() && is_num(t_[i_ + 1])))
				break;
			advance();
			const uint32_t v = static_cast<uint32_t>(parse_num(cur()));
			advance();
			if (oi == 0)
				it.style |= v;
			else if (oi == 1)
				it.exstyle = v;
			else
				it.helpid = v;
		}
		dx.items.push_back(std::move(it));
		return !failed_;
	}

	// 内联字节 { b0, 0x1, "str", ... }
	bool parse_inline_bytes(std::vector<uint8_t> &data)
	{
		advance(); // {
		while (more())
		{
			const Token &t = cur();
			if (is_punct(t, "}"))
			{
				advance();
				return true;
			}
			if (is_punct(t, ","))
			{
				advance();
				continue;
			}
			if (is_num(t))
			{
				data.push_back(static_cast<uint8_t>(parse_num(t) & 0xFF));
				advance();
				continue;
			}
			if (is_str(t))
			{
				const std::string s = rcutil::rc_decode_string(t.text);
				for (unsigned char c : s)
					data.push_back(c);
				advance();
				continue;
			}
			err(t, "unexpected token in inline data");
			return false;
		}
		err(loc_tok(), "unterminated { inline data }");
		return false;
	}

	void push_item(std::vector<ResItem> &items, const Ref &type, const Ref &name, uint16_t lang,
	               std::vector<uint8_t> &&data, const SourceLoc &loc)
	{
		ResItem it;
		it.type_is_string = type.is_string;
		it.type_id = type.id;
		it.type_str = type.str;
		it.name_is_string = name.is_string;
		it.name_id = name.id;
		it.name_str = name.str;
		it.language = lang;
		it.data = std::move(data);
		it.loc = loc;
		items.push_back(std::move(it));
	}

	Ref parse_ref(const Token &t)
	{
		Ref r;
		if (is_num(t))
		{
			r.is_string = false;
			r.id = static_cast<uint16_t>(parse_num(t));
		}
		else if (is_id(t)) // 裸标识符：大写、无引号（对齐 rc.exe 未定义标识符→字符串）
		{
			r.is_string = true;
			r.str = rcutil::utf8_to_u16(to_upper(t.text));
		}
		else // String：原样含引号（与 rc.exe 串型逐字节一致）
		{
			r.is_string = true;
			r.str = rcutil::utf8_to_u16(t.text);
		}
		return r;
	}

	// —— VERSIONINFO 块 ——
	bool parse_versioninfo(const Ref &name, const SourceLoc &loc, std::vector<ResItem> &items)
	{
		VersionInfoInput vi;
		vi.loc = loc;
		// 固定字段：直到 BEGIN
		while (more() && !(is_id(cur()) && to_upper(cur().text) == "BEGIN"))
		{
			if (is_punct(cur(), ","))
			{
				advance();
				continue;
			}
			if (!is_id(cur()))
			{
				err(cur(), "unexpected token in VERSIONINFO header");
				return false;
			}
			const std::string up = to_upper(cur().text);
			advance();
			if (up == "FILEVERSION")
				read_version_tuple(vi.fileversion, vi.has_fileversion);
			else if (up == "PRODUCTVERSION")
				read_version_tuple(vi.productversion, vi.has_productversion);
			else if (up == "FILEFLAGSMASK")
				vi.fileflagsmask = static_cast<uint32_t>(read_single_num());
			else if (up == "FILEFLAGS")
				vi.fileflags = static_cast<uint32_t>(read_single_num());
			else if (up == "FILEOS")
				vi.fileos = static_cast<uint32_t>(read_single_num()); // 对齐 rc.exe：指定则覆盖，省略默认 0
			else if (up == "FILETYPE")
				vi.filetype = static_cast<uint32_t>(read_single_num());
			else if (up == "FILESUBTYPE")
				vi.filesubtype = static_cast<uint32_t>(read_single_num());
			else if (up == "FILEDATE")
			{
				vi.datedateMS = static_cast<uint32_t>(read_single_num());
				if (more() && is_punct(cur(), ","))
					advance();
				vi.datedateLS = static_cast<uint32_t>(read_single_num());
			}
			else
			{
				err(loc_tok(), "unknown VERSIONINFO field: " + up);
				return false;
			}
		}
		if (!more())
		{
			err(loc_tok(), "VERSIONINFO missing BEGIN");
			return false;
		}
		advance(); // BEGIN
		std::vector<std::string> path;
		if (!parse_version_body(vi, path))
			return false;
		if (!more() || !(is_id(cur()) && to_upper(cur().text) == "END"))
		{
			err(loc_tok(), "VERSIONINFO missing END");
			return false;
		}
		advance(); // END

		std::vector<uint8_t> data;
		rc_build_version_info(vi, data, errors_);
		push_item(items, Ref{false, 16, {}}, name, cur_lang_, std::move(data), loc);
		return !failed_;
	}

	// BEGIN…END 之间：BLOCK 嵌套 + VALUE。path 为当前块键栈。
	bool parse_version_body(VersionInfoInput &vi, std::vector<std::string> &path)
	{
		while (more())
		{
			if (is_id(cur()) && to_upper(cur().text) == "END")
				return true;
			if (is_punct(cur(), ","))
			{
				advance();
				continue;
			}
			if (is_id(cur()) && to_upper(cur().text) == "BLOCK")
			{
				advance();
				if (!more() || !is_str(cur()))
				{
					err(loc_tok(), "BLOCK expects a \"string\" key");
					return false;
				}
				const std::string key = rcutil::rc_decode_string(cur().text);
				advance();
				path.push_back(key);
				// StringFileInfo 的直接子块键 = lang/charset StringTable
				if (path.size() == 2 && path[0] == "StringFileInfo")
					vi.lang_charset = key;
				if (!more() || !(is_id(cur()) && to_upper(cur().text) == "BEGIN"))
				{
					err(loc_tok(), "BLOCK missing BEGIN");
					path.pop_back();
					return false;
				}
				advance(); // BEGIN
				if (!parse_version_body(vi, path))
				{
					path.pop_back();
					return false;
				}
				if (!more() || !(is_id(cur()) && to_upper(cur().text) == "END"))
				{
					err(loc_tok(), "BLOCK missing END");
					path.pop_back();
					return false;
				}
				advance(); // END
				path.pop_back();
				continue;
			}
			if (is_id(cur()) && to_upper(cur().text) == "VALUE")
			{
				advance();
				if (!more() || !is_str(cur()))
				{
					err(loc_tok(), "VALUE expects a \"string\" key");
					return false;
				}
				const std::string kname = rcutil::rc_decode_string(cur().text);
				advance();
				const bool in_var = std::find(path.begin(), path.end(), "VarFileInfo") != path.end();
				// 值：串（文本）或数字序列（Translation）
				if (more() && is_punct(cur(), ","))
					advance();
				if (more() && is_str(cur()))
				{
					const std::string vval = rcutil::rc_decode_string(cur().text);
					advance();
					if (!in_var)
						vi.strings.emplace_back(kname, vval);
				}
				else
				{
					// 逗号分隔的数字
					std::vector<uint32_t> nums;
					while (more() && is_num(cur()))
					{
						nums.push_back(static_cast<uint32_t>(parse_num(cur())));
						advance();
						if (more() && is_punct(cur(), ","))
							advance();
					}
					if (in_var && kname == "Translation")
						for (std::size_t k = 0; k + 1 < nums.size(); k += 2)
							vi.translation.push_back((nums[k] << 16) | (nums[k + 1] & 0xFFFF));
				}
				continue;
			}
			err(cur(), "unexpected token in VERSIONINFO block");
			return false;
		}
		err(loc_tok(), "unterminated VERSIONINFO block");
		return false;
	}

	uint64_t read_single_num()
	{
		if (more() && is_num(cur()))
		{
			const uint64_t v = parse_num(cur());
			advance();
			return v;
		}
		err(loc_tok(), "expected a number");
		return 0;
	}

	void read_version_tuple(uint16_t out[4], bool &has)
	{
		has = true;
		int idx = 0;
		while (more() && is_num(cur()) && idx < 4)
		{
			out[idx++] = static_cast<uint16_t>(parse_num(cur()));
			advance();
			if (more() && is_punct(cur(), ","))
				advance();
		}
	}

	Token loc_tok() const { return more() ? cur() : (t_.empty() ? Token{} : t_.back()); }

	SourceLoc loc_token() const
	{
		const Token &t = loc_tok();
		return {t.loc.file.empty() ? rc_ : t.loc.file, t.loc.line, t.loc.column};
	}

	std::string rc_;
	std::string dir_;
	const std::vector<Token> &t_;
	std::vector<Error> &errors_;
	std::size_t i_ = 0;
	uint16_t cur_lang_ = 0x0409;
	uint16_t next_icon_id_ = 1; // RT_ICON 名跨 ICON 语句全局递增，防多语句撞名
	std::vector<StringEntry> pending_strings_; // 跨语句累积的 STRINGTABLE 项，collect() 末尾统一编码
};

}  // namespace

bool rc_collect_items(const std::string &rc_path, const CompileOptions &base,
                      std::vector<ResItem> &out, std::vector<Error> &errors)
{
	std::vector<Token> toks;
	if (!rc_preprocess(rc_path, base, toks, errors))
		return false;
	RcParser p(rc_path, toks, errors);
	return p.collect(out);
}

bool rc_compile(const std::string &rc_path, const CompileOptions &base,
                std::vector<uint8_t> &out_res, std::vector<Error> &errors)
{
	std::vector<ResItem> items;
	if (!rc_collect_items(rc_path, base, items, errors))
		return false;
	rc_emit_res(items, out_res);
	return true;
}

}  // namespace colang
