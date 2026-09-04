//
// colang
//
#include "colang.h"
#include <stdio.h>
#include <string>
#include <vector>
#include <string.h>

//版本号（与 PROJECT.md 的"当前版本"字段保持同步：升版本时同步修改此处）
//	只含 MAJOR.MINOR.PATCH，不含阶段标签（阶段标签只在 PROJECT.md 文档里管理，不在 CLI 显示）
//	变动条件见 PROJECT.md 维护规则第 4 条
static const char* COLANG_VERSION = "v0.0.4";

//多字节字符只由首字节贡献列宽，与 lexer 中的列计数逻辑保持一致（copy from lexer.cpp）
static inline bool diag_is_utf8_continue(char c)
{
	return ((unsigned char)c & 0xC0) == 0x80;
}

//把整行（可能含 '\r'）清洗成干净的一行（去掉末尾 \r）。返回 false 表示行越界或文件打不开
static bool diag_load_source_line(const std::string& filename, int row_1based, std::string& out_line)
{
	//col_index 是"显示列"（按 utf-8 字符计数）；token.col_index 是 0-based。
	//读取源文件，按 '\n' 切行，换行时自动跳过 BOM 和 \r。
	FILE* fp = fopen(filename.c_str(), "rb");
	if (!fp) return false;
	std::vector<char> buf;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz > 0) {
		buf.resize((size_t)sz);
		size_t n = fread(buf.data(), 1, (size_t)sz, fp);
		buf.resize(n);
	}
	fclose(fp);

	int cur_row = 1;
	size_t start = 0;
	//剥离 UTF-8 BOM
	if (buf.size() >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
		start = 3;
	for (size_t i = start; i <= buf.size(); i++) {
		bool is_nl = (i == buf.size()) || (buf[i] == '\n');
		if (is_nl) {
			if (cur_row == row_1based) {
				out_line.clear();
				for (size_t j = start; j < i; j++) {
					char c = buf[j];
					if (c == '\r') continue;
					out_line.push_back(c);
				}
				return true;
			}
			cur_row++;
			start = i + 1;
		}
	}
	return false;
}

//把"显示列 col_0based"转换成 out_line 中的字节偏移（用于把 '^' 放到正确的显示位置）。
//与 lexer 列计数规则完全一致：每个非 utf8_continue 字节 = 新列。
static int diag_display_col_to_byte_offset(const std::string& line, int col_0based)
{
	int display = 0;
	for (int b = 0; b < (int)line.size(); b++) {
		if (!diag_is_utf8_continue(line[b])) {
			if (display == col_0based) return b;
			display++;
		}
	}
	//列超出行尾，返回行尾
	return (int)line.size();
}

//打印诊断的通用辅助：filename:line:col header + source line + '^' indicator。
//	stream：stderr 用 fprintf 传 stderr，stdout 用 stdout。
//  banner：如 "---------- Error ----------" / "---------- Warning ----------"
//  message：纯文本错误消息
//  token：含 filename/row_index(0)/col_index(0)/Value
static void diag_print(FILE* stream, const char* banner, const char* message, TOKEN token)
{
	std::string line;
	bool have_line = diag_load_source_line(token.filename, token.row_index + 1, line);
	int token_display_len = 0;
	if (have_line) {
		//token 指示符下划线长度：按 token.Value 实际字符显示宽度估算；至少 1。
		for (char c : token.Value) if (!diag_is_utf8_continue(c)) token_display_len++;
		if (token_display_len <= 0) token_display_len = 1;
		// 保留将来扩展（tab 对齐、多字节下划线用字节偏）：避免 C4189/C4551；引用函数名不调用且不取 sizeof(function)
		static const auto* _unused_diag_fn = &diag_display_col_to_byte_offset; (void)_unused_diag_fn;
	}
	// banner + message
	fprintf(stream, "\n%s\n%s\n", banner, message);
	// filename:line:col: （clang/gcc 风格，便于 IDE 跳转）
	fprintf(stream, " --> %s:%d:%d\n", token.filename.c_str(), token.row_index + 1, token.col_index + 1);
	if (have_line) {
		//行号列固定 MIN_LN_COLS 位右对齐（clang 风格，避免 2/3/4 位行号导致前缀宽度跳变产生"视觉错位"）
		const int MIN_LN_COLS = 4;
		char ln_buf[32];
		int ln_len = snprintf(ln_buf, sizeof(ln_buf), "%d", token.row_index + 1);
		int ln_cols = (ln_len < MIN_LN_COLS) ? MIN_LN_COLS : ln_len;
		char ln_pad[32];
		memset(ln_pad, ' ', ln_cols);
		ln_pad[ln_cols] = '\0';

		//两条前缀必须用同一套 snprintf 公式：" %*s | " = 1 前置空格 + 行号（ln_cols 位右对齐）+ " | "
		//源码行示例（ln=84, cols=4）: "   84 | int ii=12345678901234;"
		//指示行示例              : "      |        ^^^^^^^^^^^^^^"
		//                                        ^^ 第 14 列开始与 token.col_index=8 (0-based) + 前缀宽度对齐
		fprintf(stream, " %*s | %s\n", ln_cols, ln_buf, line.c_str());

		char prefix[64];
		int pl = snprintf(prefix, sizeof(prefix), " %*s | ", ln_cols, ln_pad);
		fwrite(prefix, 1, pl, stream);
		for (int d = 0; d < token.col_index; d++) fputc(' ', stream);
		for (int k = 0; k < token_display_len; k++) fputc('^', stream);
		fputc('\n', stream);
	}
	fprintf(stream, " token_type:%d token_value:%s\n", token.type, token.Value.c_str());
}

