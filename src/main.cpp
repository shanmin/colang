// ============================================================
// main.cpp — colang 命令行入口
//
// 框架阶段：解析命令行 → 读源（剥 BOM）→ 调 driver::compile → 打印诊断。
// 链接/转储等动作随 P1/P2 各层落地后接线；此处先把参数与选项模型建全。
// ============================================================

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Host.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "colang/common.h"
#include "colang/driver.h"
#include "colang/lexer.h"
#include "colang/link.h"
#include "colang/version.h"
#include "colang/winres.h"

namespace
{

void print_usage(const char *program)
{
	std::printf("colang compiler %s\n\n", COLANG_VERSION_FULL);
	std::printf("usage: %s [options] <source.co>\n\n", program);
	std::printf("options:\n");
	std::printf("  -h, --help        show this help\n");
	std::printf("  -v, --version     show version and target info\n");
	std::printf("  -E                preprocess only, emit expanded translation unit\n");
	std::printf("  --dump-tokens     list every token found in the source\n");
	std::printf("  --dump-ast        parse the source and print the syntax tree\n");
	std::printf("  --emit-llvm       emit LLVM IR text (.ll)\n");
	std::printf("  --per-module      [experimental probe] per-module codegen: one .obj per module, linked in-process\n");
	std::printf("  -c                compile only, produce COFF .obj file\n");
	std::printf("  -o <file>         write output to <file>\n");
	std::printf("  -I <dir>          add header search directory (also -I<dir>)\n");
	std::printf("  --target <triple>  target triple for data model (default: host)\n");
	std::printf("  --resource <file>  embed Win32 resources into exe/dll (Windows; repeatable); accepts .rc (built-in compile) or precompiled .res\n");
}

void print_version()
{
	std::printf("colang %s\n", COLANG_VERSION_FULL);
	std::printf("LLVM   %s\n", LLVM_VERSION_STRING);
	const colang::TargetInfo ti = colang::resolve_target("");
	std::printf("target %s\n", ti.triple.c_str());
	std::printf("model  %s (long=%d, void*=%d, wchar_t=%d)\n",
		ti.is_linux ? "LP64/ILP32 (linux)" : (ti.is_windows ? "LLP64 (windows)" : "unknown"),
		ti.sizeof_long, ti.sizeof_pointer, ti.sizeof_wchar);
}

// 读入整个文件；剥除 UTF-8 BOM（EF BB BF）。失败返回 false。
bool read_source(const std::string &path, std::string &out)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
		static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
	{
		out.erase(0, 3);
	}
	return true;
}

// 定位 colang 自制标准头目录 <...>/include：
// 优先环境变量 COLANG_STDLIB；否则从可执行文件所在目录逐级上溯，以标记文件 stddef.h 定位。
// 未找到返回空串（此时仅 -I 显式路径生效，行为与引入本机制前一致）。
std::string find_stdlib_dir(const std::string &program_path)
{
	namespace fs = std::filesystem;
	if (const char *env = std::getenv("COLANG_STDLIB"))
	{
		if (*env)
		{
			return env;
		}
	}
	std::error_code ec;
	fs::path dir = fs::absolute(fs::path(program_path), ec).parent_path();
	for (int depth = 0; depth < 8 && !dir.empty(); ++depth)
	{
		fs::path cand = dir / "include";
		if (fs::exists(cand / "stddef.h", ec))
		{
			return cand.string();
		}
		dir = dir.parent_path();
	}
	return {};
}

void print_errors(const std::vector<colang::Error> &errors)
{
	for (const colang::Error &error : errors)
	{
		const char *sev = error.is_warning ? "warning" : "error";
		std::fprintf(stderr, "%s:%zu:%zu: %s: %s\n", error.file.c_str(), error.pos.line,
			error.pos.column, sev, error.message.c_str());
		for (const colang::Error &n : error.notes)
			std::fprintf(stderr, "%s:%zu:%zu: note: %s\n", n.file.c_str(), n.pos.line,
				n.pos.column, n.message.c_str());
	}
}

// 诊断列表中是否存在致命错误（排除 warning）
bool has_fatal(const std::vector<colang::Error> &errors)
{
	for (const colang::Error &e : errors)
	{
		if (!e.is_warning)
		{
			return true;
		}
	}
	return false;
}

const char *tok_kind_name(colang::TokKind kind)
{
	switch (kind)
	{
	case colang::TokKind::Identifier:
		return "identifier";
	case colang::TokKind::Keyword:
		return "keyword";
	case colang::TokKind::Number:
		return "number";
	case colang::TokKind::Char:
		return "char";
	case colang::TokKind::String:
		return "string";
	case colang::TokKind::Punct:
		return "punct";
	case colang::TokKind::EndOfFile:
		return "eof";
	}
	return "?";
}

// 把一个 token 流向标准输出逐行转储（--dump-tokens / -E 共用）。
void print_tokens(const std::vector<colang::Token> &tokens)
{
	for (const colang::Token &token : tokens)
	{
		if (token.kind == colang::TokKind::EndOfFile)
		{
			break;
		}
		std::string text = token.text;
		for (char &ch : text)
		{
			if (ch == '\n' || ch == '\r')
			{
				ch = ' '; // 折叠字面量内换行，保持单行显示
			}
		}
		std::printf("%s:%zu:%zu  %-10s  %s\n", token.loc.file.c_str(), token.loc.line,
			token.loc.column, tok_kind_name(token.kind), text.c_str());
	}
}

