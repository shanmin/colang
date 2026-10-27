// ============================================================
// lld_link.cpp — 进程内链接：把目标文件链成可执行文件/动态库。
//
// 平台分岔（构建期铁律）：
//   Windows 构建仅编入 lldCOFF（LLD_HAS_DRIVER(coff)），链 COFF .obj；
//   Linux 构建仅编入 lldELF（LLD_HAS_DRIVER(elf)），链 ELF .o。
//   另一平台的入口编译为返回诊断的 stub（否则请求未链入的驱动符号 → undefined symbol）。
//
// [COFF / Windows] 复刻原 clang 链接站点喂给链接器的参数（经 spike 实测定稿）：
//   - 静态 CRT：-defaultlib:libcmt -defaultlib:oldnames
//   - 公共名 stdio 兜底：-defaultlib:legacy_stdio_definitions（修 sprintf LNK2019）
//   - 大栈：-stack:16777216（承接原 -Wl,/STACK，修多维大局部数组栈溢出）
//   - 子系统/入口：默认（Console + mainCRTStartup）由 libcmt 内嵌 .drectve 自动提供；
//     P229 依 windows_subsystem/use_winmain 显式传 -subsystem:windows / -entry:WinMainCRTStartup。
//   - [dll] `-o *.dll`：传 -dll -entry:_DllMainCRTStartup，导出表来自 public 符号的 .drectve /EXPORT:。
//   链接库搜索路径交 lld 自身（%LIB% + Windows SDK/VC 注册表自动发现）。
//
// [ELF / Linux] P2xx 最小闭环：ld.lld -static，喂 crt1.o/crti.o + objs + -lc + crtn.o。
//   crt/libc.a 由 find_crt_dir() 按 env → Debian 多架构 → /usr/lib64 → /usr/lib 定位；缺件给清晰诊断。
// ============================================================

#include <cctype>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <llvm/Support/raw_ostream.h>

#include "lld/Common/Driver.h"
#ifdef _WIN32
LLD_HAS_DRIVER(coff)
#else
LLD_HAS_DRIVER(elf)
#endif

#include "colang/link.h"