void ErrorExit(const char* str, TOKEN token)
{
	diag_print(stdout, "---------- Error ----------", str, token);
	exit(1);
}
void ErrorExit(const char* str, std::vector<TOKEN>& tokens)
{
	if (tokens.size() == 0)
		printf("\n---------- Error ----------\n%s\n", str);
	else
	{
		ErrorExit(str, tokens[0]);
	}
	exit(1);
}

//字面量越界/失精等告警：方案 B 不改退出码，仅提示（stderr），包含源码行和 '^' 指示符
void Warning(const char* str, TOKEN token)
{
	diag_print(stderr, "---------- Warning ----------", str, token);
}

//解析指定的co文件到llvm ir module
void co2m(const char* filename)
{
    SRCINFO srcinfo = loadsrc(filename);

	//词法分析
	std::vector<TOKEN> tokens;
	lexer(tokens, srcinfo);

	//预处理
	lexer_prepare(tokens);

	//printf("---------- Lexer ----------\n");
	//token_echo(tokens, "");

	//语法分析
	//printf("\n---------- AST ----------\n");
	std::vector<AST*> ast_list = ast(tokens);
	//ast_echo(ast_list, "");

	//IR
	//printf("\n---------- IR ----------\n");
	ir(ast_list, filename);

	//printf("\n---------- IR OVER ----------\n");
	printf("\n---------- Compilation complete ----------\n\n");
}

//解析一个指定的co文件到bc格式
void co2bc(const char* filename)
{
	co2m(filename);
}

//取输出文件基名：若以 .co 结尾则替换掉扩展名（test.co → test），否则原样返回
std::string co_base(const char* filename)
{
	std::string base(filename);
	if (base.size() > 3 && base.compare(base.size() - 3, 3, ".co") == 0)
		base.resize(base.size() - 3);
	return base;
}

//命令行执行，编译输入文件为 LLVM IR（.ll 文本 + .bc bitcode）
//	后端（.bc → .obj → .exe）由 tests\colang.bat 编排调用 llc/clang，编译器自身保持纯前端
int main(int argc, char** argv)
{
	//用法输出：-h / --help / /?
	if (argc == 2 && (
		(strcmp(argv[1], "-h") == 0) ||
		(strcmp(argv[1], "--help") == 0) ||
		(strcmp(argv[1], "/?") == 0)))
	{
		printf("colang %s\n", COLANG_VERSION);
		printf("usage: colang <input.co>\n");
		printf("  output: <input>.ll (LLVM IR text) + <input>.bc (LLVM bitcode)\n");
		printf("  backend pipeline (external scripts, e.g. tests\\colang.bat):\n");
		printf("    llc  -filetype=obj .bc -> .obj\n");
		printf("    clang .obj + builtins  -> .exe (and run)\n");
		printf("  exit codes:\n");
		printf("    0  success\n");
		printf("    1  diagnostics / verification error\n");
		printf("    2  file I/O error (cannot open / short read / out of memory)\n");
		return 0;
	}

	//参数处理
	if (argc == 1)
	{
		printf("ERROR: no input file (try -h)\n");
		return 1;
	}

	if (argc > 2)
	{
		// 多余参数：显式警告（不退出，避免破坏"colang a.co b.co"的未来扩展/脚本参数）
		fprintf(stderr, "warning: extra arguments after '%s' ignored (colang currently takes one .co file; use -h for usage)\n", argv[1]);
	}

	//存在性早验：纯前端阶段快速给用户可读提示，不进入 lexer 崩
	{
		FILE* fp = fopen(argv[1], "rb");
		if (!fp) {
			fprintf(stdout, "\n---------- Error ----------\nERROR: cannot open input file: %s\n", argv[1]);
			return 2;
		}
		fclose(fp);
	}

	//每次顶层编译开始：清空 import 全局状态（多 co 独立 CLI 调用仍走同一 import 状态是 OK，
	//但 colang.exe 是单次单文件调用，入口清一次以避免残留上次同进程调用（若将来多文件））
	import_clear_state();
	ir_set_nested_mode(false);
	ir_set_current_module_name("");  //顶层模块：不做符号 mangling（main/顶层函数保留原名）

	co2bc(argv[1]);
	return 0;
}

//	THE END