// --dump-tokens：把源文件切为原始 pp-token（phase 2–4，预处理前）并逐行打印。
int dump_pp_tokens(const colang::CompileOptions &opts)
{
	colang::Lexer lexer(opts.source_path, opts.source_text);
	std::vector<colang::Token> tokens;
	std::vector<colang::Error> errors;
	if (!lexer.tokenize(tokens, errors))
	{
		print_errors(errors);
		return 1;
	}
	print_tokens(tokens);
	return 0;
}

}  // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
	// 控制台统一按 UTF-8 输出（诊断可能回显源文件非 ASCII 内容）
	SetConsoleOutputCP(CP_UTF8);
#endif

	std::vector<std::string> args(argv + 1, argv + argc);

	if (args.empty())
	{
		print_usage(argv[0]);
		return 1;
	}

	colang::CompileOptions opts;
	bool have_input = false;
	for (std::size_t i = 0; i < args.size(); ++i)
	{
		const std::string &arg = args[i];
		if (arg == "-h" || arg == "--help")
		{
			print_usage(argv[0]);
			return 0;
		}
		if (arg == "-v" || arg == "--version")
		{
			print_version();
			return 0;
		}
		if (arg == "-E")
		{
			opts.preprocess_only = true;
			continue;
		}
		if (arg == "--dump-tokens")
		{
			opts.dump_tokens = true;
			continue;
		}
		if (arg == "--dump-ast")
		{
			opts.dump_ast = true;
			continue;
		}
		if (arg == "--emit-llvm")
		{
			opts.emit_llvm = true;
			continue;
		}
		if (arg == "--per-module")
		{
			opts.per_module = true;
			continue;
		}
		if (arg == "--per-module-merge") // 路 B 阶段2：分桶隔离 + linkInModule 合回单 obj（转正默认形态开发开关）
		{
			opts.per_module = true;
			opts.per_module_single_obj = true;
			continue;
		}
		if (arg == "-c")
		{
			opts.compile_only = true;
			continue;
		}
		if (arg == "-o")
		{
			if (i + 1 >= args.size())
			{
				std::fprintf(stderr, "colang: error: option '-o' requires an argument\n");
				return 1;
			}
			opts.output = args[++i];
			continue;
		}
		if (arg == "--resource")
		{
			if (i + 1 >= args.size())
			{
				std::fprintf(stderr, "colang: error: option '--resource' requires an argument\n");
				return 1;
			}
			opts.res_files.push_back(args[++i]);
			continue;
		}
		if (arg == "-I")
		{
			if (i + 1 >= args.size())
			{
				std::fprintf(stderr, "colang: error: option '-I' requires an argument\n");
				return 1;
			}
			opts.include_dirs.push_back(args[++i]);
			continue;
		}
		if (arg.rfind("-I", 0) == 0 && arg.size() > 2)
		{
			opts.include_dirs.push_back(arg.substr(2));
			continue;
		}
		if (arg == "--target")
		{
			if (i + 1 >= args.size())
			{
				std::fprintf(stderr, "colang: error: option '--target' requires an argument\n");
				return 1;
			}
			opts.target_triple = args[++i];
			continue;
		}
		if (arg.rfind("--target=", 0) == 0)
		{
			opts.target_triple = arg.substr(std::string("--target=").size());
			continue;
		}
		if (!arg.empty() && arg[0] == '-')
		{
			std::fprintf(stderr, "colang: error: unknown option '%s'\n", arg.c_str());
			return 1;
		}
		// 位置参数：唯一源文件（多 .bc 合并链接的残留路径已移除）
		opts.source_path = arg;
		have_input = true;
	}

	if (!have_input)
	{
		print_usage(argv[0]);
		return 1;
	}

	// 解析目标 / 数据模型（单一真值源；空 triple → 宿主默认）。必须在任何
	// 会预处理/解析的短路（-E/--dump-tokens/compile）之前完成，使各层取同一目标。
	opts.target = colang::resolve_target(opts.target_triple);

	// 追加自制标准头目录（若找到）作为最低优先级搜索路径（用户 -I 优先）
	{
		const std::string stdlib_dir = find_stdlib_dir(argv[0]);
		if (!stdlib_dir.empty())
		{
			opts.include_dirs.push_back(stdlib_dir);
		}
	}

	if (!read_source(opts.source_path, opts.source_text))
	{
		std::fprintf(stderr, "colang: error: cannot open source file '%s'\n",
			opts.source_path.c_str());
		return 1;
	}

	// 未显式指定任何输出/转储动作时，默认编译并链接为可执行文件
	if (!opts.preprocess_only && !opts.dump_tokens && !opts.dump_ast &&
		!opts.emit_llvm && !opts.compile_only)
	{
		opts.link_output = true;
	}

	// --dump-tokens：词法器已落地，先行短路走 token 转储
	if (opts.dump_tokens)
	{
		return dump_pp_tokens(opts);
	}

	std::vector<colang::Error> errors;
	colang::CompileState state;
	if (!colang::compile(opts, state, errors))
	{
		print_errors(errors);
		return 1;
	}

	// -E：预处理已完成（非活跃分支已裁剪、宏已展开），转储展开后 token 流。
	// 预处理期的非致命诊断（如 #warning）不会中断 -E，但仍需呈现。
	if (opts.preprocess_only)
	{
		print_errors(errors);
		print_tokens(state.tokens);
		return has_fatal(errors) ? 1 : 0;
	}

	// 编译成功：诊断列表中只剩非致命项（warning），照常呈现，不改变退出码
	if (!errors.empty())
		print_errors(errors);

	// PE 资源（档②③）：--resource 仅 Windows 目标、仅链接期生效。
	//   .res → 交 lld COFF 嵌入 .rsrc；.rc → 内置编译为临时 .res（档③），链接后删。
	std::vector<std::string> rc_temp_res; // 内置编译产出的临时 .res（链接后清理）
	if (!opts.res_files.empty())
	{
		if (!opts.target.is_windows)
		{
			std::fprintf(stderr, "colang: error: --resource (PE resources) is only supported for Windows targets\n");
			return 1;
		}
		if (opts.compile_only)
		{
			std::fprintf(stderr, "colang: warning: --resource is ignored under -c (no link step)\n");
		}
		std::error_code rec;
		for (const std::string &r : opts.res_files)
		{
			if (!std::filesystem::exists(r, rec))
			{
				std::fprintf(stderr, "colang: error: resource file not found: %s\n", r.c_str());
				return 1;
			}
		}
		// 档③：对 .rc 项内置编译为临时 .res，并以临时路径替换该项；.res 项原样透传。
		for (std::size_t k = 0; k < opts.res_files.size(); ++k)
		{
			std::string &r = opts.res_files[k];
			std::string low = r;
			for (char &c : low)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (low.size() < 3 || low.compare(low.size() - 3, 3, ".rc") != 0)
				continue;
			std::vector<uint8_t> res_bytes;
			std::vector<colang::Error> rc_errors;
			const bool rc_ok = colang::rc_compile(r, opts, res_bytes, rc_errors);
			if (!rc_errors.empty())
				print_errors(rc_errors);
			if (!rc_ok)
			{
				std::fprintf(stderr, "colang: error: rc compilation failed: %s\n", r.c_str());
				return 1;
			}
			std::error_code tec;
			std::filesystem::path tmpdir = std::filesystem::temp_directory_path(tec);
			if (tec)
				tmpdir = std::filesystem::path(r).parent_path();
			const std::string stem = std::filesystem::path(r).stem().string();
			const std::filesystem::path tmp =
				tmpdir / (stem + ".colang-rc-" + std::to_string(k) + ".res");
			std::ofstream tf(tmp, std::ios::binary);
			if (!tf)
			{
				std::fprintf(stderr, "colang: error: cannot write temporary .res: %s\n",
					tmp.string().c_str());
				return 1;
			}
			tf.write(reinterpret_cast<const char *>(res_bytes.data()),
				static_cast<std::streamsize>(res_bytes.size()));
			tf.close();
			rc_temp_res.push_back(tmp.string());
			r = tmp.string();
		}
	}

	// 链接：进程内 lld 把 COFF .obj 链为可执行文件或动态库（依 -o 扩展名分岔，不再 shell out 外部 clang.exe）
	// --per-module：多 obj（state.obj_paths 非空优先）；否则单 obj 现状路径。
	const std::vector<std::string> objs =
		!state.obj_paths.empty() ? state.obj_paths
		                         : (state.obj_path.empty() ? std::vector<std::string>{}
		                                                    : std::vector<std::string>{state.obj_path});
	if (opts.link_output && !objs.empty())
	{
		std::string out_path = opts.output;
		if (out_path.empty())
		{
			// 输出名依 target（而非 host）：Windows 目标补 .exe，Linux 目标无扩展名。
			out_path = std::filesystem::path(opts.source_path).stem().string();
			if (opts.target.is_windows)
				out_path += ".exe";
		}
		// 链接层按 target 分平台：Windows → 进程内 lldCOFF；Linux → 进程内 lldELF。
		const bool ok = opts.target.is_windows
			? colang::link_exe(objs, out_path, state.windows_subsystem,
					state.use_winmain, opts.res_files, errors)
			: colang::link_elf(objs, out_path, errors);
		std::error_code ec;
		// 临时 obj 统一删（-c 路径不走本块；per-module 的 obj_paths 均为链接模式临时产物）。
		for (const std::string &o : objs)
			std::filesystem::remove(o, ec);
		// 档③：内置 rc 编译产出的临时 .res 一并清理。
		for (const std::string &tr : rc_temp_res)
			std::filesystem::remove(tr, ec);
		if (!ok)
		{
			print_errors(errors);
			std::fprintf(stderr, "colang: error: linking failed\n");
			return 1;
		}
	}
	return 0;
}
