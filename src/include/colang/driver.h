// ============================================================
// driver.h — 编译管线编排：源文本 → 预处理 → 语法 → 语义 → 代码生成
//
// 各层产物通过 CompileState 接力（预处理产出 token 流，语法产出 AST…）。
// 框架阶段仍是逐层桩，前沿随 P1→P2→P3 逐层落地自然后移。
// ============================================================
#pragma once

#include <string>
#include <vector>

#include "colang/ast.h"
#include "colang/common.h"
#include "colang/lexer.h"

namespace colang
{

// 一次编译请求的完整选项（由 main 解析命令行后填充）
struct CompileOptions
{
	std::string source_path;   // 源文件路径（诊断回显用）
	std::string source_text;   // 源文件内容

	bool preprocess_only = false; // -E：只做预处理，输出展开结果
	bool dump_tokens = false;     // --dump-tokens
	bool dump_ast = false;        // --dump-ast
	bool emit_llvm = false;       // --emit-llvm：产出 .ll
	bool compile_only = false;    // -c：编译为 COFF .obj，不链接
	bool link_output = false;     // 默认动作：编译并链接为可执行文件
	std::string output;           // -o 指定的输出路径（可空）
	std::vector<std::string> include_dirs; // -I 头文件搜索目录（按顺序）
};

// 层间共享的编译产物；随管线推进逐步填充。
struct CompileState
{
	std::vector<Token> tokens; // preprocess 产出（已展开、已裁剪非活跃分支）
	TUPtr ast;                 // parse 产出（翻译单元）
	std::string obj_path;      // codegen 产出（COFF .obj；链接模式为临时、链接后删除）
	// P229 codegen 产出：入口子系统标志（供 main.cpp 链接调用传 link_exe）。
	bool windows_subsystem = false; // [[subsystem("windows")]] 或 WinMain → -subsystem:windows
	bool use_winmain = false;       // 定义 WinMain → -entry:WinMainCRTStartup
};

// 运行整条管线；诊断追加到 errors，成功返回 true。
bool compile(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors);

}  // namespace colang
