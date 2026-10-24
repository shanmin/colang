// ============================================================
// driver.cpp — 编译管线编排（P2 全链路打通，P3+ 各层增长）
//
// 顺序执行：preprocess → parse → analyze → generate。某层失败即中断。
// -E 短路在 preprocess 后；链接由 main.cpp 在 generate 产出 .bc 后触发 clang。
// ============================================================

#include "colang/driver.h"

#include "colang/codegen.h"
#include "colang/loader.h"
#include "colang/parser.h"
#include "colang/preproc.h"
#include "colang/sema.h"

namespace colang
{

bool compile(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors)
{
	// 源文本非空校验（空文件按 C 是合法的空翻译单元，此处仅拦 CLI 未读到内容）
	if (opts.source_text.empty())
	{
		errors.push_back(Error{opts.source_path, Pos{1, 1}, "input is empty"});
		return false;
	}

	// 顺序执行管线；任一环节失败即中断（诊断已在该环节内追加）。
	// -E（preprocess_only）在预处理成功后即结束，不再进后续层。
	if (!preprocess(opts, state, errors))
	{
		return false;
	}
	if (opts.preprocess_only)
	{
		return true;
	}
	if (!parse(opts, state, errors))
	{
		return false;
	}
	// P212 Co 层：递归加载并合并 imports（拓序前插子模块 AST 到主 TU）。
	//   主 TU.imports 为空时快速返回、不做任何修改；-E 模式不会进到此处（前面已短路）。
	if (!load_and_merge_imports(opts, *state.ast, errors))
	{
		return false;
	}
	if (!analyze(opts, state, errors))
	{
		return false;
	}
	if (!generate(opts, state, errors))
	{
		return false;
	}
	return true;
}

}  // namespace colang