namespace colang
{

namespace
{
// 汇总一条链接诊断，回灌给 colang 诊断层（file 取首个 obj，退化取 out_path）。
void push_link_error(const std::string &file, const std::string &msg,
                     std::vector<Error> &errors)
{
	errors.push_back(Error{file, Pos{1, 1}, "linker: " + msg, false});
}

#ifdef _WIN32
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
#else
// 定位含 crt1.o/crti.o/crtn.o 的目录；找不到返回空串。
std::string find_crt_dir()
{
	namespace fs = std::filesystem;
	auto complete = [](const fs::path &d)
	{
		std::error_code ec;
		return fs::exists(d / "crt1.o", ec) && fs::exists(d / "crti.o", ec) &&
		       fs::exists(d / "crtn.o", ec);
	};
	std::vector<fs::path> cand;
	if (const char *env = std::getenv("COLANG_CRT_DIR"))
		if (*env)
			cand.emplace_back(env);
	if (const char *sr = std::getenv("COLANG_LINUX_SYSROOT"))
		if (*sr)
		{
			const fs::path s(sr);
			cand.emplace_back(s / "usr/lib/x86_64-linux-gnu");
			cand.emplace_back(s / "usr/lib64");
			cand.emplace_back(s / "usr/lib");
		}
	// Debian/Ubuntu 多架构默认，其次常见布局兜底。
	cand.emplace_back("/usr/lib/x86_64-linux-gnu");
	cand.emplace_back("/usr/lib64");
	cand.emplace_back("/usr/lib");
	for (const auto &c : cand)
		if (complete(c))
			return c.string();
	return {};
}

// 定位含 libgcc.a 的 GCC 运行库目录（静态 glibc 依赖 libgcc/libgcc_eh 提供
//   __*tf2 软浮点与 _Unwind_Resume/__gcc_personality_v0 展开符）。
//   优先级：env COLANG_GCC_LIB_DIR → 扫描 /usr/lib/gcc/<triplet>/<ver>/（取版本号最大）
//   → 兜底 `gcc -print-file-name=libgcc.a` 仅查询路径（不参与链接）。
std::string find_gcc_lib_dir()
{
	namespace fs = std::filesystem;
	std::error_code ec;
	if (const char *env = std::getenv("COLANG_GCC_LIB_DIR"))
		if (*env)
			return env;
	std::vector<std::string> hits;
	const fs::path base("/usr/lib/gcc");
	if (fs::is_directory(base, ec))
	{
		for (const auto &trip : fs::directory_iterator(base, ec))
			if (trip.is_directory(ec))
				for (const auto &ver : fs::directory_iterator(trip.path(), ec))
					if (ver.is_directory(ec) &&
					    fs::exists(ver.path() / "libgcc.a", ec))
						hits.push_back(ver.path().string());
	}
	if (!hits.empty())
	{
		std::sort(hits.begin(), hits.end());
		return hits.back();
	}
	// 兜底：向 gcc 查询 libgcc.a 路径，取其父目录。
	if (FILE *pp = ::popen("gcc -print-file-name=libgcc.a 2>/dev/null", "r"))
	{
		char buf[1024];
		std::string line;
		if (std::fgets(buf, static_cast<int>(sizeof buf), pp))
			line = buf;
		::pclose(pp);
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
			line.pop_back();
		const auto slash = line.find_last_of('/');
		if (slash != std::string::npos && !fs::exists(line, ec))
			return {};
		if (slash != std::string::npos)
			return line.substr(0, slash);
	}
	return {};
}
#endif
}  // namespace

// ------------------------------------------------------------
// COFF 链接（Windows 构建为真实现；Linux 构建为 stub）
// ------------------------------------------------------------
#ifdef _WIN32

bool link_exe(const std::vector<std::string> &objs, const std::string &out_path,
              bool windows_subsystem, bool use_winmain,
              const std::vector<std::string> &res_files,
              std::vector<Error> &errors)
{
	// 组装 lld 参数向量（首元素为程序名占位，lld COFF 驱动忽略之）。
	std::vector<std::string> argv;
	argv.emplace_back("lld-link");
	for (const auto &o : objs)
		argv.push_back(o);
	// PE 资源（档②）：预编译 .res 紧跟 objs 平铺入 argv，lld COFF 原生读为 .rsrc$01/$02 段嵌入；
	//   exe/dll 两分支通用（图标/版本/manifest 均已在 .res 内，colang 不解析其内容）。
	for (const auto &r : res_files)
		argv.push_back(r);

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
	else
	{
		// P229 exe：Windows GUI 子系统 / WinMain 入口（均命令行覆盖 .drectve）。
		if (windows_subsystem)
			argv.emplace_back("-subsystem:windows");
		if (use_winmain)
			argv.emplace_back("-entry:WinMainCRTStartup");
		else if (windows_subsystem)
			// windows 子系统但不走 WinMain（main/顶层入口）：必须显式钉 mainCRTStartup。
			//   否则 lld 会按 windows 子系统推断入口为 WinMainCRTStartup → 报 undefined WinMain。
			argv.emplace_back("-entry:mainCRTStartup");
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
		const std::string file = objs.empty() ? out_path : objs.front();
		push_link_error(file, out_buf + err_buf, errors);
	}
	return ok;
}

bool link_elf(const std::vector<std::string> &, const std::string &,
              std::vector<Error> &errors)
{
	push_link_error("colang",
	                "ELF linking is not available in this (Windows/COFF-only) build",
	                errors);
	return false;
}

// ------------------------------------------------------------
// ELF 链接（Linux 构建为真实现；Windows 构建为上面的 stub）
// ------------------------------------------------------------
#else  // !_WIN32

bool link_exe(const std::vector<std::string> &, const std::string &, bool, bool,
              const std::vector<std::string> &, std::vector<Error> &errors)
{
	push_link_error("colang",
	                "COFF linking is not available in this (Linux/ELF-only) build",
	                errors);
	return false;
}

bool link_elf(const std::vector<std::string> &objs, const std::string &out_path,
              std::vector<Error> &errors)
{
	const std::string file = objs.empty() ? out_path : objs.front();
	const std::string crt_dir = find_crt_dir();
	if (crt_dir.empty())
	{
		push_link_error(file,
		                "cannot locate Linux CRT objects (crt1.o/crti.o/crtn.o); install "
		                "libc6-dev (Debian/Ubuntu) or glibc-static, or set COLANG_CRT_DIR / "
		                "COLANG_LINUX_SYSROOT",
		                errors);
		return false;
	}
	const std::filesystem::path crt(crt_dir);
	// 静态 libc.a 存在性预检：缺失给专一诊断，避免 lld 的泛化 undefined/-lc not found。
	{
		std::error_code ec;
		if (!std::filesystem::exists(crt / "libc.a", ec))
		{
			push_link_error(file,
			                "static libc not found: " + (crt / "libc.a").string() +
			                    " (install libc6-dev / glibc-static)",
			                errors);
			return false;
		}
	}

	// 复刻 glibc 静态链顺序：crt1.o(_start) + crti.o 先行，objs 居中，
	//   --start-group -lc -lm -lgcc -lgcc_eh --end-group（解 libc/libm↔libgcc 相互引用），crtn.o 末位。
	//   -lm 必给：libm.a 与 libc.a 同处多架构目录（sin/cos/… 由数学函数引用），静态 glibc 不内嵌 math。
	const std::string gcc_dir = find_gcc_lib_dir();
	std::vector<std::string> argv;
	argv.emplace_back("ld.lld");
	argv.push_back((crt / "crt1.o").string());
	argv.push_back((crt / "crti.o").string());
	for (const auto &o : objs)
		argv.push_back(o);
	argv.emplace_back("-static");
	argv.emplace_back("-o");
	argv.push_back(out_path);
	argv.push_back("-L" + crt_dir);
	if (!gcc_dir.empty())
		argv.push_back("-L" + gcc_dir);
	argv.emplace_back("--start-group");
	argv.emplace_back("-lc");
	argv.emplace_back("-lm");
	if (!gcc_dir.empty())
	{
		argv.emplace_back("-lgcc");
		argv.emplace_back("-lgcc_eh");
	}
	argv.emplace_back("--end-group");
	argv.push_back((crt / "crtn.o").string());

	std::vector<const char *> args;
	args.reserve(argv.size());
	for (const auto &s : argv)
		args.push_back(s.c_str());

	std::string out_buf, err_buf;
	llvm::raw_string_ostream out_os(out_buf);
	llvm::raw_string_ostream err_os(err_buf);

	const bool ok =
		lld::elf::link(args, out_os, err_os, /*exitEarly=*/false, /*disableOutput=*/false);
	out_os.flush();
	err_os.flush();

	if (!ok)
		push_link_error(file, out_buf + err_buf, errors);
	return ok;
}

#endif  // _WIN32

}  // namespace colang
