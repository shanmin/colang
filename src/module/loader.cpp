// ============================================================
// loader.cpp — P212 模块导入加载器实现
//
// 见 loader.h 头注释。仅处理 .co 消费、拓序前插合并到主 TU。
// P233：拆成两阶段——load_imports（递归加载、采集标签种子、不合并）+
//   merge_imports（拓序前插合并 + 回填 resolved_module_key）；load_and_merge_imports
//   保留为二者组合的薄封装（主源 imports 由传入 TU 读取）。
// ============================================================

#include "colang/loader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "colang/ast.h"
#include "colang/common.h"
#include "colang/driver.h"
#include "colang/parser.h"
#include "colang/preproc.h"

namespace colang
{

namespace
{

// 统一大小写与路径分隔符的规范化绝对路径 key（Windows FS 大小写不敏感）。
// 优先 weakly_canonical（会 follow symlink、消歧 `.`/`..`），失败退 absolute。
std::string normalize_key(const std::filesystem::path &p)
{
	std::error_code ec;
	std::filesystem::path abs = std::filesystem::weakly_canonical(p, ec);
	if (ec)
	{
		ec.clear();
		abs = std::filesystem::absolute(p, ec);
		if (ec) abs = p; // 极端情况退回原路径
	}
	std::string s = abs.generic_string(); // 统一 '/' 分隔
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// 从模块路径抽「模块名」用于诊断（默认取文件名去扩展名）。
std::string module_name_of(const std::filesystem::path &p)
{
	std::string stem = p.stem().string();
	return stem.empty() ? p.string() : stem;
}

// 读文件；失败返回 false 并由调用方报错（避免 loader 依赖 main.cpp 里的 read_source）。
bool read_module_text(const std::filesystem::path &p, std::string &out)
{
	std::ifstream ifs(p, std::ios::binary);
	if (!ifs) return false;
	std::ostringstream ss;
	ss << ifs.rdbuf();
	out = ss.str();
	return true;
}

// 解析 import 路径 → 绝对路径 + 补 `.co` 后缀（N3：本版仅解析 `.co`、其它后缀直接失败）。
// base_dir = 引用方文件所在目录（主源或某子模块）。
std::filesystem::path resolve_module_path(const CompileOptions &opts,
                                          const std::string &base_dir,
                                          const std::string &raw_filename)
{
	std::filesystem::path p(raw_filename);
	std::filesystem::path abs;
	if (p.is_absolute())
	{
		abs = p;
	}
	else
	{
		abs = std::filesystem::path(base_dir) / p;
	}
	// 补 .co 后缀（若尚未带）
	if (abs.extension().string().empty())
	{
		abs += ".co";
	}
	else if (abs.extension().string() != ".co")
	{
		// 显式指定了非 .co 后缀：本版不支持（.bc/.dll 消费归 0.3.0）。
		// 保留原路径、由 exists 检查报错。
	}
	// 回退检索库模块目录（include 等已入 include_dirs 的路径）：使 winapi.co 这类
	// 库模块可按名导入、无需与调用方同目录。仅当上面「调用方目录相对 / 绝对」候选不存在
	// 时启用，既有相对/绝对解析逐字不变（同目录 import、-I 显式路径均不受影响）。
	std::error_code fec;
	if (!std::filesystem::exists(abs, fec))
	{
		std::filesystem::path rel(raw_filename);
		if (rel.extension().string().empty()) rel += ".co";
		for (const auto &d : opts.include_dirs)
		{
			std::filesystem::path cand = std::filesystem::path(d) / rel;
			if (std::filesystem::exists(cand, fec)) return cand;
		}
	}
	return abs;
}

// 加载器状态：cache + DFS 栈 + postorder 输出。
struct Loader
{
	const CompileOptions &main_opts;
	std::vector<Error> &errs;

	// 规范化绝对路径 → 已加载子 TU（null 表示「正在栈上、尚未完成」，用于环检测哨兵）。
	std::unordered_map<std::string, TUPtr> cache;
	// 规范化路径 → 展示名（主源/子模块均写入，环检测报路径与 module not found 拼串用）。
	std::unordered_map<std::string, std::string> display_map;
	// DFS 栈上的规范化路径（顺序即「引用链」，报环时切片拼接）。
	std::vector<std::string> stack;
	std::unordered_set<std::string> on_stack;
	// DFS 后序输出的规范化路径（首次完成的模块按序 push_back、不含主源）。
	std::vector<std::string> postorder;

	Loader(const CompileOptions &o, std::vector<Error> &e) : main_opts(o), errs(e) {}

	// 递归加载单个模块。abs_path 已解析、normalized_key 已算好。
	// 返回 false 表示错误（not found / cycle / 子模块 preprocess+parse 失败）。
	bool load_recursive(const std::filesystem::path &abs_path,
	                    const std::string &norm_key,
	                    const SourceLoc &referrer_loc)
	{
		// 命中 cache：可能是「已加载完」或「正在栈上（环）」
		auto it = cache.find(norm_key);
		if (it != cache.end())
		{
			if (on_stack.count(norm_key))
			{
				// 环：从栈上定位起点、拼出 chain = 栈上起点 -> … -> 栈顶 -> 目标（=起点）
				std::string chain;
				auto start = std::find(stack.begin(), stack.end(), norm_key);
				if (start == stack.end()) start = stack.begin(); // 兜底
				for (auto p = start; p != stack.end(); ++p)
				{
					if (!chain.empty()) chain += " -> ";
					auto dit = display_map.find(*p);
					chain += (dit != display_map.end()) ? dit->second : *p;
				}
				// 目标模块（即当前报环的导入目标、与栈上起点同一路径）重复拼一次、成闭环
				if (!chain.empty())
				{
					chain += " -> ";
					auto dit = display_map.find(norm_key);
					chain += (dit != display_map.end()) ? dit->second : norm_key;
				}
				errs.push_back(Error{referrer_loc.file,
				                    Pos{referrer_loc.line, referrer_loc.column},
				                    "cycle detected: " + chain});
				return false;
			}
			// 已加载、直接返回（首次执行一次语义、不重复进 postorder）
			return true;
		}

		// 读文件
		std::string text;
		if (!read_module_text(abs_path, text))
		{
			errs.push_back(Error{referrer_loc.file,
			                    Pos{referrer_loc.line, referrer_loc.column},
			                    "module not found: " + abs_path.generic_string()});
			return false;
		}

		// 空文件按合法空翻译单元处理（与 C 一致、避免 source_text.empty 被 driver 拦）
		if (text.empty()) text = "\n";

		// 递归 preprocess + parse
		CompileOptions sub_opts;
		sub_opts.source_path = abs_path.generic_string();
		sub_opts.source_text = std::move(text);
		sub_opts.include_dirs = main_opts.include_dirs;
		// 其它 flag（dump_*/preprocess_only/emit_llvm/...）全 false、走完整 preprocess+parse 路径

		CompileState sub_state;
		if (!preprocess(sub_opts, sub_state, errs)) return false;
		if (!parse(sub_opts, sub_state, errs)) return false;
		if (!sub_state.ast) return false; // parse 应该已保证非空

		TUPtr sub_tu = sub_state.ast;
		// 记录诊断展示名（环检测报路径、同 norm_key 一一对应）
		display_map[norm_key] = abs_path.filename().generic_string();

		// 占位入 cache + 栈
		cache[norm_key] = sub_tu;
		stack.push_back(norm_key);
		on_stack.insert(norm_key);

		// 递归处理子模块自身的 imports
		std::string sub_dir = abs_path.parent_path().generic_string();
		bool ok = true;
		for (const auto &imp : sub_tu->imports)
		{
			auto child_abs = resolve_module_path(main_opts, sub_dir, imp.filename);
			std::string child_key = normalize_key(child_abs);
			if (!load_recursive(child_abs, child_key, imp.loc))
			{
				ok = false;
				break;
			}
		}

		// 弹栈（无论成败、保持一致）
		on_stack.erase(norm_key);
		stack.pop_back();

		if (!ok) return false;

		// DFS 后序：本模块完成后 push 到 postorder
		postorder.push_back(norm_key);
		return true;
	}
};

// 拓序合并：postorder 逆序遍历（依赖方在后、被依赖方在前）、每个前插到主 TU 各 vector 头。
// 结果 = [最深依赖 ... 直接依赖 ..., 主源本体]（符合 B 方案「被依赖方先跑」)。
void merge_bundle(TranslationUnit &main_tu,
                  std::unordered_map<std::string, TUPtr> &cache,
                  const std::vector<std::string> &postorder)
{
	std::size_t imported_stmt_prefix = 0;
	for (auto rit = postorder.rbegin(); rit != postorder.rend(); ++rit)
	{
		auto it = cache.find(*rit);
		if (it == cache.end() || !it->second) continue;
		TranslationUnit &sub = *it->second;
		// 拓序前插时子模块顶层裸语句项数（供 imported_stmt_prefix 累加）。
		const std::size_t sub_stmt_count = sub.top_level_body.size();
		// P212 归属标记：将子模块中所有顶层项的 source_module_key = 规范化绝对路径（*rit）。
		//   sema 可见性走查依赖本标记区分「主源自身」 vs 「被导入」。
		for (auto &f : sub.funcs)   f.source_module_key   = *rit;
		for (auto &g : sub.globals) g.source_module_key   = *rit;
		for (auto &s : sub.structs) s.source_module_key   = *rit;
		for (auto &e : sub.enums)   e.source_module_key   = *rit;
		// 前插声明/定义（顺序不影响 codegen 符号解析、但保留子模块内部相对顺序：
		//   每次前插整段到头部、多子模块连续前插会让「后加载」的（更靠 postorder 尾部）
		//   排在「先加载」的之前——正是「依赖方在后」的逆序展开)。
		main_tu.funcs.insert(main_tu.funcs.begin(),
			std::make_move_iterator(sub.funcs.begin()),
			std::make_move_iterator(sub.funcs.end()));
		main_tu.globals.insert(main_tu.globals.begin(),
			std::make_move_iterator(sub.globals.begin()),
			std::make_move_iterator(sub.globals.end()));
		main_tu.structs.insert(main_tu.structs.begin(),
			std::make_move_iterator(sub.structs.begin()),
			std::make_move_iterator(sub.structs.end()));
		main_tu.enums.insert(main_tu.enums.begin(),
			std::make_move_iterator(sub.enums.begin()),
			std::make_move_iterator(sub.enums.end()));
		// 顶层裸代码：拓序前插 → 子模块 body 排在主源 body 之前（B 方案「main 前置统一 emit」）
		main_tu.top_level_body.insert(main_tu.top_level_body.begin(),
			std::make_move_iterator(sub.top_level_body.begin()),
			std::make_move_iterator(sub.top_level_body.end()));
		// 累计头部「属于 imported」的项数：本次前插 = sub 拓序前的顶层条数。
		imported_stmt_prefix += sub_stmt_count;
		// 注：sub.imports 不搬、主 TU 保留自己原始的 imports 列表供 sema 可见集构造。
		// 清空的 sub 由 shared_ptr 管理、自然析构。
	}
	main_tu.imported_top_level_body_count = imported_stmt_prefix;
}

// 采集 public struct/enum 标签种子（供 P233 跨模块裸标签在 parse 主源前登记）。
//   遍历各已加载子 TU（排除主源哨兵 null），对 is_public 且非空 tag 的 struct/union/enum 出种子。
//   去重：同名仅出一条（首个胜出；跨模块同名 public 标签罕见，冲突交 sema/parse 既有规则处理）。
void harvest_tag_seeds(const std::unordered_map<std::string, TUPtr> &cache,
                       std::vector<ImportTagSeed> &out)
{
	std::unordered_set<std::string> seen;
	for (const auto &kv : cache)
	{
		const TUPtr &tu = kv.second;
		if (!tu) continue; // 主源哨兵
		for (const auto &s : tu->structs)
		{
			if (!s.is_public || s.tag.empty()) continue;
			if (!seen.insert(s.tag).second) continue;
			out.push_back(ImportTagSeed{s.tag, s.is_union, false});
		}
		for (const auto &e : tu->enums)
		{
			if (!e.is_public || e.tag.empty()) continue;
			if (!seen.insert(e.tag).second) continue;
			out.push_back(ImportTagSeed{e.tag, false, true});
		}
	}
}

// P256：采集各已加载子模块的 public const int 全局值（parser 已在模块 parse 期折好入
//   tu->const_globals），供主源 parse 期把 import 常量作 case 标签/常量表达式求值。
//   去重：同名仅出一条（首个胜出；跨模块同名 public const 罕见，冲突交既有规则）。
void harvest_const_seeds(const std::unordered_map<std::string, TUPtr> &cache,
                         std::vector<ImportConstSeed> &out)
{
	std::unordered_set<std::string> seen;
	for (const auto &kv : cache)
	{
		const TUPtr &tu = kv.second;
		if (!tu) continue; // 主源哨兵
		for (const auto &cg : tu->const_globals)
		{
			if (!seen.insert(cg.first).second) continue;
			out.push_back(ImportConstSeed{cg.first, cg.second});
		}
	}
}

}  // namespace

// ============================================================
// 对外接口
// ============================================================

// P233 阶段一：递归加载、去重、环检测；产出 bundle（已加载子 TU + 拓序 + 标签种子）。不合并。
bool load_imports(const CompileOptions &opts, const std::vector<ImportDecl> &root_imports,
                  ImportBundle &bundle, std::vector<Error> &errors)
{
	if (root_imports.empty()) return true; // 快速路径

	Loader loader(opts, errors);

	// 主源路径的所在目录 = 相对 import 的基准
	std::filesystem::path main_abs = std::filesystem::absolute(opts.source_path);
	std::string main_dir = main_abs.parent_path().generic_string();
	std::string main_display = main_abs.filename().generic_string();

	// 主源占位在栈上（间接环「主 → 子 → 主」会命中哨兵报错）
	std::string main_key = normalize_key(main_abs);
	loader.cache[main_key] = nullptr; // 主源不进 cache TU（诊断展示名仍写入）
	loader.display_map[main_key] = main_display;
	loader.stack.push_back(main_key);
	loader.on_stack.insert(main_key);

	bool ok = true;
	for (const auto &imp : root_imports)
	{
		auto child_abs = resolve_module_path(opts, main_dir, imp.filename);
		std::string child_key = normalize_key(child_abs);
		// resolved_module_key 的回填在 merge_imports 统一做（落在最终主源 imports 上）；
		//   此处 root_imports 为 const（预扫副本或主源本体），不回写。
		if (!loader.load_recursive(child_abs, child_key, imp.loc))
		{
			ok = false;
			break;
		}
	}

	// 弹主源栈（保持一致、虽然此时已不影响结果）
	loader.on_stack.erase(main_key);
	if (!loader.stack.empty() && loader.stack.back() == main_key) loader.stack.pop_back();

	if (!ok) return false;

	// 移交已加载子模块 + 拓序给 bundle（供阶段二 merge）；并采集标签种子。
	bundle.cache = std::move(loader.cache);
	bundle.postorder = std::move(loader.postorder);
	// 主源哨兵（null）不应进 bundle.cache；移走后清理。
	bundle.cache.erase(main_key);
	harvest_tag_seeds(bundle.cache, bundle.tag_seeds);
	harvest_const_seeds(bundle.cache, bundle.const_seeds); // P256
	return true;
}

// P233 阶段二：拓序前插合并 + 回填 resolved_module_key。需先跑 load_imports 填充 bundle。
bool merge_imports(const CompileOptions &opts, TranslationUnit &main_tu, ImportBundle &bundle)
{
	// 诊断展示路径（环检测报路径 / module not found 拼串）：主源 = 文件名。
	main_tu.diagnostics_display_path =
		std::filesystem::absolute(opts.source_path).filename().generic_string();
	// 回填 main_tu.imports 各项 resolved_module_key（对主源目录确定性重算）。
	//   注：主源 parse（带种子）产出的 imports 与预扫 root_imports 同源同序；此处以最终主源
	//   imports 为准重算，确保 sema 可见集匹配用的 key 落在正式 AST 上。
	std::filesystem::path main_abs = std::filesystem::absolute(opts.source_path);
	std::string main_dir = main_abs.parent_path().generic_string();
	for (auto &imp : main_tu.imports)
	{
		auto child_abs = resolve_module_path(opts, main_dir, imp.filename);
		imp.resolved_module_key = normalize_key(child_abs);
	}
	merge_bundle(main_tu, bundle.cache, bundle.postorder);
	return true;
}

// 组合薄封装：主源 imports 由传入 TU 读取（非种子路径、.c 无 import 时快速返回）。
bool load_and_merge_imports(const CompileOptions &opts, TranslationUnit &main_tu,
                            std::vector<Error> &errors)
{
	if (main_tu.imports.empty()) return true; // 快速路径
	ImportBundle bundle;
	if (!load_imports(opts, main_tu.imports, bundle, errors)) return false;
	return merge_imports(opts, main_tu, bundle);
}

}  // namespace colang
