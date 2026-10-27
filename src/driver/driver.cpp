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
	// P233 Co 层：`.co` 新面若含顶层 import，走「预扫 import → 先加载采集标签种子 →
	//   带种子单次 parse 主源 → 合并」，使跨模块 public struct/union/enum 可裸用作类型。
	//   `.c` 与无 import 的 `.co` 保持原「parse → load_and_merge」路径、逐字不变。
	bool is_co = false;
	{
		const std::string &sp = opts.source_path;
		is_co = sp.size() >= 3 &&
			(sp.compare(sp.size() - 3, 3, ".co") == 0 || sp.compare(sp.size() - 3, 3, ".CO") == 0);
	}
	// 路 B 阶段1c：有效选项——`.co` 含 import 时转正为 per-module（默认单 obj linkInModule 合回）。
	//   preprocess/parse/load/merge 只读 source/target/include 等、不依 per_module 位，仍传 opts；
	//   eff 仅供 analyze 分流与 generate 消费。显式 --per-module/--per-module-merge 优先；未显式开关时 `.co`+import 恒走 per-module。
	CompileOptions eff = opts;
	bool seeded = false;
	if (is_co)
	{
		std::vector<ImportDecl> root_imports;
		collect_imports(state, root_imports); // 复用 parse_import_decl、只提取顶层 import
		if (!root_imports.empty())
		{
			ImportBundle bundle;
			if (!load_imports(opts, root_imports, bundle, errors))
				return false;
			if (!parse(opts, state, errors, bundle.tag_seeds, bundle.const_seeds))
				return false;
			if (!merge_imports(opts, *state.ast, bundle))
				return false;
			seeded = true;
			// 转正默认：未显式开 --per-module → per-module 单 obj。
			if (!opts.per_module)
			{
				eff.per_module = true;
				eff.per_module_single_obj = true;
			}
		}
	}
	if (!seeded)
	{
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
	}
	// 路 B 阶段1c：per-module 转正/探针路径（eff.per_module）走 analyze_per_module（ModuleCatalog +
	//   撞名分流 + ambient 切断）；无 import/.c 走旧单遍 analyze。
	if (eff.per_module)
	{
		if (!analyze_per_module(eff, state, errors))
		{
			return false;
		}
	}
	else if (!analyze(eff, state, errors))
	{
		return false;
	}
	if (!generate(eff, state, errors))
	{
		return false;
	}
	return true;
}

}  // namespace colang
