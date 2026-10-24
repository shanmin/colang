// ============================================================
// lld_link.cpp — 进程内链接：调 lld::coff::link 把 COFF .obj 链成 exe 或 dll。
//
// 按输出路径扩展名分岔（复刻原 clang 依 -o 扩展名自动选链接模式的语义）：
//  [exe] 复刻当前 clang 链接站点喂给链接器的参数（经 spike 实测定稿）：
//   - 静态 CRT：-defaultlib:libcmt -defaultlib:oldnames
//   - 公共名 stdio 兜底：-defaultlib:legacy_stdio_definitions（修 sprintf LNK2019）
//   - 大栈：-stack:16777216（承接原 -Wl,/STACK，修多维大局部数组栈溢出）
//   - 子系统/入口（Console + mainCRTStartup）由 libcmt 内嵌 .drectve 自动提供，
//     故此处不显式传 -subsystem/-entry，产物退出码/基线与 clang 一致。
//  [dll] `-o *.dll`：传 -dll -entry:_DllMainCRTStartup，导出表由 codegen 对 public
//   符号发射的 `.drectve` `/EXPORT:` 指令填充；CRT 初始化随 attach 正常执行。
//   导入库由 lld 默认随 -out 基名生成（如 foo.lib）。
// 链接库搜索路径交 lld 自身（%LIB% 环境变量 + Windows SDK/VC 注册表自动发现），
// colang 层不自建发现器。
// ============================================================

#include <cctype>
#include <string>
#include <vector>

#include <llvm/Support/raw_ostream.h>

#include "lld/Common/Driver.h"
LLD_HAS_DRIVER(coff)

#include "colang/link.h"

namespace colang
{

namespace
{
// 大小写无关的后缀判定（Windows 扩展名 .dll/.DLL 同义）。
bool ends_with_ci(const std::string& s, const std::string& suffix)
{
	if (s.size() < suffix.size())
		return false;
	const std::size_t off = s.size() - suffix.size();
	for (std::size_t i = 0; i < suffix.size(); ++i)
	{
		const unsigned char a = static_cast<unsigned char>(s[off + i]);
		const unsigned char b = static_cast<unsigned char>(suffix[i]);
		if (std::tolower(a) != std::tolower(b))
			return false;
	}
	return true;
}
}  // namespace

bool link_exe(const std::vector<std::string> &objs, const std::string &out_path,
              std::vector<Error> &errors)
{
	// 组装 lld 参数向量（首元素为程序名占位，lld COFF 驱动忽略之）。
	std::vector<std::string> argv;
	argv.emplace_back("lld-link");
	for (const auto &o : objs)
		argv.push_back(o);

	// 依输出扩展名分岔：*.dll → 动态库；其余 → 可执行文件。
	// 两者均链入静态 CRT（libcmt）：exe 需 mainCRTStartup、dll 需 _DllMainCRTStartup，
	// 两个入口例程同在 libcmt 内，按链接模式取用。
	const bool as_dll = ends_with_ci(out_path, ".dll");
	argv.emplace_back("-out:" + out_path);
	if (as_dll)
	{
		argv.emplace_back("-dll");
		// 显式指定 DLL 入口，避免 libcmt 的 .drectve `/ENTRY:mainCRTStartup`（exe 专用）误导。
		argv.emplace_back("-entry:_DllMainCRTStartup");
	}
	argv.emplace_back("-defaultlib:libcmt");
	argv.emplace_back("-defaultlib:oldnames");
	argv.emplace_back("-defaultlib:legacy_stdio_definitions");
	argv.emplace_back("-stack:16777216");
	argv.emplace_back("-nologo");

	std::vector<const char *> args;
	args.reserve(argv.size());
	for (const auto &s : argv)
		args.push_back(s.c_str());

	// 捕获 lld 的 stdout/stderr 诊断文本，失败时回灌给 colang 诊断层。
	std::string out_buf, err_buf;
	llvm::raw_string_ostream out_os(out_buf);
	llvm::raw_string_ostream err_os(err_buf);

	const bool ok =
		lld::coff::link(args, out_os, err_os, /*exitEarly=*/false, /*disableOutput=*/false);
	out_os.flush();
	err_os.flush();

	if (!ok)
	{
		// 汇总 lld 输出为诊断（去掉首尾空行），作为链接错误上报。
		std::string msg = out_buf + err_buf;
		const std::string file = objs.empty() ? out_path : objs.front();
		errors.push_back(Error{file, Pos{1, 1}, "linker: " + msg, false});
	}
	return ok;
}

}  // namespace colang
