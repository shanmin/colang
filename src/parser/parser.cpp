// ============================================================
// parser.cpp — 语法分析器（P3.4）
//
// 手写递归下降 + 优先级爬升。解析范围：
//   函数声明/定义/全局变量/多声明符/数组声明符/int,char,void,const,
//   pointer/参数表/变参/复合语句/if-else/while/do-while/for/break/
//   continue/switch/case/default/goto/label/局部声明/return/表达式语句/
//   二元运算符全集/一元运算/赋值(=/op=)/数组下标/sizeof/三元(?:)/
//   逗号(,)/显式类型转换(cast)/字面量/标识符/函数调用。
// 后续 P3 扩展：struct/union/enum/typedef/++/--。
// ============================================================

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "colang/parser.h"
#include "colang/type_resolver.h"
#include "colang/type_spell.h"

namespace colang
{

namespace
{

// ============================================================
// 内部 Parser 结构
// ============================================================
struct Parser
{
	const std::vector<Token> &toks;
	std::size_t pos = 0;
	std::vector<Error> &errs;
	// typedef 名 -> 类型映射
	std::unordered_map<std::string, CTypePtr> typedefs_;
	// struct/union 定义存储（tag -> index in TU）
	std::unordered_map<std::string, std::size_t> struct_tags_;
	// enum 常量名 -> 值
	std::unordered_map<std::string, std::int64_t> enum_constants_;
	// P232 Co 层：enum 标签名集合（供「省去 enum 关键字」按名作类型）。仅 .co（co_mode_）启用。
	std::unordered_set<std::string> enum_tag_names_;
	// P232 Co 层：源文件是否为 .co「新面」。为 false（纯 C 档 / .c）时，裸 struct/union/enum
	//   标签名**不**被视为类型起始（is_type_start / 声明解析逐字同改前），C 语言零影响由结构坐实。
	bool co_mode_ = false;
	// P233 Co 层：跨模块裸标签种子（指向 driver 持有的 ImportBundle.tag_seeds，仅 .co 且非空时置位）。
	//   parse_translation_unit 起始据此把「已导入模块的 public 标签」登记为类型种子。
	const std::vector<ImportTagSeed> *seeds_ = nullptr;
	// P256：跨模块 public const int 值种子（指向 driver 持有的 ImportBundle.const_seeds，仅 .co 带 import 且非空）。
	//   eval_const_int / is_const_int_expr 据此在主源 parse 期把 import 的 public const int 穿透作 case 标签/常量表达式。
	const std::vector<ImportConstSeed> *const_seeds_ = nullptr;
	// 当前 TU 指针（parse_decl_specifiers 存 struct/enum 用）
	TranslationUnit *cur_tu_ = nullptr;
	bool last_auto_ = false; // parse_decl_specifiers 返回 nullptr 且因 auto
	// parse_decl_specifiers 期间消费到的存储类说明符（说明符可任意顺序，调用方读取后合并）
	bool last_static_ = false;
	bool last_extern_ = false;
	bool last_typedef_ = false;
	bool last_public_ = false; // P211 Co 层：public 存储类位（仅顶层函数/全局有意义）
	unsigned last_alignas_ = 0; // parse_decl_specifiers 捕获的 _Alignas 强制对齐（字节；0=未指定）
	// P250：_Alignas 约束检查（§6.7.2.1p3/p4/p6）所需的两项旁路信息
	SourceLoc last_alignas_loc_;  // 本声明 _Alignas 的位置（诊断指向；仅在 last_alignas_!=0 时有效）
	SourceLoc gnu_alignas_loc_;   // GNU __attribute__((aligned)) 记录位置（脱糖并入 _Alignas 时供诊断指向）
		unsigned gnu_decl_align_ = 0;   // GNU __attribute__((aligned)) 提升通道（本声明 aligned 最大值；0=无）：仅取 max 抬升对齐，不触发 _Alignas 约束拒收
	// 本声明是否出现过 _Alignas（_Alignas(0) 不等于「未出现」：对齐值 0 表示无效果，
	//   但属性本身仍不得用于 typedef/函数/位域等语境——与 clang 同口径）。
	bool last_has_alignas_ = false;
	bool last_register_ = false;  // 声明说明符里出现过 register（_Alignas 与之互斥）
	SourceLoc last_ident_loc_;    // P252：本声明符的标识符位置（clang 的「must be specified on
	//   definition」指标识符列，而非声明首列）
	DeclAttributes last_attrs_;   // P214：parse_decl_specifiers 期间（中置位）识别到的属性
	// P214：属性列表本身语法错（未闭合/缺属性名）时置位。
	//   parse_translation_unit 对顶层项做推测解析、失败会回滚 errs；若不标记，本类诊断会被一并吞掉。
	//   置位后 parse_global_decl_or_func 直接“认领”本项（返回 true）→ 诊断得以保留。
	bool attr_fatal_ = false;
	// 函数名 -> 返回类型（auto 推断用）
	std::unordered_map<std::string, CTypePtr> func_return_types_;
	// 局部变量名 -> 类型（typeof/auto 推断用，作用域简化为全局累积）
	std::unordered_map<std::string, CTypePtr> local_var_types_;
	// struct/union/enum 标签的块作用域栈：块内重定义同名 tag 时重命名为唯一 tag，
	//   避免覆盖文件作用域的同名定义（C §6.2.1p4：tag 是有作用域的标识符）。
	//   每层 map：块内写的 tag → 实际 tag（无重命名时与写名相同）。
	std::vector<std::unordered_map<std::string, std::string>> tag_scope_;
	int block_depth_ = 0;
	unsigned tag_rename_seq_ = 0;
	// 块作用域的普通标识符（局部变量/形参）名字栈：parser 把 enum 常量折叠成字面量，
	//   需先判断该名是否被内层变量声明隐藏（C 同名空间，内层声明隐藏外层 enumerator）。
	std::vector<std::unordered_set<std::string>> var_scope_;
	// P256：局部 const int 值作用域栈（与 var_scope_ 并列压弹）：每层 名→已折常量。
	//   仅登记「初值解析期可折叠」的 const 限定整数局部；内层遮蔽外层/import const（C §6.2.1 同名空间）。
	std::vector<std::unordered_map<std::string, std::int64_t>> const_local_vals_;
	// 函数形参名：进入函数体复合语句时注入新作用域
	std::vector<std::string> pending_param_names_;
	// 目标 / 数据模型（单一真值源，由 parse() 从 opts.target 注入）：
	//   驱动编译期 sizeof/_Alignof 的 long/wchar_t/指针宽度。默认构造=Windows/LLP64，
	//   故 Windows 主机不带 --target 时与旧写死值逐字恒等。
	TargetInfo target;

	Parser(const std::vector<Token> &tokens, std::vector<Error> &errors)
		: toks(tokens), errs(errors)
	{
	}

	// 把源码写的 tag 解析为实际 tag（由内向外查块作用域栈；未命中即原样）
	std::string resolve_tag(const std::string &tag)
	{
		for (auto it = tag_scope_.rbegin(); it != tag_scope_.rend(); ++it)
		{
			auto f = it->find(tag);
			if (f != it->end()) return f->second;
		}
		return tag;
	}

	// 登记局部声明的名字（供 enum 常量遮蔽判定）
	void note_local_name(const std::string &n)
	{
		if (!var_scope_.empty()) var_scope_.back().insert(n);
	}

	// 该名是否被内层局部声明（变量或形参）占用
	bool name_in_scope(const std::string &n) const
	{
		for (auto it = var_scope_.rbegin(); it != var_scope_.rend(); ++it)
			if (it->count(n)) return true;
		return false;
	}

	// 生成块作用域 tag 的内部唯一名：块内定义/首次声明一律独立于文件作用域同名 tag。
	std::string fresh_block_tag(const std::string &tag)
	{
		return tag + ".__blk" + std::to_string(++tag_rename_seq_);
	}

	// 块作用域内的 struct/union 定义：先决定实际 tag，并在解析成员之前登记前置声明，
	//   使成员中的递归自引用（如 struct tnode *left）落到同一 tag。
	//   同块内已声明过 → 沿用映射；否则一律取唯一内部名（C §6.2.1p4：块内定义遮蔽外层同名 tag，
	//   且文件作用域中后出现的同名定义不得覆盖它）。空 tag（匿名）不参与。
	std::string begin_definition_tag(const std::string &tag, bool is_union)
	{
		if (tag.empty()) return tag;
		std::string actual = tag;
		if (block_depth_ > 0 && !tag_scope_.empty())
		{
			auto cur = tag_scope_.back().find(tag);
			if (cur != tag_scope_.back().end())
				actual = cur->second;
			else
			{
				actual = fresh_block_tag(tag);
				tag_scope_.back()[tag] = actual;
			}
		}
		if (!struct_tags_.count(actual))
		{
			StructDef fwd;
			fwd.tag = actual;
			fwd.is_union = is_union;
			cur_tu_->structs.push_back(std::move(fwd));
			struct_tags_[actual] = cur_tu_->structs.size() - 1;
		}
		return actual;
	}

	// —— token 导航 ——
	const Token &peek(std::size_t off = 0) const
	{
		const std::size_t i = pos + off;
		if (i < toks.size())
		{
			return toks[i];
		}
		static const Token eof_tok = [] { Token t; t.kind = TokKind::EndOfFile; return t; }();
		return eof_tok;
	}
	const Token &advance() { return toks[pos++]; }
	bool at_end() const { return pos >= toks.size(); }

	bool check_keyword(const std::string &kw) const
	{
		const Token &t = peek();
		return (t.kind == TokKind::Keyword || t.kind == TokKind::Identifier) && t.text == kw;
	}
	bool check_punct(const std::string &p) const
	{
		const Token &t = peek();
		return t.kind == TokKind::Punct && t.text == p;
	}
	bool match_keyword(const std::string &kw)
	{
		if (check_keyword(kw)) { advance(); return true; }
		return false;
	}
	bool match_punct(const std::string &p)
	{
		if (check_punct(p)) { advance(); return true; }
		return false;
	}
	const Token &expect_punct(const std::string &p)
	{
		if (check_punct(p)) return advance();
		error(peek().loc, "expected '" + p + "'");
		static const Token dummy = [] { Token t; t.kind = TokKind::Punct; return t; }();
		return dummy;
	}
	const Token &expect_identifier()
	{
		if (peek().kind == TokKind::Identifier) return advance();
		error(peek().loc, "expected identifier");
		static const Token dummy = [] { Token t; t.kind = TokKind::Identifier; return t; }();
		return dummy;
	}

	void error(const SourceLoc &loc, const std::string &msg)
	{
		errs.push_back(Error{loc.file, Pos{loc.line, loc.column}, msg});
	}

	// 告警：不置致命标志（Error::is_warning=true，driver 侧 has_fatal 不计入）
	void warn(const SourceLoc &loc, const std::string &msg)
	{
		Error e;
		e.file = loc.file;
		e.pos = Pos{loc.line, loc.column};
		e.message = msg;
		e.is_warning = true;
		errs.push_back(std::move(e));
	}

	// —— 语法规约 ——

	// P233：跨模块裸标签种子登记——把已导入模块的 public struct/union/enum 标签，在主源 parse
	//   起始就登进 struct_tags_/enum_tag_names_，使裸标签（`Point p;`）在 parse 期即被识别为类型。
	//   种子 struct/union 出一条 **inert 前向引用**（is_definition=false、source_module_key 空、is_public  false），
	//   与「关键字跨模块前向引用空壳」同构：不注册进 codegen struct_defs_（需 is_definition），
	//   真正的成员布局/可见性由 merge 前插的实定义 + sema enforce_type_visible 负责。enum 仅登记名。
	void seed_import_tags()
	{
		if (!co_mode_ || !seeds_ || !cur_tu_) return;
		for (const auto &s : *seeds_)
		{
			if (s.is_enum)
			{
				if (!s.name.empty() && !enum_tag_names_.count(s.name))
					enum_tag_names_.insert(s.name);
				continue;
			}
			if (s.name.empty() || struct_tags_.count(s.name)) continue;
			StructDef sd;
			sd.tag = s.name;
			sd.is_union = s.is_union;
			sd.is_definition = false; // inert：codegen 不据此排版
			sd.is_public = false;
			cur_tu_->structs.push_back(std::move(sd));
			struct_tags_[s.name] = cur_tu_->structs.size() - 1;
		}
	}

	TUPtr parse_translation_unit()
	{
		auto tu = std::make_shared<TranslationUnit>();
		cur_tu_ = tu.get();
		seed_import_tags(); // P233：先登记跨模块裸标签种子，再进顶层循环
		while (!at_end())
		{
			// P212 Co 层：`import` 前置识别。词法上属 Keyword，不匹 is_type_start
			//   → parse_global_decl_or_func 失败回滚 → 落入裸语句兜底 → 会误报
			//   “expected declaration or statement”而无法产 ImportDecl；此处短路。
			//   识别语法后填入 TU.imports、供 driver 递归加载使用；Step 1 仅识别不加载，
			//   Step 3 起 driver 才会消费本列表。
			if (check_keyword("import"))
			{
				ImportDecl decl;
				if (parse_import_decl(decl))
				{
					tu->imports.push_back(std::move(decl));
					continue;
				}
				skip_to_semi();
				continue;
			}
			// P211 Co 层入口模型：顶层先试声明/函数定义，失败则回滚后按裸语句入 top_level_body。
			//   回滚需同时恢复 errs（防 expect_punct("(") 等推测性报错污染）。
			const std::size_t save_pos = pos;
			const std::size_t save_errs = errs.size();
			if (parse_global_decl_or_func(*tu))
				continue;
			pos = save_pos;
			errs.resize(save_errs);
			if (!is_type_start() && !check_punct("}"))
			{
				StmtPtr s = parse_stmt();
				if (s)
				{
					tu->top_level_body.push_back(std::move(s));
					continue;
				}
			}
			error(peek().loc, "expected declaration or statement");
			skip_to_semi();
		}
		return tu;
	}

	// P212 import 语法：
	//   import * | name [as alias], … from "file" [as ModAlias] ;
	// `from` / `as` 不作保留字（旧 C 代码可能用同名标识符）、靠上下文词法识别：
	//   本函数中当“当前 token 为 Identifier 且拼写等于 from/as”视为语法位。
	bool parse_import_decl(ImportDecl &out)
	{
		out.loc = peek().loc;
		advance(); // 消耗 'import'

		// 选择器列表（`*` 或 `name [as alias]`、逗号分隔）
		bool first = true;
		while (true)
		{
			if (!first)
			{
				if (!match_punct(",")) break;
			}
			first = false;

			ImportSpecifier spec;
			spec.loc = peek().loc;
			if (check_punct("*"))
			{
				advance();
				spec.is_star = true;
			}
			else if (peek().kind == TokKind::Identifier)
			{
				spec.name = advance().text;
				// 可选 `as alias`（`as` 非保留字、Identifier 拼写判定）
				if (peek().kind == TokKind::Identifier && peek().text == "as")
				{
					advance();
					if (peek().kind != TokKind::Identifier)
					{
						error(peek().loc, "expected alias after 'as'");
						return false;
					}
					spec.alias = advance().text;
				}
			}
			else
			{
				error(peek().loc, "expected '*' or identifier in import list");
				return false;
			}
			out.specs.push_back(std::move(spec));
		}

		// `from` 关键字位（Identifier 拼写判定）
		if (peek().kind != TokKind::Identifier || peek().text != "from")
		{
			error(peek().loc, "expected 'from' in import declaration");
			return false;
		}
		advance();

		// filename 双形式（P212 简化语法）：
		//   • TokKind::String —— 带引号、可跨目录/含后缀、经 co_decode_string 剥前缀/解转义
		//   • TokKind::Identifier —— 简化形式、仅同目录且无引号/后缀/分隔符、loader 自动补 `.co`
		if (peek().kind == TokKind::String)
		{
			out.filename = co_decode_string(advance().text);
		}
		else if (peek().kind == TokKind::Identifier)
		{
			out.filename = advance().text; // 简化：同目录、无后缀、loader 补 `.co`
		}
		else
		{
			error(peek().loc, "expected string literal or identifier as import filename");
			return false;
		}

		// 可选 `as ModAlias`
		if (peek().kind == TokKind::Identifier && peek().text == "as")
		{
			advance();
			if (peek().kind != TokKind::Identifier)
			{
				error(peek().loc, "expected module alias after 'as'");
				return false;
			}
			out.module_alias = advance().text;
		}

		if (!match_punct(";"))
		{
			error(peek().loc, "expected ';' after import declaration");
			return false;
		}
		return true;
	}

	// P233：顶层 import 预扫。维护花括号深度，仅在 depth==0 处识别 `import`，复用
	//   parse_import_decl 取回 ImportDecl；其余 token 按顶层边界推进（depth 0 的 `;`、`{`/`}` 计深）。
	//   不做完整解析、不产生任何依赖完整 AST 的副作用；失败诊断写入构造时传入的独立 scratch、不外汅。
	void collect_top_level_imports(std::vector<ImportDecl> &out)
	{
		int depth = 0;
		pos = 0;
		while (!at_end())
		{
			if (depth == 0 && check_keyword("import"))
			{
				ImportDecl decl;
				if (parse_import_decl(decl))
				{
					out.push_back(std::move(decl));
					continue;
				}
				// 失败：parse_import_decl 至少已消耗 `import`（保证推进）；不收集、继续扫描。
				continue;
			}
			const Token &t = peek();
			if (t.kind == TokKind::Punct)
			{
				if (t.text == "{") ++depth;
				else if (t.text == "}") { if (depth > 0) --depth; }
			}
			advance();
		}
	}

	// 属性列表语法错专用：除报诊断外还置 attr_fatal_（防顶层推测解析回滚吞掉诊断）
	void attr_error(const SourceLoc &loc, const std::string &msg)
	{
		attr_fatal_ = true;
		error(loc, msg);
	}

	// ============================================================
	// C23 [[attribute]]（P214：由「整段吞掉」改为「识别并记录」）
	// ============================================================
	// 旧实现发现 `[[` 就一路找 `]]`，未闭合时吞到 EOF 且零诊断。现按属性名分派：
	//   已落地：deprecated / deprecated("msg")（ISO C23 §6.7.11）、lib("链接名")（Co 层新定义）
	//   P249 落地：nodiscard / noreturn（§6.7.11：前者返还值丢弃告警，后者永不返回登记）
	//   已识别、本批不生效：maybe_unused / fallthrough / trap / pure /
	//                     stable / assert / assume / incomplete / reproducible_constant /
	//                     unsequenced / misleading_casefall（静默接受；maybe_unused/fallthrough
	//                     无从生效——colang 无未使用变量/隐式贯穿类告警可抑制）
	//   厂商/限定名 [[x::y]]：报错拒收（ADR-104 不做厂商扩展）
	//   其余裸名：warning "unknown attribute 'x' ignored" + 跳过（不阻断编译）
	// collect=false = 「接受不生效」位置（局部声明/语句位/typedef/成员）：仍消费 token、
	//   仍报纯语法错（缺属性名/未闭合），但不记录、不报语义诊断（保持既有零诊断行为）。
	void parse_attributes(DeclAttributes &out, bool collect)
	{
		while (check_punct("[") && peek(1).kind == TokKind::Punct && peek(1).text == "[")
		{
			const SourceLoc loc = peek().loc;
			advance(); // [
			advance(); // [
			for (;;)
			{
				if (at_end())
				{
					attr_error(loc, "expected ']]' in attribute list");
					return;
				}
				if (check_punct("]") && peek(1).text == "]")
				{
					advance(); // ]
					advance(); // ]
					break;
				}
				// [[::]] 空属性列表（C23 §6.7.11p3 合法、无效果）
				if (check_punct(":") && peek(1).text == ":")
				{
					advance();
					advance();
					continue;
				}
				if (!parse_one_attribute(out, collect)) return;
				if (check_punct(",")) { advance(); continue; }
				// 已成对的 ']]' → 交由下一轮统一消费（含 EOF 守卫）。
				//   必须同时校验 peek(1)，否则单个 ']'（如误写 `[[deprecated] int f();`）会反复
				//   进入 parse_one_attribute 而不推进 → 死循环。
				if (check_punct("]") && peek(1).text == "]") continue;
				attr_error(peek().loc, "expected ',' or ']]' in attribute list");
				// 错误恢复：跳到匹配的 ']]' 后停住（不继续误读后续声明）
				while (!at_end())
				{
					if (check_punct("]") && peek(1).text == "]") { advance(); advance(); break; }
					advance();
				}
				return;
			}
		}
	}

	// 消费单个属性元素（名 + 可选 '::' 限定链 + 可选实参括号）。
	// 返回 false = 遇 EOF 不可恢复，调用方应立即跳出整条属性列表。
	bool parse_one_attribute(DeclAttributes &out, bool collect)
	{
		const SourceLoc loc = peek().loc;
		std::string name;
		if (peek().kind == TokKind::Identifier || peek().kind == TokKind::Keyword)
			name = advance().text;
		else
			attr_error(loc, "expected attribute name");
		// 厂商/命名空间限定名 a::b：本方言一律拒收（词法无 '::' 单 token，故 ':' ':' 两连判）
		bool qualified = false;
		std::string full = name;
		while (check_punct(":") && peek(1).text == ":")
		{
			advance();
			advance();
			qualified = true;
			if (peek().kind == TokKind::Identifier || peek().kind == TokKind::Keyword)
				full += "::" + advance().text;
		}
		// 可选实参：只取首个字符串字面量，其余 token 消费至匹配的 ')'（嵌套括号计数）
		std::string arg;
		bool has_arg = false;
		if (check_punct("("))
		{
			advance();
			int depth = 1;
			while (depth > 0)
			{
				if (at_end())
				{
					attr_error(loc, "expected ')' in attribute argument list");
					return false;
				}
				const Token &t = peek();
				if (t.kind == TokKind::Punct && t.text == ")") --depth;
				else if (t.kind == TokKind::Punct && t.text == "(") ++depth;
				if (depth > 0 && t.kind == TokKind::String && !has_arg)
				{
					has_arg = true;
					arg = co_decode_string(t.text);
				}
				advance();
			}
		}
		if (!collect) return true;
		if (qualified)
		{
			error(loc, "qualified attribute name '" + full + "' is not supported");
			return true;
		}
		if (name == "deprecated")
		{
			// P251：重复标注幂等接受、**保留首个理由串**（first-wins）——与 clang 同口径：
			//   `[[deprecated("a"), deprecated("b")]]` 用 "a"；`[[deprecated, deprecated("b")]]`
			//   用无理由（首个无实参）；相邻两个前置列表重复亦容忍（clang rc=0）。
			//   修前此处报 "can only be applied once"，对合法代码多报一条 error。
			if (!out.deprecated)
			{
				out.deprecated = true;
				out.deprecated_msg = arg;
			}
			return true;
		}
		if (name == "lib")
		{
			if (!has_arg)
			{
				error(loc, "'lib' attribute requires a string literal library name");
				return true;
			}
			if (out.has_lib)
			{
				error(loc, "'lib' attribute can only be applied once");
				return true;
			}
			// 链接名形式：不含扩展名/路径/空白/引号/冒号（codegen 拼 /DEFAULTLIB:"X.lib"）
			if (arg.empty())
				error(loc, "'lib' attribute requires a non-empty library name");
			else if (arg.size() >= 4 && arg.compare(arg.size() - 4, 4, ".lib") == 0)
				error(loc, "'lib' attribute takes a library name without the '.lib' suffix");
			else if (arg.find_first_of("\\/\"' \t:") != std::string::npos)
				error(loc, "'lib' attribute takes a bare library name (no path or spaces)");
			else
			{
				out.has_lib = true;
				out.lib_name = arg;
			}
			return true;
		}
		if (name == "subsystem")
		{
			if (!has_arg)
			{
				error(loc, "'subsystem' attribute requires a string literal subsystem name");
				return true;
			}
			if (out.subsystem_windows)
			{
				error(loc, "'subsystem' attribute can only be applied once");
				return true;
			}
			// 本方言仅支持 Windows GUI 子系统切换；"console" 为显式默认（不改动）。
			if (arg == "windows")
				out.subsystem_windows = true;
			else if (arg == "console")
				out.subsystem_windows = false;
			else
				error(loc, "'subsystem' attribute expects \"windows\" or \"console\"");
			return true;
		}
		// P249：[[nodiscard]] / [[noreturn]]（§6.7.11 标准属性，本批落地语义）
		//   重复标注：clang 容忍（实测 [[nodiscard, nodiscard]] 无诊断），此处同样幂等接受、
		//   保留首个理由串；与 [[deprecated]] 的「重复即报错」保留差异（见 BACKLOG）。
		if (name == "nodiscard")
		{
			if (!out.nodiscard)
			{
				out.nodiscard = true;
				out.nodiscard_msg = arg;   // [[nodiscard("msg")]] 的理由（可省）
			}
			return true;
		}
		// 拼写等价：[[noreturn]] 与 C11 遗留拼写 [[_Noreturn]] 同一属性（§6.7.11 表 6.8 标准属性名前带下划线的同一性规则）
		if (name == "noreturn" || name == "_Noreturn")
		{
			out.noreturn_attr = true;
			return true;
		}
		// 已识别、本批不生效的标准属性
		static const char *const k_no_effect[] = {
			"maybe_unused", "fallthrough", "trap",
			"pure", "stable", "assert", "assume", "incomplete", "reproducible_constant",
			"unsequenced", "misleading_casefall"};
		for (const char *p : k_no_effect)
			if (name == p) return true;
		if (!name.empty())
			warn(loc, "unknown attribute '" + name + "' ignored");
		return true;
	}

	// 两个声明位（前置 L811 与 parse_decl_specifiers 中置位）的属性归并。
	// P251：deprecated 在**同一位**（同一列表/相邻前置列表）重复已改为幂等接受（见 parse_one_attribute），
	//   但**跨位**（前置 + 中置，如 `[[deprecated]] static [[deprecated]] int g;`）仍报错——
	//   clang 对任何中置位属性一律报 "an attribute list cannot appear here"（rc=1），
	//   此处也跟着拒收保 rc 一致；改为容忍反而会引入 clang 没有的过度接受。
	void merge_attributes(DeclAttributes &dst, const DeclAttributes &src, const SourceLoc &loc)
	{
		if (src.deprecated)
		{
			if (dst.deprecated) { error(loc, "'deprecated' attribute can only be applied once"); return; }
			dst.deprecated = true;
			dst.deprecated_msg = src.deprecated_msg;
		}
		// P249：[[nodiscard]] / [[noreturn]] 跨两个声明位重复时同样幂等合并（保留首个理由串）
		if (src.nodiscard && !dst.nodiscard)
		{
			dst.nodiscard = true;
			dst.nodiscard_msg = src.nodiscard_msg;
		}
		if (src.noreturn_attr) dst.noreturn_attr = true;
		if (src.has_lib)
		{
			if (dst.has_lib) { error(loc, "'lib' attribute can only be applied once"); return; }
			dst.has_lib = true;
			dst.lib_name = src.lib_name;
		}
		if (src.subsystem_windows)
		{
			if (dst.subsystem_windows) { error(loc, "'subsystem' attribute can only be applied once"); return; }
			dst.subsystem_windows = true;
		}
	}

	// 「接受不生效」位的兼容入口：消费并做语法校验，不记录、不报语义诊断
	void skip_attributes()
	{
		DeclAttributes tmp;
		parse_attributes(tmp, false);
		attr_fatal_ = false; // 本位不认领声明，标志不得泄到后续顶层项
	}

	// ============================================================
	// GNU __attribute__ 兼容通道（独立于 C23 [[...]]；ADR-104 分级兼容例外）
	//   定性：为兼容 GCC/Clang 源码而增设——非 ISO C（C89–C23 皆无此语法，标准属性另属 [[...]]），
	//         亦非 colang 方言。策略：仅 aligned(N) 脱糖并入既有 _Alignas 全链走真语义；其余属性名
	//         「接受语法 + 忽略 + 警告」（危险类强警告、其余轻警告），不阻断编译。
	//   返回：本组中 aligned 的最大对齐字节数（0=无 aligned）；非 aligned 者就地发警告。
	//   与 C23 通道物理隔离（不复用 parse_attributes/last_attrs_，免污染其语义收集）。
	// ============================================================
	static std::string gnu_norm_name(const std::string &s)
	{
		// 脱去 GNU 惯用的首尾双下划线：__aligned__ -> aligned
		if (s.size() >= 5 && s.rfind("__", 0) == 0 && s.compare(s.size() - 2, 2, "__") == 0)
			return s.substr(2, s.size() - 4);
		return s;
	}
	// 布局/ABI 相关属性：忽略它们可能改变内存布局/调用约定/链接，故发强警告（P162 教训：静默错编译比报错更危险）。
	static bool gnu_is_layout_abi(const std::string &n)
	{
		static const char *const v[] = {
			"packed", "stdcall", "cdecl", "fastcall", "thiscall", "vectorcall",
			"ms_abi", "sysv_abi", "regparm", "section", "weak", "alias", "ifunc",
			"dllimport", "dllexport", "constructor", "destructor", "tls_model",
			"model", "data_members", "align_common", "naked", "optimize"};
		for (auto p : v) if (n == p) return true;
		return false;
	}
	// 从当前 '(' 之后消费到匹配的 ')'（含嵌套）；用于忽略非 aligned 属性的实参。
	void skip_balanced_until_close()
	{
		int depth = 1; // 调用方已消费左 '('，此处需再匹配一个 ')'
		while (depth > 0 && !at_end())
		{
			if (check_punct("(")) { ++depth; advance(); }
			else if (check_punct(")")) { --depth; if (depth == 0) { advance(); return; } advance(); }
			else advance();
		}
	}
	unsigned consume_gnu_attributes()
	{
		unsigned aligned_acc = 0;
		while (check_keyword("__attribute__"))
		{
			const SourceLoc kw_loc = peek().loc;
			advance(); // __attribute__
			if (!check_punct("(")) { warn(kw_loc, "'__attribute__' expects '('"); break; }
			advance(); // (
			if (!check_punct("(")) { warn(kw_loc, "'__attribute__' expects an attribute list in parentheses"); break; }
			advance(); // (
			for (;;)
			{
				if (check_punct(")")) break; // 内层列表结束，交给收尾双括号
				if (at_end()) { error(kw_loc, "unterminated '__attribute__'"); return aligned_acc; }
				const SourceLoc a_loc = peek().loc;
				std::string raw;
				if (peek().kind == TokKind::Identifier || peek().kind == TokKind::Keyword)
					raw = advance().text;
				else
					warn(a_loc, "expected attribute name in '__attribute__'");
				const std::string name = gnu_norm_name(raw);
				unsigned a_val = 0; bool a_has_arg = false;
				if (check_punct("("))
				{
					if (name == "aligned")
					{
						advance(); // (
						// 支持 aligned(常量表达式)；aligned(alignof(T))/aligned(类型) 等本批降级忽略（罕见）
						if (check_keyword("_Alignof") || check_keyword("alignof") ||
							check_keyword("__alignof__") || is_type_start())
							skip_balanced_until_close();
						else { ExprPtr ce = parse_assign(); a_val = (unsigned)eval_const_int(ce); a_has_arg = true; expect_punct(")"); }
					}
					else
					{
						advance(); // (  非 aligned：整组实参平衡吞
						skip_balanced_until_close();
					}
				}
				if (name == "aligned")
				{
					if (a_has_arg)
					{
						if (a_val != 0 && (a_val & (a_val - 1)) != 0)
							error(a_loc, "'aligned' attribute requires a power of two");
						else if (a_val > aligned_acc) { aligned_acc = a_val; gnu_alignas_loc_ = a_loc; }
					}
					else
						warn(a_loc, "'aligned' without a constant argument is ignored (non-standard, GCC/Clang compatibility)");
				}
				else if (!name.empty())
				{
					if (gnu_is_layout_abi(name))
						warn(a_loc, "gnu attribute '" + name + "' ignored: may affect layout/ABI (non-standard, GCC/Clang compatibility)");
					else
						warn(a_loc, "gnu attribute '" + name + "' ignored (non-standard, GCC/Clang compatibility)");
				}
				if (check_punct(",")) { advance(); continue; }
				break;
			}
			expect_punct(")"); // 内层 )
			expect_punct(")"); // 外层 )
		}
		return aligned_acc;
	}

	// _BitInt(N) 解析（C23 任意宽度整数）
	CTypePtr parse_bitint()
	{
		auto loc = peek().loc;
		advance(); // consume '_BitInt'
		expect_punct("(");
		ExprPtr dim = parse_assign();
		expect_punct(")");
		int64_t n = eval_const_int(dim);
		if (n <= 0 || n > 65535)
			error(loc, "_BitInt width must be 1..65535");
		auto t = std::make_shared<CType>();
		t->kind = TypeKind::BitInt;
		t->bit_width = static_cast<unsigned>(n);
		return t;
	}

	// P140 M3：注入共享 resolve_expr_type 的 parser 侧符号环境。
	// 镜像 CodegenTypeEnv/SemaTypeEnv，读 parser 自有符号表（解析期可见子集）。
	struct ParserAutoTypeEnv
	{
		Parser *self;
		CTypePtr var_type(const std::string &n)
		{
			auto it = self->local_var_types_.find(n);
			if (it != self->local_var_types_.end()) return it->second;
			if (self->cur_tu_)
				for (const auto &gv : self->cur_tu_->globals)
					if (gv.name == n) return gv.type;
			return nullptr;
		}
		CTypePtr struct_member(const std::string &tag, const std::string &field)
		{
			if (!self->cur_tu_) return nullptr;
			for (const auto &s : self->cur_tu_->structs)
				if (s.tag == tag)
					for (const auto &m : s.members)
						if (m.name == field) return m.type;
			return nullptr;
		}
		CTypePtr func_return(const std::string &n)
		{
			auto it = self->func_return_types_.find(n);
			if (it != self->func_return_types_.end()) return it->second;
			return nullptr;
		}
		// 目标数据模型的 long 位宽（供共享解析器整型提升取正确宽度）。
		unsigned long_bits() const { return static_cast<unsigned>(self->target.sizeof_long) * 8u; }
	};

	// 编译期常量整数求值（支持 _Static_assert）
	// C23 auto 类型推断（简化版：根据初始化式字面量推断）
	CTypePtr infer_auto_type(const ExprPtr &e)
	{
		if (!e) return make_type(TypeKind::Int);
		// P140 M3 定向复用：叶/简单节点交共享 resolve_expr_type（复用字面量定型、
		//   nullptr、成员查找修复）；命中即返回，null 落回下方 parser 既有近似。
		//   刻意不委托 Binary/Unary/Call：resolve 对指针算术/va_arg/++-- abstain 或
		//   仅整型取向（int_promo_of 对 double 误得 int）——直接采用会回归 p136/p137。
		switch (e->kind)
		{
		case ExprKind::VarRef:
		case ExprKind::Cast:
		case ExprKind::StringLiteral:
		case ExprKind::FloatLiteral:
		case ExprKind::IntLiteral:
		case ExprKind::CharLiteral:
		case ExprKind::Member:
		{
			ParserAutoTypeEnv env{this};
			if (CTypePtr t = resolve_expr_type(e, env))
				return t;
			break;
		}
		default:
			break;
		}
		switch (e->kind)
		{
		case ExprKind::IntLiteral:
		case ExprKind::CharLiteral:
			// 位精确字面量（wb/ub 后缀）携带 _BitInt 类型，优先采用
			if (e->type && (e->type->kind == TypeKind::BitInt ||
				e->type->kind == TypeKind::LongLong ||
				e->type->kind == TypeKind::Long ||
				e->type->kind == TypeKind::Int)) return e->type;
			return make_type(TypeKind::Int);
		case ExprKind::FloatLiteral:
			// f/F 后缀字面量已在构造处标 TypeKind::Float，优先采用（与 codegen 分派一致）。
			if (e->type && e->type->kind == TypeKind::Float) return e->type;
			return make_type(TypeKind::Double);
		case ExprKind::StringLiteral:
			return make_pointer(make_type(TypeKind::Char));
		case ExprKind::Unary:
			if (e->op == "&")
			{
				// &var → pointer to var's type
				CTypePtr inner = infer_auto_type(e->lhs);
				return make_pointer(inner);
			}
			if (e->op == "*")
			{
				CTypePtr pt = infer_auto_type(e->lhs);
				if (pt && pt->kind == TypeKind::Pointer) return pt->base;
				return make_type(TypeKind::Int);
			}
			// sizeof / _Alignof 结果 = size_t（LLP64 无符号 64，对齐 codegen int_ctype_of(64,true)）
			if (e->op == "sizeof" || e->op == "alignof")
			{
				auto st = make_type(TypeKind::LongLong);
				st->is_unsigned = true;
				return st;
			}
			if (e->op == "!")
				return make_type(TypeKind::Int);
			// 一元算术 / 位取反 / 自增自减：结果取操作数类型（整型/浮点/指针皆适用）
			if (e->op == "-" || e->op == "+" || e->op == "~" ||
				e->op == "++" || e->op == "--")
			{
				CTypePtr t = infer_auto_type(e->lhs);
				return t ? t : make_type(TypeKind::Int);
			}
			return make_type(TypeKind::Int);
		case ExprKind::Cast:
			return e->type ? e->type : make_type(TypeKind::Int);
		case ExprKind::Call:
			// 函数调用: 查返回类型
			{
				// va_arg(ap, type)：结果类型已在解析时写入 e->type
				if (e->name == "va_arg" && e->type) return e->type;
				auto fit = func_return_types_.find(e->name);
				if (fit != func_return_types_.end()) return fit->second;
			}
			return make_type(TypeKind::Int);
		case ExprKind::VarRef:
			// 查局部变量类型表
			{
				auto lit = local_var_types_.find(e->name);
				if (lit != local_var_types_.end()) return lit->second;
				// 查全局类型
				for (const auto &gv : cur_tu_->globals)
					if (gv.name == e->name) return gv.type;
			}
			return make_type(TypeKind::Int);
		case ExprKind::Binary:
		{
			const std::string &op = e->op;
			// 关系/相等/逻辑与或：结果恒 int
			if (op == "<" || op == ">" || op == "<=" || op == ">=" ||
				op == "==" || op == "!=" || op == "&&" || op == "||")
				return make_type(TypeKind::Int);
			// 数组下标 a[i] / i[a]：结果 = 指针或数组操作数的元素类型
			auto is_ptr_arr = [](const CTypePtr &t) {
				return t && (t->kind == TypeKind::Pointer || t->kind == TypeKind::Array);
			};
			if (op == "[]")
			{
				CTypePtr base = is_ptr_arr(infer_auto_type(e->lhs)) ? infer_auto_type(e->lhs)
				                 : (is_ptr_arr(infer_auto_type(e->rhs)) ? infer_auto_type(e->rhs) : nullptr);
				if (base && base->base) return base->base;
				return make_type(TypeKind::Int);
			}
			// 移位：结果为左操作数类型
			if (op == "<<" || op == ">>")
				return infer_auto_type(e->lhs);
			CTypePtr l = infer_auto_type(e->lhs);
			CTypePtr r = infer_auto_type(e->rhs);
			bool lP = l && l->kind == TypeKind::Pointer;
			bool rP = r && r->kind == TypeKind::Pointer;
			if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%")
			{
				if (op == "+" || op == "-")
				{
					if (lP && !rP) return l;            // ptr + int → ptr
					if (rP && !lP) return r;            // int + ptr → ptr
					if (lP && rP) return make_type(TypeKind::LongLong); // ptr - ptr → ptrdiff_t
				}
				bool anyD = (l && l->kind == TypeKind::Double) || (r && r->kind == TypeKind::Double);
				bool anyF = (l && l->kind == TypeKind::Float) || (r && r->kind == TypeKind::Float);
				if (anyD) return make_type(TypeKind::Double);
				if (anyF) return make_type(TypeKind::Float);
				// 整型：仅两侧同型时精确取之；否则沿用既有“未知→int”近似。
				// （完整整型提升/常用算术转换留待 sema 类型装饰单一真值源收敛，勿在此复制 codegen 格）
				if (l && r && l->kind == r->kind && !lP) return l;
				return make_type(TypeKind::Int);
			}
			if (op == "&" || op == "|" || op == "^")
			{
				if (l && r && l->kind == r->kind && !lP) return l;
				return make_type(TypeKind::Int);
			}
			return make_type(TypeKind::Int);
		}
		default:
			return make_type(TypeKind::Int);
		}
	}

	int64_t const_sizeof_type(const CTypePtr &t)
	{
		if (!t) return 4;
		switch (t->kind)
		{
		case TypeKind::Void: case TypeKind::Bool: case TypeKind::Char: case TypeKind::Char8: return 1;
		case TypeKind::Short: case TypeKind::Char16: return 2;
		case TypeKind::WChar: return target.sizeof_wchar;
		case TypeKind::Int: case TypeKind::Float: case TypeKind::Enum: case TypeKind::Char32: return 4;
		case TypeKind::Long: return target.sizeof_long;
		case TypeKind::LongLong: case TypeKind::Double: return 8;
		case TypeKind::Pointer: case TypeKind::Function: return target.sizeof_pointer; // LLP64/LP64
		case TypeKind::Array:
		{
			int64_t es = const_sizeof_type(t->base);
			return es * static_cast<int64_t>(t->array_size);
		}
		case TypeKind::Struct: case TypeKind::Union:
		{
			// 对齐感知的标准 C 布局（与 codegen 的 LLVM StructType/DataLayout 一致）
			for (const auto &s : cur_tu_->structs)
			{
				if (s.tag != t->tag) continue;
				if (s.is_union)
				{
					int64_t mx = 0, al = 1;
					for (const auto &m : s.members)
					{
						int64_t ms = const_sizeof_type(m.type);
						int64_t ma = const_member_align(m);
						if (ms > mx) mx = ms;
						if (ma > al) al = ma;
					}
					if (static_cast<int64_t>(s.align) > al) al = static_cast<int64_t>(s.align);
					if (static_cast<int64_t>(t->align) > al) al = static_cast<int64_t>(t->align);
					int64_t r = ((mx + al - 1) / al) * al;
					return r > 0 ? r : 4;
				}
				// struct：逐成员对齐填充，尾部补到最大对齐
				int64_t off = 0, al = 1;
				for (const auto &m : s.members)
				{
					int64_t ma = const_member_align(m);
					if (ma < 1) ma = 1;
					if (ma > al) al = ma;
					off = ((off + ma - 1) / ma) * ma + const_sizeof_type(m.type);
				}
				if (static_cast<int64_t>(s.align) > al) al = static_cast<int64_t>(s.align);
				if (static_cast<int64_t>(t->align) > al) al = static_cast<int64_t>(t->align);
				int64_t r = ((off + al - 1) / al) * al;
				return r > 0 ? r : 4;
			}
			return 4; // 未定义 struct/union
		}
		case TypeKind::BitInt:
			return (static_cast<int64_t>(t->bit_width) + 7) / 8;
		default: return 4;
		}
	}
	// struct/union 成员的有效对齐：max(类型自然对齐, 成员 _Alignas)
	int64_t const_member_align(const StructMember &m)
	{
		int64_t a = const_alignof_type(m.type);
		if (static_cast<int64_t>(m.align) > a) a = static_cast<int64_t>(m.align);
		return a < 1 ? 1 : a;
	}

	// 将 _Alignas 强制对齐并入类型节点（用于 typedef/声明）。
	// 仅在确会抬高对齐时浅拷贝 overriding，避免污染共享的 typedef 节点。
	CTypePtr apply_type_align(CTypePtr t, unsigned al)
	{
		if (!t || al == 0 || al <= t->align) return t;
		auto c = std::make_shared<CType>(*t);
		c->align = al;
		return c;
	}

	// —— P250：_Alignas 语境约束（§6.7.2.1p3/p4/p6）——
	// p3/p4：对齐说明符只能用于对象的声明（含 struct/union 成员），不得用于函数、typedef、
	//   形参、位域、register 对象。修前 colang 全静默接受（六类越界均 rc=0），clang 均报错。
	void reject_alignas_only_objects(bool has, const SourceLoc &loc)
	{
		if (!has) return;
		error(loc, "'_Alignas' attribute only applies to variables and fields");
	}
	// 纯类型声明（`_Alignas(16) struct S { int a; };` / `_Alignas(16) int;`）不声明任何对象，
	//   clang 仅 warn "'_Alignas' attribute ignored"（rc=0）——不得按约束违例拒收。
	void warn_alignas_ignored(bool has, const SourceLoc &loc)
	{
		if (!has) return;
		warn(loc, "'_Alignas' attribute ignored");
	}
	void reject_alignas(bool has, const SourceLoc &loc, const char *what)
	{
		if (!has) return;
		error(loc, std::string("'_Alignas' attribute cannot be applied to ") + what);
	}
	// p6：要求的对齐不得弱于类型的自然对齐（如 `_Alignas(1) int`）——属约束违例。
	// P252：文本改为 clang 同形的 `for type 'X'`（修前为 `for this type`，丢了类型拼写）。
	void check_alignas_min(unsigned al, const SourceLoc &loc, const CTypePtr &t)
	{
		if (al == 0 || !t) return;
		const int64_t nat = const_alignof_type(t);
		if (static_cast<int64_t>(al) < nat)
			error(loc, "requested alignment is less than minimum alignment of " +
				std::to_string(nat) + " for type " + describe_type(t));
	}

	// P254：类型拼写 machinery（`alias_add_qual`/`qual_pre`/`qual_suf`/`spell_type_at`/
	//   `spell_params`/`describe_type`）已提为公共头 `colang/type_spell.h`，与 sema 共用。

	int64_t const_alignof_type(const CTypePtr &t)
	{
		if (!t) return 4;
		int64_t natural = 4;
		switch (t->kind)
		{
		case TypeKind::Void: case TypeKind::Bool: case TypeKind::Char: case TypeKind::Char8: natural = 1; break;
		case TypeKind::Short: case TypeKind::Char16: natural = 2; break;
		case TypeKind::WChar: natural = target.sizeof_wchar; break;
		case TypeKind::Int: case TypeKind::Float: case TypeKind::Enum: case TypeKind::Char32: natural = 4; break;
		case TypeKind::Long: natural = target.alignof_long; break;
		case TypeKind::LongLong: case TypeKind::Double: natural = 8; break;
		case TypeKind::Pointer: case TypeKind::Function: natural = target.sizeof_pointer; break;
		case TypeKind::Array: natural = const_alignof_type(t->base); break;
		case TypeKind::BitInt: natural = 1; break; // _BitInt 对齐 = 1
		case TypeKind::Struct: case TypeKind::Union:
		{
			// struct/union 对齐 = 各成员有效对齐的最大值
			int64_t al = 1;
			for (const auto &s : cur_tu_->structs)
				if (s.tag == t->tag)
				{
					for (const auto &m : s.members)
					{
						int64_t ma = const_member_align(m);
						if (ma > al) al = ma;
					}
					if (static_cast<int64_t>(s.align) > al) al = static_cast<int64_t>(s.align);
					break;
				}
			natural = al;
			break;
		}
		default: natural = 4; break;
		}
		// 类型自身携带的 _Alignas（typedef/独立类型形式）并入：取 max
		if (static_cast<int64_t>(t->align) > natural) natural = static_cast<int64_t>(t->align);
		return natural;
	}

	// 判定数组维度是否为整型常量表达式（C99 VLA 判据，§6.5.2.1p1/§6.7.2.1）。
	// 口径与 clang 实测一致：含 VarRef / Call 即非常量——C 里 `const int n = 5; int a[n];`
	// 仍是 VLA（const 限定的变量不是整数常量表达式，与 C++ 不同）。
	// 枚举常量已折成 IntLiteral；sizeof/_Alignof 结果为 size_t 常量（其操作数为 VLA 的
	// 罕见形式不在此细分）；二元/三元/转换递归判定。
	// P256：整数类型判定（含 bool/char/charN_t/wchar_t/short/int/long/long long/enum/位域）。
	//   限定「const int 穿透」只作用于标量整数，避免把 const 指针/结构误当常量。
	static bool is_int_like_type(const CTypePtr &t)
	{
		if (!t) return false;
		switch (t->kind)
		{
		case TypeKind::Bool: case TypeKind::Char: case TypeKind::WChar:
		case TypeKind::Char8: case TypeKind::Char16: case TypeKind::Char32:
		case TypeKind::Short: case TypeKind::Int: case TypeKind::Long:
		case TypeKind::LongLong: case TypeKind::Enum: case TypeKind::BitInt:
			return true;
		default:
			return false;
		}
	}

	// P256：解析局部 const int（内层作用域优先，实现对外层/import const 的遮蔽）。
	bool const_local_value(const std::string &name, std::int64_t &out) const
	{
		for (auto it = const_local_vals_.rbegin(); it != const_local_vals_.rend(); ++it)
		{
			auto f = it->find(name);
			if (f != it->end()) { out = f->second; return true; }
		}
		return false;
	}

	// P256：解析全局/import const int。先本文件文件作用域 const（cur_tu_->globals，此前已 parse
	//   入表者；初值经 eval_const_int 链式求），后 import public const 种子（const_seeds_，
	//   因 merge_imports 前 import 全局尚未并入 cur_tu_->globals，故以种子形式可见）。
	bool const_global_value(const std::string &name, std::int64_t &out)
	{
		if (cur_tu_)
		{
			for (const auto &g : cur_tu_->globals)
			{
				if (g.name != name) continue;
				if (g.is_extern || !g.init) continue;
				if (!g.type || !g.type->is_const || !is_int_like_type(g.type)) continue;
				out = eval_const_int(g.init);
				return true;
			}
		}
		if (const_seeds_)
		{
			for (auto it = const_seeds_->rbegin(); it != const_seeds_->rend(); ++it)
				if (it->name == name) { out = it->value; return true; }
		}
		return false;
	}

	// P256：局部声明若为「const 限定整数且初值可编译期折叠」则登记 名→值 到最内层。note_local_name 后调用。
	void note_local_const(const std::string &name, const CTypePtr &type, const ExprPtr &init)
	{
		if (const_local_vals_.empty()) return;
		if (!type || !type->is_const || !is_int_like_type(type) || !init) return;
		if (!is_const_int_expr(init)) return; // 初值本身须为常量表达式（可再穿透其它 const/enum）
		const_local_vals_.back()[name] = eval_const_int(init);
	}

	bool is_const_int_expr(const ExprPtr &e)
	{
		if (!e) return false;
		switch (e->kind)
		{
		case ExprKind::IntLiteral:
		case ExprKind::CharLiteral:
			return true;
		case ExprKind::VarRef: // P256：引用 const 局部 / const 全局（含 import public const）属常量表达式
		{
			std::int64_t v;
			return const_local_value(e->name, v) || const_global_value(e->name, v);
		}
		case ExprKind::Call:
		case ExprKind::Member:
		case ExprKind::Assign:
		case ExprKind::StringLiteral:
			return false;
		case ExprKind::Unary:
			if (e->op == "sizeof" || e->op == "alignof") return true;
			return is_const_int_expr(e->lhs);
		case ExprKind::Cast:
			// P256：cast 包浮点字面量亦属整数常量表达式（C §6.6 允许算术常量转整数，
			//   §6.3.1.5 向零截断）——`(int)22.22e+2` 作 case 标签合法（CTS 0151_0087）。
			//   若不在此放行，内层 FloatLiteral 被判非恒定→case 诊断闸门误杀。
			return is_const_int_expr(e->lhs)
				|| (e->lhs && e->lhs->kind == ExprKind::FloatLiteral);
		case ExprKind::Binary:
			return is_const_int_expr(e->lhs) && is_const_int_expr(e->rhs);
		case ExprKind::Ternary:
			return is_const_int_expr(e->lhs) && is_const_int_expr(e->args.empty() ? nullptr : e->args[0])
				&& is_const_int_expr(e->args.size() < 2 ? nullptr : e->args[1]);
		default:
			return false;
		}
	}

	int64_t eval_const_int(const ExprPtr &e)
	{
		if (!e) return 0;
		switch (e->kind)
		{
		case ExprKind::IntLiteral:
		case ExprKind::CharLiteral:
			return e->int_val;
		case ExprKind::VarRef: // P256：内层局部 const 优先，再全局/import const；皆非→ 0（由 case 标签诊断闸门接住）
		{
			std::int64_t v;
			if (const_local_value(e->name, v)) return v;
			if (const_global_value(e->name, v)) return v;
			return 0;
		}
		case ExprKind::Cast:
		{
			// 显式类型转换常量：递归求值源值后按目标类型做 C 值转换（§6.3.1.3）。
			// 旧缺本分支 → (char)/(short)/(unsigned) 等 cast case 标签落 default 折成 0，
			// 使 `switch((char)1){case (char)1:...}` 匹配不到（CTS 0150/0054/0149 declarator 矩阵族）。
			// 截断/重解读须与 codegen fold_int_const 一致：按目标位宽 mod 2^N、有符号且最高位
			// 置位则回绕为负；指针/浮点/struct 等非标量目标不截断透传。
			int64_t v = eval_const_int(e->lhs);
			// P256：源为浮点字面量（如 case (int)22.22e+2）：eval_const_int 对 FloatLiteral
			//   落 default 得 0，此处先按 §6.3.1.5 向零截断取整，再走目标类型转换链。
			if (e->lhs && e->lhs->kind == ExprKind::FloatLiteral)
				v = static_cast<int64_t>(e->lhs->float_val);
			if (e->type)
			{
				if (e->type->kind == TypeKind::Bool) return v ? 1 : 0;
				unsigned bits = 0;
				bool uns = e->type->is_unsigned;
				switch (e->type->kind)
				{
				case TypeKind::Char: case TypeKind::Char8: bits = 8; break;
				case TypeKind::Short: case TypeKind::Char16: bits = 16; break;
				case TypeKind::WChar: bits = target.sizeof_wchar * 8; break; // 宽度随目标：Win=16、Linux=32
				case TypeKind::Int: case TypeKind::Long: case TypeKind::Char32: bits = 32; break;
				case TypeKind::LongLong: bits = 64; break;
				case TypeKind::BitInt: bits = e->type->bit_width; break;
				default: bits = 0; break;
				}
				switch (e->type->kind) // wchar_t 符号随目标；char8_t/16/32 恒无符号
				{
				case TypeKind::WChar: uns = !target.wchar_signed; break;
				case TypeKind::Char8: case TypeKind::Char16: case TypeKind::Char32: uns = true; break;
				default: break;
				}
				if (bits > 0 && bits < 64)
				{
					uint64_t m = static_cast<uint64_t>(v) & ((1ull << bits) - 1);
					if (!uns && (m >> (bits - 1)) != 0)
						v = static_cast<int64_t>(m) - static_cast<int64_t>(1ull << bits);
					else
						v = static_cast<int64_t>(m);
				}
			}
			return v;
		}
		case ExprKind::Unary:
		{
			if (e->op == "sizeof")
			{
				// 编译期 sizeof: 简化平台大小 (LLP64 Windows)
				// 类型式 sizeof(T)：parser 已填 e->type。
				if (e->type) return const_sizeof_type(e->type);
				// 表达式式 sizeof(expr)：按操作数表达式的类型求大小。
				// 旧此路仅 BitInt 字面量特判、其余一律 return 4，令 `char s[sizeof(arr)+1]` 等
				// sizeof(数组/结构/指针) 在数组维度/枚举/static_assert 语境误折成 int 大小 4（CTS 0119/0011 暴露）。
				if (e->lhs)
				{
					// sizeof 是数组→指针退化的例外语境（§6.3.2.1）：字符串字面量作操作数时保持数组类型
					//   （char[N]/wchar_t[N]/char32_t[N]，N 含 '\0'），否则 infer_auto_type 给出元素指针→得指针宽 8。
					CTypePtr ot = e->lhs->kind == ExprKind::StringLiteral
						? ctype_of_string_literal(e->lhs->str_val)
						: infer_auto_type(e->lhs);
					if (ot) return const_sizeof_type(ot);
					return 4; // sizeof(expr) 未能定型：默认按 int
				}
				return 4;
			}
			if (e->op == "alignof")
			{
				if (e->type) return const_alignof_type(e->type);
				return 4;
			}
			int64_t v = eval_const_int(e->lhs);
			if (e->op == "-") return -v;
			if (e->op == "!") return !v;
			if (e->op == "~") return ~v;
			return 0;
		}
		case ExprKind::Binary:
		{
			int64_t l = eval_const_int(e->lhs);
			int64_t r = eval_const_int(e->rhs);
			const auto &op = e->op;
			if (op == "+") return l + r;
			if (op == "-") return l - r;
			if (op == "*") return l * r;
			if (op == "/") return r ? l / r : 0;
			if (op == "%") return r ? l % r : 0;
			if (op == "<<") return l << r;
			if (op == ">>") return l >> r;
			if (op == "&") return l & r;
			if (op == "|") return l | r;
			if (op == "^") return l ^ r;
			if (op == "==") return l == r;
			if (op == "!=") return l != r;
			if (op == "<") return l < r;
			if (op == "<=") return l <= r;
			if (op == ">") return l > r;
			if (op == ">=") return l >= r;
			if (op == "&&") return (l && r) ? 1 : 0;
			if (op == "||") return (l || r) ? 1 : 0;
			return 0;
		}
		default:
			return 0;
		}
	}

	// _Static_assert(const_expr [, "msg"]); —— ISO C §6.7.4 静态断言声明（C11 起；C23 起消息可省）。
	// 三处语境共用本函数：文件作用域（parse_global_decl_or_func）、块作用域（parse_stmt）、
	//   struct/union 成员表（parse_decl_specifiers 的成员循环）——三者均不产生 AST 语句/IR，
	//   只在编译期折叠条件并诊断。消息按字面量解码后输出（不带原文双引号，与 clang 同形）。
	bool parse_static_assert_decl()
	{
		const SourceLoc loc = peek().loc;
		advance(); // keyword：_Static_assert / static_assert
		expect_punct("(");
		ExprPtr cond = parse_assign();
		match_punct(",");
		std::string msg;
		if (peek().kind == TokKind::String) msg = co_decode_string(advance().text);
		expect_punct(")");
		expect_punct(";");
		if (!cond) return false; // 条件解析已报错：不再作二次诊断
		if (eval_const_int(cond) == 0)
			error(loc, "static assertion failed" + (msg.empty() ? std::string() : ": " + msg));
		return true;
	}

	bool parse_global_decl_or_func(TranslationUnit &tu)
	{
		SourceLoc loc = peek().loc;
		attr_fatal_ = false; // P214：每项顶层声明入口清空（标志只反映本项）
		// C23 [[attribute]]：前置位识别并记录（P214）
		DeclAttributes attrs;
		parse_attributes(attrs, true);
		if (attr_fatal_) { attr_fatal_ = false; return true; } // 认领本项，保留属性语法诊断
		// _Static_assert(const_expr, "msg");（实现见 parse_static_assert_decl）
		if (check_keyword("_Static_assert") || check_keyword("static_assert"))
		{
			parse_static_assert_decl();
			return true;
		}
		// 存储类说明符（前置消费 + parse_decl_specifiers 期间消费的任意顺序部分）
		// P211 Co 层：允许 public 与 static/extern 任意顺序混用，顶层存储类位。
		bool is_public = match_keyword("public");
		bool is_static = match_keyword("static");
		bool is_extern = match_keyword("extern");
		// 后置顺序的 public（例如 `static public int x;`）在 parse_decl_specifiers 中消费。
		if (match_keyword("public")) is_public = true;
		// typedef 检测
		bool is_typedef = match_keyword("typedef");
		CTypePtr base_type = parse_decl_specifiers(is_public); // P217：顶层 public 下传，回填本层定义的 struct/enum is_public
		is_static = is_static || last_static_;
		is_extern = is_extern || last_extern_;
		is_typedef = is_typedef || last_typedef_;
		is_public = is_public || last_public_;
		unsigned ga = last_alignas_; // 本声明的 _Alignas（需在任何后续递归解析前捕获）
		const bool ga_has = last_has_alignas_;   // P250：_Alignas 是否出现（含 _Alignas(0)）
		const SourceLoc ga_loc = last_alignas_loc_; // P250：诊断定位（递归解析会覆盖成员值）
		// P214：中置位（类型说明符之前/之间）识别到的属性与前置位归并；重复项报错
		merge_attributes(attrs, last_attrs_, loc);
		last_attrs_ = DeclAttributes();
		if (attr_fatal_) { attr_fatal_ = false; return true; } // 中置位属性语法错：同样认领本项
		if (!base_type) return false;
		// 纯类型定义（struct/enum 无变量名）: 后面跟 ';'
		if (check_punct(";")) { advance(); warn_alignas_ignored(ga_has, ga_loc); return true; } // P250
		// —— typedef：各声明符经统一 Decorator（对象/数组/函数指针皆然）——
		if (is_typedef)
		{
			reject_alignas_only_objects(ga_has, ga_loc); // P250：typedef 不得带 _Alignas（GNU aligned 不属此拒收）
			// 位点1/4 前置 GNU aligned 提升值（须在声明符解析前捕获，函数指针 typedef 的参数表会清零通道）。
			const unsigned tgnu = gnu_decl_align_;
			do
			{
				std::string nm;
				DeclaratorFn d = parse_declarator_rec(nm);
				// 位点后置：typedef 名尾 GNU __attribute__（`typedef int I __attribute__((aligned(16)));`）。
				//   aligned 施加于别名类型（不入 ga、不逆 P250 reject，与位点3 struct-typedef-aligned 同口径）；余忽略告警。
				const unsigned tp = consume_gnu_attributes();
				unsigned tal = ga; if (tgnu > tal) tal = tgnu; if (tp > tal) tal = tp;
				typedefs_[nm] = apply_type_align(d(base_type), tal);
			} while (match_punct(","));
			match_punct(";");
			return true;
		}
		// —— 函数声明/定义（保留具名参数与函数体）：(*[qual] Identifier 后紧跟 '(' ——
		if (is_function_form())
		{
			reject_alignas_only_objects(ga_has, ga_loc); // P250：函数不得带 _Alignas
			CTypePtr dtype = parse_pointer_suffix(base_type);
			std::string name = expect_identifier().text;
			FuncDecl fn;
			fn.loc = loc;
			fn.name = std::move(name);
			fn.return_type = std::move(dtype);
			fn.is_static = is_static;
			fn.is_public = is_public;
			fn.is_deprecated = attrs.deprecated;
			fn.deprecated_msg = attrs.deprecated_msg;
			fn.is_nodiscard = attrs.nodiscard;      // P249
			fn.nodiscard_msg = attrs.nodiscard_msg;
			fn.is_noreturn = attrs.noreturn_attr;   // P249
			fn.lib_name = attrs.lib_name;
			fn.subsystem_windows = attrs.subsystem_windows;
			expect_punct("(");
			parse_param_list(fn.params, fn.is_variadic);
			expect_punct(")");
			// 位点4：函数声明符尾部 GNU __attribute__（`void foo(void) __attribute__((stdcall));`）。
			consume_gnu_attributes();
			if (check_punct("{"))
			{
				pending_param_names_.clear();
				for (const auto &p : fn.params) pending_param_names_.push_back(p.name);
				fn.body = parse_compound_stmt();
			}
			else if (!check_punct(","))
				match_punct(";");
			func_return_types_[fn.name] = fn.return_type;
			tu.funcs.push_back(std::move(fn));
			// 多声明符续接（如 int f(int a), g(int a), a;）：其余声明符经 Decorator
			while (match_punct(","))
				parse_global_declarator_item(tu, base_type, is_static, is_extern, ga, loc, is_public, attrs, ga_has, ga_loc);
			match_punct(";");
			return true;
		}
		// —— 对象/分组/函数指针对象声明符（可多个，逗号分隔）——
		do
		{
			parse_global_declarator_item(tu, base_type, is_static, is_extern, ga, loc, is_public, attrs, ga_has, ga_loc);
		} while (match_punct(","));
		match_punct(";");
		return true;
	}

	CTypePtr parse_decl_specifiers(bool top_public = false)
	{
		// 声明说明符可任意顺序出现（C §6.7.1p1：如 `volatile static int` / `struct S static x`）。
		// 存储类在此统一消费并记入 last_*_，由调用方与前置已消费的存储类合并。
		last_static_ = false;
		last_extern_ = false;
		last_typedef_ = false;
		last_register_ = false;
		last_public_ = false;
		last_has_alignas_ = false; // P250：入口清零（_Alignas 出现标志，含 _Alignas(0)）
		gnu_decl_align_ = 0;       // GNU aligned 提升通道入口清零（独立于 _Alignas）
		last_attrs_ = DeclAttributes(); // P214：中置属性位同样需在入口清空（防递归/上一声明残留）
		bool cnst_front = false;
		bool vol_front = false; // P253：volatile 修前被整组丢弃（`volatile int` 丢限定）
		bool ato_front = false; // P253：`_Atomic` 同上，仅拼写用（不引入原子语义）
		// register：C23 仍保留该关键字（存储类），但不保证真实寄存器绑定；按 clang 默认作 no-op 消费
		// P250：抽成 lambda——存储类/限定符与 _Alignas 可任意交错（`_Alignas(16) register int r;`），
		//   故在 _Alignas 前缀消费前后各扫一遍，否则后者会把 register 漏记为未出现。
		auto consume_sc_quals = [&]() {
			for (;;)
			{
				if (match_keyword("static")) { last_static_ = true; continue; }
				if (match_keyword("extern")) { last_extern_ = true; continue; }
				if (match_keyword("typedef")) { last_typedef_ = true; continue; }
				if (match_keyword("public")) { last_public_ = true; continue; }
				if (match_keyword("register")) { last_register_ = true; continue; }
				if (match_keyword("inline") || match_keyword("restrict") ||
					match_keyword("_Inline") || match_keyword("_Noreturn") || match_keyword("_Thread_local") ||
					match_keyword("thread_local"))
					continue;
				// P253：volatile/_Atomic 不再当 no-op，归入与 const 同一套限定符建模
				if (match_keyword("volatile")) { vol_front = true; continue; }
				if (match_keyword("_Atomic")) { ato_front = true; continue; }
				if (match_keyword("const") || match_keyword("constexpr")) { cnst_front = true; continue; }
				break;
			}
		};
		consume_sc_quals();
		// 位点1：GNU __attribute__ 前缀（非 ISO C，GCC/Clang 兼容；见文件顶 GNU 通道注释）。
		//   可与存储类/限定符任意交错：前置组在此消费，其后若仍跟存储类，由 _Alignas 循环后的
		//   consume_sc_quals 兜住。aligned 走**独立提升通道**（gnu_decl_align_）：仅在各对象/成员/
		//   typedef 落点取 max 抬升对齐，绝不并入 _Alignas 的 acc/last_has_alignas_——因 clang 对 GNU
		//   __attribute__((aligned)) 不套用 _Alignas 的约束拒收（函数/位域/低于自然对齐均接受）。
		const unsigned gnu_prefix = consume_gnu_attributes();
		if (gnu_prefix > gnu_decl_align_) gnu_decl_align_ = gnu_prefix;
		const bool sc_static = last_static_, sc_extern = last_extern_, sc_typedef = last_typedef_;
		// C23 _Alignas(...)：§6.7.2.1p6 多个取最大；类型形式取 _Alignof，常量形式取该常量。
		// 前缀在此消费，避免后续类型解析（struct/typeof 递归）覆盖本声明的对齐值。
		unsigned acc = 0;
		for (;;)
		{
			if (!check_keyword("_Alignas") && !check_keyword("alignas")) break;
			// P252：诊断位置取 `_Alignas` 关键字本体——clang 的「less than minimum alignment」与
			//   「redeclaration has different alignment requirement」均指此列（修前取 '(' 列、偏 +8）。
			last_alignas_loc_ = peek().loc;
			advance(); // _Alignas / alignas
			last_has_alignas_ = true;
			expect_punct("(");
			int64_t a = 0;
			if (is_type_start())
			{
				// _Alignas(type-name)：取该类型的对齐（含 struct 成员对齐感知）
				CTypePtr at = parse_decl_specifiers_core();
				at = parse_pointer_suffix(at);
				if (check_punct("["))
					at = parse_array_suffix(at);
				a = const_alignof_type(at);
			}
			else
			{
				// _Alignas(constant-expression)
				ExprPtr ce = parse_assign();
				a = eval_const_int(ce);
			}
			expect_punct(")");
			if (a < 0 || (a != 0 && (a & (a - 1)) != 0))
				error(peek().loc, "_Alignas argument must be a power of two or zero");
			else if (static_cast<unsigned>(a) > acc)
				acc = static_cast<unsigned>(a);
		}
		consume_sc_quals(); // P250：_Alignas 前缀之后可能还跟着存储类/限定符
		// P250：core 会就地解析 struct/union 体，体内成员的 _Alignas 会改写下面两个成员；
		//   先快照本声明自身的前缀状态、core 返回后还原，否则成员的「出现过 _Alignas」
		//   会泄漏到外层（`struct B { _Alignas(16) int x:3; };` 除位域诊断外又多报一条
		//   「only applies to variables and fields」）。
		const bool own_has_alignas = last_has_alignas_;
		const SourceLoc own_alignas_loc = last_alignas_loc_;
		const unsigned own_gnu_align = gnu_decl_align_;
		const SourceLoc own_gnu_loc = gnu_alignas_loc_;
		CTypePtr r = parse_decl_specifiers_core(top_public);
		last_has_alignas_ = own_has_alignas;
		last_alignas_loc_ = own_alignas_loc;
		gnu_decl_align_ = own_gnu_align;
		gnu_alignas_loc_ = own_gnu_loc;
		if (cnst_front && r)
			r->is_const = true;
		// P253：volatile/_Atomic 与 const 同一回填口径（clang 将 `int volatile x` 与 `volatile int x` 归一）。
		//   别名节点同步并入拼写串，令 `const MyInt`/`const volatile SS` 形可还原。
		if (vol_front && r)
			r->is_volatile = true;
		if (ato_front && r)
			r->is_atomic = true;
		if (r && (cnst_front || vol_front))
		{
			if (cnst_front) alias_add_qual(r->alias, false);
			if (vol_front) alias_add_qual(r->alias, true);
		}
		// C23 §6.7：限定符/存储类可出现在类型说明符之后（int const x / struct S static s2）。
		//   指针限定符（int * const p）的 const 在 * 之后、不在此路径，仍由 parse_declarator_rec/parse_pointer_suffix 处理。
		for (;;)
		{
			if (match_keyword("const"))
			{
				if (r) { r->is_const = true; alias_add_qual(r->alias, false); }
				continue;
			}
			// P253：volatile/_Atomic/restrict 修前被整组丢弃，现与 const 同一套建模（仅拼写用）
			if (match_keyword("volatile"))
			{
				if (r) { r->is_volatile = true; alias_add_qual(r->alias, true); }
				continue;
			}
			if (match_keyword("_Atomic")) { if (r) r->is_atomic = true; continue; }
			if (match_keyword("restrict")) { if (r) r->is_restrict = true; continue; }
			if (match_keyword("static")) { last_static_ = true; continue; }
			if (match_keyword("extern")) { last_extern_ = true; continue; }
			if (match_keyword("typedef")) { last_typedef_ = true; continue; }
			// 位点4：GNU __attribute__ 后置（类型说明符与声明符之间，如
			//   `void __attribute__((stdcall)) f(void)`；非 ISO C，GCC/Clang 兼容）。
			if (check_keyword("__attribute__"))
			{
				const unsigned g2 = consume_gnu_attributes();
				if (g2 > gnu_decl_align_) gnu_decl_align_ = g2;
				continue;
			}
			break;
		}
		last_static_ = last_static_ || sc_static;
		last_extern_ = last_extern_ || sc_extern;
		last_typedef_ = last_typedef_ || sc_typedef;
		last_alignas_ = acc; // 本声明的 _Alignas（覆盖递归期间对 last_alignas_ 的改写）
		return r;
	}

	// 类型说明符组合允许任意顺序（C §6.7.2p2，如 `int long unsigned`）。
	// 既有实现按 canonical 顺序线性匹配；此处在进入 core 前探测连续类型关键字个数，
	// ≥2 则整体收集后归一化，否则仍走原线性分支（单关键字组合行为不变）。
	int type_combo_len() const
	{
		int n = 0;
		for (std::size_t i = 0; i < 8; ++i)
		{
			const Token &t = peek(i);
			if (t.kind != TokKind::Keyword && t.kind != TokKind::Identifier)
				break;
			const std::string &s = t.text;
			if (s == "const" || s == "volatile" || s == "restrict" || s == "_Atomic" ||
				s == "register" || s == "inline")
				continue;
			if (s == "void" || s == "_Bool" || s == "bool" || s == "char" || s == "short" ||
				s == "int" || s == "long" || s == "float" || s == "double" ||
				s == "signed" || s == "unsigned")
			{
				++n;
				continue;
			}
			break;
		}
		return n;
	}

	CTypePtr parse_type_specifier_combo(bool cnst, bool vol = false, bool ato = false)
	{
		bool saw_void = false, saw_bool = false, saw_char = false, saw_short = false;
		bool saw_int = false, saw_float = false, saw_double = false;
		bool saw_signed = false, saw_unsigned = false;
		int long_count = 0;
		for (;;)
		{
			if (match_keyword("void")) saw_void = true;
			else if (match_keyword("_Bool") || match_keyword("bool")) saw_bool = true;
			else if (match_keyword("char")) saw_char = true;
			else if (match_keyword("short")) saw_short = true;
			else if (match_keyword("int")) saw_int = true;
			else if (match_keyword("long")) ++long_count;
			else if (match_keyword("float")) saw_float = true;
			else if (match_keyword("double")) saw_double = true;
			else if (match_keyword("signed")) saw_signed = true;
			else if (match_keyword("unsigned")) saw_unsigned = true;
			else if (match_keyword("const")) cnst = true;
			else if (match_keyword("volatile")) vol = true; // P253：类型关键字之间插写的限定符同样建模（仅拼写用）
			else if (match_keyword("_Atomic")) ato = true;
			else if (match_keyword("restrict") ||
				match_keyword("register") || match_keyword("inline"))
				;
			else break;
		}
		// 归一化（§6.7.2p2 组合表）：先定底层类型，再叠加符号
		TypeKind k = TypeKind::Int;
		if (saw_void) k = TypeKind::Void;
		else if (saw_bool) k = TypeKind::Bool;
		else if (saw_char) k = TypeKind::Char;
		else if (long_count >= 2) k = TypeKind::LongLong;
		else if (saw_double) k = TypeKind::Double;
		else if (long_count == 1) k = TypeKind::Long;
		else if (saw_float) k = TypeKind::Float;
		else if (saw_short) k = TypeKind::Short;
		else if (saw_int) k = TypeKind::Int;
		auto t = make_type(k, cnst);
		t->is_volatile = vol;
		t->is_atomic = ato;
		if (saw_unsigned) t->is_unsigned = true;
		else if (saw_signed) t->is_unsigned = false;
		return t;
	}

	CTypePtr parse_decl_specifiers_core(bool top_public = false)
	{
		last_auto_ = false;
		// 跳过不影响类型的存储类/属性关键字（含 register no-op，见 P141）
		while (match_keyword("register") ||
			match_keyword("inline") || match_keyword("restrict") || match_keyword("_Inline") ||
			match_keyword("_Noreturn") || match_keyword("_Thread_local") ||
			match_keyword("thread_local") || match_keyword("_Atomic"))
			;
		// C23 [[attribute]]：中置位识别并记录（不影响类型解析）
		parse_attributes(last_attrs_, true);
		bool cnst = false;
		bool vol = false, ato = false; // P253：core 入口可达的两条直达路径（_Alignas(类型名)/auto 递归）
		// volatile/_Atomic 仅为限定符：不改变类型/布局，现保留供诊断拼写还原（P253）。
		for (;;)
		{
			if (match_keyword("const") || match_keyword("constexpr")) { cnst = true; continue; }
			if (match_keyword("volatile")) { vol = true; continue; }
			if (match_keyword("_Atomic")) { ato = true; continue; }
			break;
		}
		if (type_combo_len() >= 2)
			return parse_type_specifier_combo(cnst, vol, ato);
		if (match_keyword("void"))
			return make_type(TypeKind::Void, cnst);
		if (match_keyword("auto"))
		{
			// C23 auto 类型推断仅在 auto 后不再跟类型说明符时启用；
			// 旧式 C 的 auto 是存储类（如 `auto volatile const char a1;`），此时按 no-op 继续解析类型。
			if (!is_type_start())
			{
				last_auto_ = true; // C23 auto: 类型由初始化式推断
				return nullptr;
			}
			CTypePtr t = parse_decl_specifiers_core();
			if (t && cnst)
				t->is_const = true;
			if (t && vol) t->is_volatile = true; // P253：与 const 同步下传
			if (t && ato) t->is_atomic = true;
			return t;
		}
		// C23 typeof(expr) / typeof(type) / typeof_unqual(type)
		if (check_keyword("typeof") || check_keyword("typeof_unqual"))
		{
			// C23: typeof_unqual 与 typeof 同解析路径，仅额外剥离顶层限定符
			bool unqual = (peek().text == "typeof_unqual");
			advance(); // consume keyword
			expect_punct("(");
			CTypePtr result;
			// 此时当前 token 是括号内第一个: 若是类型关键字则为 typeof(type)
			if (is_type_start())
			{
				// typeof(type)
				result = parse_decl_specifiers();
				result = parse_pointer_suffix(result);
				if (check_punct("["))
					result = parse_array_suffix(result);
			}
			else
			{
				// typeof(expr) —— 解析表达式并推断类型
				ExprPtr inner = parse_assign();
				// typeof 取**表达式自身的类型**、不施加左值转换（与 sizeof 同属 §6.3.2.1 的退化例外）：
				//   字符串字面量应得 char[N]/wchar_t[N]/char32_t[N]（N 含 '\0'），而非元素指针。
				//   `auto` 推导相反——C23 auto 经左值转换、字面量得元素指针（实测 clang = sizeof 8），
				//   故仅在此处对字面量取不衰变口径，`auto` 路径（infer_auto_type）保持不变。
				result = (inner && inner->kind == ExprKind::StringLiteral)
					? ctype_of_string_literal(inner->str_val)
					: infer_auto_type(inner);
			}
			expect_punct(")");
			// typeof_unqual: 剥离顶层限定符（P253：colang 已建模 volatile/_Atomic，一并剥）
			if (unqual && result)
			{
				result->is_const = false;
				result->is_volatile = false;
				result->is_atomic = false;
			}
			return result;
		}
		if (match_keyword("_Bool") || match_keyword("bool"))
			return make_type(TypeKind::Bool, cnst);
		if (match_keyword("char"))
			return make_type(TypeKind::Char, cnst);
		if (match_keyword("wchar_t"))
			return make_type(TypeKind::WChar, cnst);  // 宽度/符号性随 target 分派（Win=i16/unsigned、Linux=i32/signed）；AST 层不置 is_unsigned（由 codegen 读 target 判）
		// charN_t：恒无符号的独立整型（char8_t=C23/1B、char16_t=C11/2B、char32_t=C11/4B）；
		//   不可与 signed/unsigned/short/long 复合，故直接 return。
		if (match_keyword("char8_t"))  { auto t = make_type(TypeKind::Char8,  cnst); t->is_unsigned = true; return t; }
		if (match_keyword("char16_t")) { auto t = make_type(TypeKind::Char16, cnst); t->is_unsigned = true; return t; }
		if (match_keyword("char32_t")) { auto t = make_type(TypeKind::Char32, cnst); t->is_unsigned = true; return t; }
		if (match_keyword("short"))
		{
			match_keyword("int");  // 可选尾随 int（short int）
			return make_type(TypeKind::Short, cnst);  // short: i16（2 字节，有符号）
		}
		if (match_keyword("int"))
			return make_type(TypeKind::Int, cnst);
		if (match_keyword("long"))
		{
			// long long [int] → LongLong（尾随 int 可选，对齐 signed/unsigned long long int 分支）
			if (match_keyword("long"))
			{
				match_keyword("int");
				return make_type(TypeKind::LongLong, cnst);
			}
			// long double → Double（本机 Windows x64 long double == double，8 字节，对齐 clang -std=c23；修 plct 00204）
			if (match_keyword("double"))
				return make_type(TypeKind::Double, cnst);
			// optional 'int' after long
			match_keyword("int");
			return make_type(TypeKind::Long, cnst);
		}
		if (match_keyword("unsigned"))
		{
			// unsigned _BitInt(N)
			if (check_keyword("_BitInt"))
			{
				auto t = parse_bitint();
				t->is_unsigned = true;
				return t;
			}
			if (match_keyword("char")) { auto t = make_type(TypeKind::Char, cnst); t->is_unsigned = true; return t; }
			if (match_keyword("short")) { match_keyword("int"); auto t = make_type(TypeKind::Short, cnst); t->is_unsigned = true; return t; }
			if (match_keyword("long"))
			{
				if (match_keyword("long")) { match_keyword("int"); auto t = make_type(TypeKind::LongLong, cnst); t->is_unsigned = true; return t; }
				match_keyword("int");
				auto t = make_type(TypeKind::Long, cnst); t->is_unsigned = true; return t;
			}
			match_keyword("int");
			{ auto t = make_type(TypeKind::Int, cnst); t->is_unsigned = true; return t; }
		}
		if (match_keyword("signed"))
		{
			// signed _BitInt(N)
			if (check_keyword("_BitInt"))
				return parse_bitint();
			if (match_keyword("char")) return make_type(TypeKind::Char, cnst);
			if (match_keyword("short")) { match_keyword("int"); return make_type(TypeKind::Short, cnst); }
			if (match_keyword("long"))
			{
				if (match_keyword("long")) { match_keyword("int"); return make_type(TypeKind::LongLong, cnst); }
				match_keyword("int");
				return make_type(TypeKind::Long, cnst);
			}
			match_keyword("int");
			return make_type(TypeKind::Int, cnst);
		}
		// _BitInt(N) 独立出现（默认 signed）
		if (check_keyword("_BitInt"))
			return parse_bitint();
		if (match_keyword("float"))
			return make_type(TypeKind::Float, cnst);
		if (match_keyword("double"))
			return make_type(TypeKind::Double, cnst);
		// struct / union
		if (check_keyword("struct") || check_keyword("union"))
		{
			bool is_union = check_keyword("union");
			advance(); // consume 'struct'/'union'
			// 位点2：GNU __attribute__ 于 struct/union 关键字与标签名之间（`union __attribute__((packed)) U {`）。
			unsigned sd_align = consume_gnu_attributes();
			std::string tag;
			if (peek().kind == TokKind::Identifier)
				tag = advance().text;
			// 位点2：标签名与 '{' 之间（`struct __attribute__((aligned(16))) S {`）。
			if (auto g = consume_gnu_attributes(); g > sd_align) sd_align = g;
			if (check_punct("{"))
			{
				// 完整定义
				advance(); // consume '{'
				StructDef sd;
				// 先定 tag 并登记前置声明，成员中的自引用才能命中同一 tag
				tag = begin_definition_tag(tag, is_union);
				sd.tag = tag;
				sd.is_union = is_union;
				sd.is_definition = true; // 带 { ... }：空成员表也是完整（空）类型，区别于前置引用
				sd.loc = peek().loc;
				while (!check_punct("}") && !at_end())
				{
					// struct 内静态断言（§6.7.2.1 允许 static_assert-declaration 作成员声明）：不占成员槽
					if (check_keyword("_Static_assert") || check_keyword("static_assert"))
					{
						parse_static_assert_decl();
						continue;
					}
					CTypePtr mtype = parse_decl_specifiers();
					const SourceLoc mem_decl_loc = peek().loc; // 成员声明类型起始（备用）
					unsigned mal = last_alignas_; // 本成员的 _Alignas（后续声明符解析前捕获）
					const bool mal_has = last_has_alignas_;    // P250
					const SourceLoc mal_loc = last_alignas_loc_; // P250
					// 位点1 前置 GNU aligned 走独立 gnu_decl_align_ 通道（不并入 mal——mal 供 _Alignas 专用
					//   check_alignas_min/reject，GNU 不受该约束）；仅在下方落点取 max 抬 m.align（纯提升）。
					const unsigned mg = gnu_decl_align_;
					if (!mtype) { skip_to_semi_or_brace(); continue; }
					// 匿名 struct/union 成员: type是struct/union且紧跟';'（无成员名）
					if ((mtype->kind == TypeKind::Struct || mtype->kind == TypeKind::Union)
						&& check_punct(";"))
					{
						// 匿名成员宿主为 union（is_union）、或自身是 union：一律保留嵌套、不展开。
						//   union 语义要求各成员偏移恒 0，平铺内层字段会把它们各自变成独立 union
						//   成员、抹平内层 struct 的偏移（LARGE_INTEGER.HighPart 落到 LowPart，P235）；
						//   mtype 已带合成 tag `__anon_union_N`/`__anon_struct_N`，内层布局由
						//   get_struct_type 按该 tag 建型，访问/初始化按名提升递归解析。
						// 仅「struct 宿主的匿名 struct」沿用展开（布局等价，省一层）。
						// 提升名的衍生用法均按上述递归正确处理、无遗留限制（实测与 clang 逐字节一致）：
						//   指定符 `.名`（P236）、`offsetof` 与内层位域提升成员（P235）、
						//   `_Alignas` 叠加匿名聚合按成员/宿主对齐正常布局。
						if (is_union || mtype->kind == TypeKind::Union)
						{
							StructMember m;
							m.name = ""; // 匿名：成员名由 sema/codegen 名提升递归解析
							m.type = mtype;
							m.align = last_alignas_ > mg ? last_alignas_ : mg;
							m.loc = mem_decl_loc;
							sd.members.push_back(std::move(m));
							match_punct(";");
							continue;
						}
						// 匿名 struct：展开内部成员（布局/初始化/访问与嵌等价）
						const std::string &inner_tag = mtype->tag;
						const StructDef *inner_sd = nullptr;
						for (const auto &s : cur_tu_->structs)
						{
							if (inner_tag.empty())
							{
								// 最近定义的匿名 struct
								if (!s.tag.empty() || &s == &cur_tu_->structs.back())
									{ inner_sd = &s; break; }
							}
							else if (s.tag == inner_tag) { inner_sd = &s; break; }
						}
						// 匿名: 取最后定义的
						if (!inner_sd && !cur_tu_->structs.empty())
							inner_sd = &cur_tu_->structs.back();
						if (inner_sd)
							for (const auto &im : inner_sd->members)
								sd.members.push_back(im);
						match_punct(";");
						continue;
					}
					do {
						// 每个声明符都从声明说明符给出的基础类型重新包裹指针/数组。
						// 旧写法在循环内改写 mtype：第二个及以后的成员声明符会叠加成
						// 「指针的指针」，且合成指针类型的 struct tag 为空 → 成员访问按空 tag
						// 造出 opaque %struct.（GEP into unsized type，CTS 0011/0151 族）。
						CTypePtr dtype = mtype;
						// 函数指针成员: int (*fp)(int, int)
						if (is_funcptr_declarator())
						{
							auto [fp_name, fp_type] = parse_funcptr_declarator(dtype);
							StructMember m;
							m.name = fp_name;
							m.type = fp_type;
							m.align = mal > mg ? mal : mg;
							m.loc = mem_decl_loc;
							// 位点后置：函数指针成员名后 GNU __attribute__（aligned 仅抬 m.align；余忽略告警）。
							{ const unsigned mp = consume_gnu_attributes(); if (mp > m.align) m.align = mp; }
							sd.members.push_back(std::move(m));
							break; // funcptr 不支持逗号分隔多个声明符
						}
						dtype = parse_pointer_suffix(dtype);
						StructMember m;
						if (!check_punct(":")) // 匿名位域（type : width，无成员名，§6.7.2.1p5）
						{ const Token &id = expect_identifier(); m.name = id.text; m.loc = id.loc; }
						else m.loc = mem_decl_loc;
						m.type = parse_array_suffix(dtype);
						m.align = mal > mg ? mal : mg;
						// 位点后置：成员名后 GNU __attribute__((aligned)) 仅抬 m.align（纯提升），绝不并入 mal——
						//   mal 供下方 _Alignas 专用 check_alignas_min/reject，GNU 不受该约束（clang 对 GNU 皆接受）。
						{ const unsigned mp = consume_gnu_attributes(); if (mp > m.align) m.align = mp; }
						if (is_void_object_type(m.type))
							error(peek().loc, "field has incomplete type 'void'");
						// 位域: name : width
						if (check_punct(":"))
						{
							reject_alignas(mal_has, mal_loc, "a bit-field"); // P250：位域不得带 _Alignas
							advance(); // ':'
							ExprPtr w = parse_assign();
							m.bit_width = static_cast<int>(eval_const_int(w));
						}
						else
							check_alignas_min(mal, mal_loc, m.type); // P250：非位域成员同样不得弱于自然对齐
						sd.members.push_back(std::move(m));
					} while (match_punct(","));
					match_punct(";");
				}
				expect_punct("}");
				// 位点3：闭 '}' 之后、typedef/变量名之前（`struct S { ... } __attribute__((packed)) x;`）。
				if (auto g = consume_gnu_attributes(); g > sd_align) sd_align = g;
				// 匿名 struct/union: 分配合成 tag
				if (tag.empty() && !sd.members.empty())
				{
					tag = std::string("__anon_") + (is_union ? "union_" : "struct_")
						+ std::to_string(cur_tu_->structs.size());
					sd.tag = tag;
				}
				// 块作用域内重定义同名 tag 已在解析成员前改名为唯一 tag（见 begin_definition_tag）
				sd.tag = tag;
				// P217：顶层 public 回填——仅最外层被定义的 struct 继承 public（成员循环等内部调用保持默认 false）
				sd.is_public = sd.is_public || top_public;
				// GNU __attribute__((aligned(N))) 于 struct/union 类型（位点2/3）：登进 StructDef，
				//   供后续按 tag 引用（`struct B`）时 const_sizeof/const_alignof 与 codegen 布局一致采用。
				sd.align = sd_align;
				// 前置声明已由 begin_definition_tag 登记：原地补全，避免同一 tag 出现两条定义，
				// 使按 tag 取首个匹配的成员查找（offsetof/sizeof/成员访问）读到空壳。
				bool completed_in_place = false;
				if (!tag.empty())
				{
					auto it = struct_tags_.find(tag);
					if (it != struct_tags_.end())
					{
						cur_tu_->structs[it->second] = std::move(sd);
						completed_in_place = true;
					}
				}
				if (!completed_in_place)
				{
					cur_tu_->structs.push_back(std::move(sd));
					if (!tag.empty())
						struct_tags_[tag] = cur_tu_->structs.size() - 1;
				}
			}
			else
			{
				// 引用已有 tag（未定义也先记录）；先按块作用域解析到实际 tag。
				// 本块内首次出现且外层无同名可见 tag → 属本块的 tag 声明，记入本层映射（取唯一内部名），
				// 后续同块定义据此沿用同一 tag（如 typedef struct tnode TNODE; 后随定义）。
				std::string resolved = resolve_tag(tag);
				if (block_depth_ > 0 && !tag_scope_.empty() &&
					!tag_scope_.back().count(tag) && resolved == tag && !struct_tags_.count(tag))
				{
					resolved = fresh_block_tag(tag);
					tag_scope_.back()[tag] = resolved;
				}
				tag = resolved;
				if (!struct_tags_.count(tag))
				{
					StructDef fwd;
					fwd.tag = tag;
					fwd.is_union = is_union;
					cur_tu_->structs.push_back(std::move(fwd));
					struct_tags_[tag] = cur_tu_->structs.size() - 1;
				}
			}
			auto st = std::make_shared<CType>();
			st->kind = is_union ? TypeKind::Union : TypeKind::Struct;
			st->tag = tag;
			st->is_const = cnst;
			// aligned 直接抬升聚合类型对齐（独立于 _Alignas acc 链：GNU aligned 于 typedef 合法且有意义）
			if (sd_align && sd_align > st->align) st->align = sd_align;
			return st;
		}
		// enum
		if (check_keyword("enum"))
		{
			advance();
			std::string tag;
			if (peek().kind == TokKind::Identifier)
				tag = advance().text;
			if (!tag.empty()) enum_tag_names_.insert(tag); // P232：登记 enum 标签名（含前向声明），供裸标签作类型
			if (check_punct("{"))
			{
				advance();
				EnumDef ed;
				ed.tag = tag;
				ed.loc = peek().loc;
				std::int64_t next_val = 0;
				while (!check_punct("}") && !at_end())
				{
					EnumConstant ec;
					ec.name = expect_identifier().text;
					if (match_punct("="))
						next_val = eval_const_int(parse_assign()); // 一般常量表达式枚举值（含 0x/0b/0o/offsetof/sizeof/算术）
					ec.value = next_val++;
					enum_constants_[ec.name] = ec.value;
					ed.constants.push_back(std::move(ec));
					if (!match_punct(",")) break;
				}
				expect_punct("}");
				// P217：顶层 public 回填——仅最外层被定义的 enum 继承 public
				ed.is_public = ed.is_public || top_public;
				cur_tu_->enums.push_back(std::move(ed));
			}
			auto et = std::make_shared<CType>();
			et->kind = TypeKind::Enum;
			et->tag = tag;
			et->is_const = cnst;
			return et;
		}
		// typedef 名解析
		if (peek().kind == TokKind::Identifier)
		{
			auto it = typedefs_.find(peek().text);
			if (it != typedefs_.end())
			{
				advance();
				// P253：返回**克隆**并登记使用点别名。
				//   修前直接返回 `typedefs_` 里的共享节点，使用点回填的限定符（本函数调用方的
				//   `r->is_const = true`）会写进 typedef 本体、污染其后所有使用点——实测既让诊断
				//   拼写出错（`const MyInt` 之后的 `MyInt *` 被打成 `const int *`），又真确改变
				//   `_Generic` 关联结果（同一段码仅调换声明顺序即选错分支，clang 恒取正确分支）。
				//   克隆同时为拼写提供别名一名（clang：`'MyInt' (aka 'int')`；多级别名只保留书写处
				//   一名、aka 直达底层类型，如 `typedef int A1; typedef A1 B2;` → `'B2' (aka 'int')`）。
				auto t = std::make_shared<CType>(*it->second);
				t->alias = it->first;
				if (cnst)
				{
					t->is_const = true; // 修前此处的使用点 const 被丢（core 直达路径上）
					alias_add_qual(t->alias, false);
				}
				if (vol) t->is_volatile = true;
				if (ato) t->is_atomic = true;
				return t;
			}
			// P232 Co 层：省去 struct/union/enum 关键字——裸标签名解析为其类型（仅 .co）。
			//   命名冲突时 typedef 优先（上方已先查）；标签名按定义处登记的内部 tag 取回。
			if (co_mode_)
			{
				const std::string nm = peek().text;
				auto sit = struct_tags_.find(nm);
				if (sit != struct_tags_.end())
				{
					advance();
					const StructDef &sd = cur_tu_->structs[sit->second];
					auto st = std::make_shared<CType>();
					st->kind = sd.is_union ? TypeKind::Union : TypeKind::Struct;
					st->tag = sd.tag;
					st->is_const = cnst;
					return st;
				}
				if (enum_tag_names_.count(nm))
				{
					advance();
					auto et = std::make_shared<CType>();
					et->kind = TypeKind::Enum;
					et->tag = nm;
					et->is_const = cnst;
					return et;
				}
			}
		}
		error(peek().loc, "expected type specifier, got '" + peek().text + "'");
		return nullptr;
	}

	CTypePtr parse_pointer_suffix(CTypePtr base)
	{
		while (check_punct("*"))
		{
			advance();
			// '*' 之后的限定符属于**指针自身**（顶层限定），不是 pointee。
			//   旧写法误记到 base(pointee)->is_const，致 `int * const` 与 `const int *` 不可区分（_Generic L46 会误命中）。
			// P253：四类限定符分别记账——修前除 const 外一律并入 is_const 占位，令拼写无法区分
			//   `int *const` / `int *volatile` / `int *restrict` / `int *_Atomic`（clang 四者各不相同）。
			//   is_const 仍只由 `const` 置位（保持 `_Generic` 既有判定不变），其余走各自专位。
			bool qc = false, qv = false, qr = false, qa = false;
			for (;;)
			{
				if (match_keyword("const")) { qc = true; continue; }
				if (match_keyword("volatile")) { qv = true; continue; }
				if (match_keyword("restrict")) { qr = true; continue; }
				if (match_keyword("_Atomic")) { qa = true; continue; }
				break;
			}
			// pointee const/volatile 由 parse_decl_specifiers 前置关键字置位，不在此处理。
			auto p = make_pointer(base);
			p->is_const = qc;
			p->is_volatile = qv;
			p->is_restrict = qr;
			p->is_atomic = qa;
			base = p;
		}
		return base;
	}

	// void 仅可作函数返回类型、void* 的基类型与形参表标记；作对象/数组元素/成员类型即不完整类型。
	static bool is_void_object_type(const CTypePtr &t)
	{
		if (!t) return false;
		if (t->kind == TypeKind::Void) return true;
		if (t->kind == TypeKind::Array) return is_void_object_type(t->base);
		return false;
	}

	void parse_param_list(std::vector<Param> &params, bool &variadic)
	{
		// (void)
		if (check_keyword("void") && peek(1).kind == TokKind::Punct && peek(1).text == ")")
		{
			advance(); // consume 'void'
			return;
		}
		if (check_punct(")"))
		{
			return; // empty
		}
		variadic = false;
		for (;;)
		{
			if (check_punct("..."))
			{
				advance();
				variadic = true;
				break;
			}
			SourceLoc ploc = peek().loc; // 形参声明起点（诊断用）
			Param p;
			p.loc = ploc;
			CTypePtr base = parse_decl_specifiers();
			reject_alignas(last_has_alignas_, last_alignas_loc_, "a function parameter"); // P250：形参不得带 _Alignas
			if (!base)
				base = make_type(TypeKind::Int); // fallback
			CTypePtr t;
			// 前导 '(' 消歧（§6.7.6p6）：其后为类型/typedef 名/void/.../空 → 函数指示符参数表（退化为指针）；
			//   否则（* / ( / [ / 普通标识符）→ 分组/具名声明符，委托 Decorator。
			if (check_punct("(") && starts_parameter_type_list())
			{
				advance(); // '('
				std::vector<Param> inner;
				bool ivariadic = false;
				parse_param_list(inner, ivariadic);
				expect_punct(")");
				std::vector<CTypePtr> innerTypes;
				for (auto &ip : inner) innerTypes.push_back(ip.type);
				t = make_pointer(make_func_type(base, innerTypes, ivariadic)); // 函数指示符形参 → 函数指针
			}
			else
			{
				std::string pname;
				DeclaratorFn d = parse_declarator_rec(pname); // 具名/* /数组/抽象/分组/内联函数指针皆正确处理
				t = d(base);
				p.name = std::move(pname);
			}
			// 退化调整（§6.7.6.3）：数组形参 → 元素指针；顶层函数指示符形参 → 函数指针
			while (t && t->kind == TypeKind::Array)
				t = make_pointer(t->base);
			if (t && t->kind == TypeKind::Function)
				t = make_pointer(t);
			p.type = t;
			if (is_void_object_type(p.type))
				error(ploc, "argument has incomplete type 'void'");
			params.push_back(std::move(p));
			if (!match_punct(","))
			{
				break;
			}
		}
	}

	StmtPtr parse_compound_stmt()
	{
		auto stmt = std::make_shared<Stmt>();
		stmt->kind = StmtKind::Compound;
		stmt->is_block_scope = true; // 真块 {...}：sema/codegen 据此压弹块级作用域
		stmt->loc = peek().loc;
		expect_punct("{");
		// 块级标签作用域：块内定义的 struct/union tag 只在本块可见（同名则改名，见 scope_definition_tag）
		++block_depth_;
		tag_scope_.emplace_back();
		var_scope_.emplace_back();
		const_local_vals_.emplace_back(); // P256：与 var_scope_ 并列压弹，两层保持同步
		// 函数体：形参在本块可见（须先于 enumerator 判定）
		for (const auto &pn : pending_param_names_)
			if (!pn.empty()) var_scope_.back().insert(pn);
		pending_param_names_.clear();
		while (!check_punct("}") && !at_end())
		{
			auto inner = parse_stmt();
			if (inner)
			{
				stmt->body.push_back(std::move(inner));
			}
			else
			{
				skip_to_semi_or_brace();
			}
		}
		var_scope_.pop_back();
		const_local_vals_.pop_back(); // P256
		tag_scope_.pop_back();
		--block_depth_;
		expect_punct("}");
		return stmt;
	}

	StmtPtr parse_stmt()
	{
		// C23: [[attribute]] 可出现在语句前
		skip_attributes();
		if (check_keyword("if"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::If;
			s->loc = advance().loc;
			expect_punct("(");
			// C23 init-statement: if (init; cond)
			skip_attributes();
			if (is_type_start())
			{
				s->init_stmt = parse_local_decl_no_semi();
				if (!s->init_stmt) { error(peek().loc, "expected init-declaration"); return s; }
				expect_punct(";");
				s->expr = parse_expr();
			}
			else
			{
				ExprPtr first = parse_expr();
				if (check_punct(";"))
				{
					// expression-statement as init
					auto es = std::make_shared<Stmt>();
					es->kind = StmtKind::ExprStmt;
					es->expr = first;
					s->init_stmt = es;
					advance(); // ;
					s->expr = parse_expr();
				}
				else
					s->expr = first;
			}
			expect_punct(")");
			s->then_branch = parse_stmt();
			if (match_keyword("else"))
				s->else_branch = parse_stmt();
			return s;
		}
		if (check_keyword("while"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::While;
			s->loc = advance().loc;
			expect_punct("(");
			s->expr = parse_expr();
			expect_punct(")");
			s->then_branch = parse_stmt();
			return s;
		}
		if (check_keyword("do"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::DoWhile;
			s->loc = advance().loc;
			s->then_branch = parse_stmt();
			if (!match_keyword("while")) { error(peek().loc, "expected 'while'"); return s; }
			expect_punct("(");
			s->expr = parse_expr();
			expect_punct(")");
			match_punct(";");
			return s;
		}
		if (check_keyword("return"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Return;
			s->loc = advance().loc;
			if (!check_punct(";"))
				s->expr = parse_expr();
			match_punct(";");
			return s;
		}
		if (check_keyword("for"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::For;
			s->loc = advance().loc;
			expect_punct("(");
			// init
			if (check_punct(";"))
				advance();
			else if (is_type_start())
				s->for_init = parse_local_decl();
			else
			{
				auto es = std::make_shared<Stmt>();
				es->kind = StmtKind::ExprStmt;
				es->loc = peek().loc;
				es->expr = parse_expr();
				match_punct(";");
				s->for_init = es;
			}
			// cond
			if (!check_punct(";"))
				s->expr = parse_expr();
			match_punct(";");
			// incr
			if (!check_punct(")"))
				s->for_incr = parse_expr();
			expect_punct(")");
			s->then_branch = parse_stmt();
			return s;
		}
		if (check_keyword("break"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Break;
			s->loc = advance().loc;
			match_punct(";");
			return s;
		}
		if (check_keyword("continue"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Continue;
			s->loc = advance().loc;
			match_punct(";");
			return s;
		}
		if (check_keyword("switch"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Switch;
			s->loc = advance().loc;
			expect_punct("(");
			// C23 init-statement: switch (init; cond)
			skip_attributes();
			if (is_type_start())
			{
				s->init_stmt = parse_local_decl_no_semi();
				if (!s->init_stmt) { error(peek().loc, "expected init-declaration"); return s; }
				expect_punct(";");
				s->expr = parse_expr();
			}
			else
			{
				ExprPtr first = parse_expr();
				if (check_punct(";"))
				{
					auto es = std::make_shared<Stmt>();
					es->kind = StmtKind::ExprStmt;
					es->expr = first;
					s->init_stmt = es;
					advance(); // ;
					s->expr = parse_expr();
				}
				else
					s->expr = first;
			}
			expect_punct(")");
			s->then_branch = parse_stmt(); // body (usually compound)
			return s;
		}
		if (check_keyword("case"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Case;
			s->loc = advance().loc;
			// P256：一般常量表达式 case 标签（含 0x/0b/0o/字符/offsetof/sizeof/枚举/算术，及 Co 的 const 全局/局部）。
			//   先由 is_const_int_expr 把关：非恒定（如引用非 const 变量）报 clang 同款错，
			//   消除旧 eval_const_int「失败即 0」静默折成 case 0 的误编译（旧路零诊断）。
			ExprPtr ce = parse_assign();
			if (ce && !is_const_int_expr(ce))
				error(ce->loc, "case label does not reduce to an integer constant");
			s->case_val = eval_const_int(ce);
			expect_punct(":");
			s->then_branch = parse_stmt();
			return s;
		}
		if (check_keyword("default"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Default;
			s->loc = advance().loc;
			expect_punct(":");
			s->then_branch = parse_stmt();
			return s;
		}
		if (check_keyword("goto"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Goto;
			s->loc = advance().loc;
			s->label_name = expect_identifier().text;
			match_punct(";");
			return s;
		}
		// label: identifier + ':' (不跟另一个 ':')
		if (peek().kind == TokKind::Identifier &&
			peek(1).kind == TokKind::Punct && peek(1).text == ":")
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Label;
			s->loc = peek().loc;
			s->label_name = advance().text;
			advance(); // consume ':'
			s->then_branch = parse_stmt();
			return s;
		}
		// _Static_assert 在块作用域是声明（§6.7.4），不产生代码：折后返回空语句占位
		// C23 [[attribute]] 出现后的静态断言照常处理（skip_attributes 已在入口调用）
		if (check_keyword("_Static_assert") || check_keyword("static_assert"))
		{
			parse_static_assert_decl();
			auto nop = std::make_shared<Stmt>();
			nop->kind = StmtKind::ExprStmt;
			nop->loc = peek().loc;
			return nop;
		}
		// 局部声明（类型关键字开头）
		if (is_type_start())
			return parse_local_decl();
		// 复合语句
		if (check_punct("{"))
			return parse_compound_stmt();
		// expression-statement
		auto s = std::make_shared<Stmt>();
		s->kind = StmtKind::ExprStmt;
		s->loc = peek().loc;
		if (!check_punct(";"))
		{
			s->expr = parse_expr();
			if (!s->expr)
				return nullptr;
		}
		match_punct(";");
		return s;
	}

	StmtPtr parse_local_decl()
	{
		// C23 [[attribute]] 跳过
		skip_attributes();
		// 存储类说明符（前置消费 + parse_decl_specifiers 期间消费的任意顺序部分）
		bool is_static = match_keyword("static");
		bool is_extern = match_keyword("extern");
		// 块级 typedef
		bool is_td = match_keyword("typedef");
		CTypePtr base_type = parse_decl_specifiers();
		is_static = is_static || last_static_;
		is_extern = is_extern || last_extern_;
		is_td = is_td || last_typedef_;
		unsigned al = last_alignas_; // 本声明的 _Alignas（在后续递归解析前捕获）
		const bool al_has = last_has_alignas_;    // P250
		const SourceLoc al_loc = last_alignas_loc_; // P250
		// 位点1 前置 GNU aligned 走独立 gnu_decl_align_ 通道（声明符解析前捕获，函数指针参数表会清零）；
		//   仅在各落点取 max 经 apply_type_align 纯提升，绝不并入 al（al 供 _Alignas 专用 check_alignas_min）。
		const unsigned bgnu = gnu_decl_align_;
		if (!base_type && !last_auto_) return nullptr;
		// C23 auto 类型推断: 解析 name + init，然后推断类型
		if (last_auto_)
		{
			last_auto_ = false;
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Decl;
			s->loc = peek().loc;
			s->var_name = expect_identifier().text;
			s->is_static = is_static;
			s->var_align = al;
			if (!match_punct("="))
			{
				error(peek().loc, "'auto' variable requires an initializer");
				return nullptr;
			}
			s->var_init = check_punct("{") ? parse_init_list() : parse_assign();
			// 推断类型
			s->var_type = infer_auto_type(s->var_init);
			local_var_types_[s->var_name] = s->var_type;
			note_local_name(s->var_name);
			note_local_const(s->var_name, s->var_type, s->var_init); // P256
			match_punct(";");
			return s;
		}
		// 纯类型定义（无变量名）: struct/enum 后紧跟 ';'
		if (!is_td && check_punct(";"))
		{
			advance();
			warn_alignas_ignored(al_has, al_loc); // P250
			auto nop = std::make_shared<Stmt>();
			nop->kind = StmtKind::ExprStmt;
			nop->loc = peek().loc;
			return nop;
		}
		if (is_td)
		{
			reject_alignas_only_objects(al_has, al_loc); // P250：typedef 不得带 _Alignas
			// typedef name; 注册别名（对象/数组/函数指针皆经 Decorator）
			do {
				std::string nm;
				DeclaratorFn d = parse_declarator_rec(nm);
				// 位点后置：块级 typedef 名尾 GNU __attribute__（aligned 施加于别名类型；余忽略告警）。
				const unsigned tp = consume_gnu_attributes();
				unsigned tal = al; if (bgnu > tal) tal = bgnu; if (tp > tal) tal = tp;
				typedefs_[nm] = apply_type_align(d(base_type), tal);
			} while (match_punct(","));
			match_punct(";");
			// 返回空 ExprStmt 作占位
			auto nop = std::make_shared<Stmt>();
			nop->kind = StmtKind::ExprStmt;
			nop->loc = peek().loc;
			return nop;
		}
		// 局部声明符（含分组 (*p)[4]、函数指针对象 (*fp)(int)、数组等）：Decorator
		auto compound = std::make_shared<Stmt>();
		compound->kind = StmtKind::Compound;
		compound->loc = peek().loc;
		// P250：register 对象不得带 _Alignas（§6.7.2.1p3）
		reject_alignas(last_register_ && al_has, al_loc, "a variable with 'register' storage class");
		do
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Decl;
			s->loc = peek().loc;
			std::string nm;
			DeclaratorFn d = parse_declarator_rec(nm);
			s->var_type = d(base_type);
			s->var_name = nm;
			if (is_void_object_type(s->var_type))
				error(s->loc, "variable has incomplete type 'void'");
			// 位点后置：局部变量声明符尾 GNU __attribute__((aligned))——独立纯提升通道，
			//   仅经 apply_type_align 抬 s->var_type 并计入 var_align，绝不并入 al（al 供 _Alignas 专用 check_alignas_min）。
			const unsigned bp = consume_gnu_attributes();
			const unsigned bgnu_here = bgnu > bp ? bgnu : bp;
			check_alignas_min(al, al_loc, s->var_type); // P250：_Alignas 强制对齐不得弱于自然对齐（GNU 不套用此约束）
			if (bgnu_here) s->var_type = apply_type_align(s->var_type, bgnu_here);
			s->is_static = is_static;
			s->is_extern = is_extern;
			s->var_align = al > bgnu_here ? al : bgnu_here;
			if (match_punct("="))
				s->var_init = check_punct("{") ? parse_init_list() : parse_assign();
			// 块内函数定义（C 不允许）：函数体只能出现在文件作用域。
			// 此前被当作「块作用域函数声明 + 紧跟的裸复合语句」静默接受。
			SourceLoc bl = peek().loc;
			if (s->var_type && s->var_type->kind == TypeKind::Function && check_punct("{"))
			{
				error(bl, "function definition is not allowed here");
				parse_compound_stmt(); // 消费函数体，避免后续级联误报
			}
			deduce_array_size(s->var_type, s->var_init);
			local_var_types_[s->var_name] = s->var_type;
			note_local_name(s->var_name);
			note_local_const(s->var_name, s->var_type, s->var_init); // P256
			compound->body.push_back(std::move(s));
		} while (match_punct(","));
		match_punct(";");
		if (compound->body.size() == 1)
			return compound->body[0];
		return compound;
	}
	
	// C23 if/switch init-declaration: 解析类似 parse_local_decl 但不消费 ';'
	StmtPtr parse_local_decl_no_semi()
	{
		skip_attributes();
		CTypePtr base_type = parse_decl_specifiers();
		if (!base_type && !last_auto_) return nullptr;
		if (last_auto_)
		{
			last_auto_ = false;
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Decl;
			s->loc = peek().loc;
			s->var_name = expect_identifier().text;
			if (!match_punct("=")) { error(peek().loc, "auto requires initializer"); return nullptr; }
			s->var_init = parse_assign();
			s->var_type = infer_auto_type(s->var_init);
			local_var_types_[s->var_name] = s->var_type;
			note_local_name(s->var_name);
			return s;
		}
		// 简单声明: type name = expr
		auto s = std::make_shared<Stmt>();
		s->kind = StmtKind::Decl;
		s->loc = peek().loc;
		s->var_type = parse_pointer_suffix(base_type);
		s->var_name = expect_identifier().text;
		s->var_type = parse_array_suffix(s->var_type);
		if (is_void_object_type(s->var_type))
			error(s->loc, "variable has incomplete type 'void'");
		check_alignas_min(last_alignas_, last_alignas_loc_, s->var_type); // P250：强制对齐不得弱于自然对齐
		if (match_punct("="))
			s->var_init = check_punct("{") ? parse_init_list() : parse_assign();
		deduce_array_size(s->var_type, s->var_init);
		local_var_types_[s->var_name] = s->var_type;
		note_local_name(s->var_name);
		return s;
	}
	
	// 复合初始化器 {e1, e2, ...} 或嵌套 {...}/{...}
	ExprPtr parse_init_list()
	{
		auto e = std::make_shared<Expr>();
		e->kind = ExprKind::InitList;
		e->loc = expect_punct("{").loc;
		while (!check_punct("}") && !at_end())
		{
			// 指定初始化器（C11 §6.7.9p6）：`.name` / `[expr]` 可串接成设计符链（如 `.ps[1].y`）。
			int di = -1;
			std::string df;
			std::vector<DesigStep> dchain;
			if (check_punct("[") || (check_punct(".") && peek(1).kind == TokKind::Identifier))
			{
				while (true)
				{
					DesigStep st;
					if (check_punct("["))
					{
						advance(); // [
						// 护栏：数组下标指定符须为整型常量表达式；解析失败(如不支持的 GCC 范围指定符
						//   [a ... b]，parse_assign 见 '...' 返回 null)时不可直接 ->int_val，否则空指针 AV。
						//   parse_primary 已记录诊断，st.index 保持 -1（无效下标，后续按畸形初始化器处理）。
						auto idx = parse_assign();
						st.is_index = true;
						st.index = idx ? idx->int_val : -1;
						expect_punct("]");
					}
					else
					{
						advance(); // .
						st.name = advance().text;
					}
					dchain.push_back(st);
					if (!(check_punct("[") || (check_punct(".") && peek(1).kind == TokKind::Identifier)))
						break;
				}
				expect_punct("=");
				// 兼容字段：首级设计符仍回填 desig_index/desig_field，旧消费路径逐字不变；
				// 链长 ≥ 2 时由 codegen 按 desig_chain 走物理路径（P237）。
				if (dchain[0].is_index) di = static_cast<int>(dchain[0].index);
				else df = dchain[0].name;
			}
			// 值表达式
			ExprPtr val;
			if (check_punct("{"))
				val = parse_init_list();
			else
				val = parse_assign();
			// 护栏：值表达式解析失败时(如不支持的 GCC 范围指定符 [a ... b]、畸形初始化器)
			//   parse_assign/parse_init_list 返回 null，parse_primary 已记录诊断；此处终止本
			//   初始化列表，避免对空 ExprPtr 解引用而崩溃（00216 编译期 AV 根因）。
			if (!val) break;
			// 设置 designator
			val->desig_index = di;
			val->desig_field = df;
			val->desig_chain = dchain;
			e->args.push_back(val);
			if (!match_punct(",")) break;
		}
		expect_punct("}");
		return e;
	}

	// 检测 (*name) 函数指针声明符模式
	bool is_funcptr_declarator() const
	{
		return check_punct("(") && peek(1).kind == TokKind::Punct && peek(1).text == "*";
	}

	// 解析 (*name)(params) → Pointer{Function{...}}
	std::pair<std::string, CTypePtr> parse_funcptr_declarator(CTypePtr ret_type)
	{
		advance(); // (
		advance(); // *
		std::string name = expect_identifier().text;
		expect_punct(")");
		if (check_punct("("))
		{
			advance(); // (
			std::vector<Param> ps;
			bool variadic = false;
			parse_param_list(ps, variadic);
			expect_punct(")");
			std::vector<CTypePtr> ptypes;
			for (auto &p : ps) ptypes.push_back(p.type);
			auto fn_ty = make_func_type(ret_type, std::move(ptypes), variadic);
			return {name, make_pointer(fn_ty)};
		}
		if (check_punct("["))
		{
			// (*name)[N] —— pointer to array[N] of ret_type
			auto arr_ty = parse_array_suffix(ret_type);
			return {name, make_pointer(arr_ty)};
		}
		// 无参数列表/无数组后缀：只是指针
		return {name, make_pointer(ret_type)};
	}

	// 解析连续的数组后缀 [ ... ]，返回各维大小（<=0 表示未定/柔性）。消费所有前导 [..]。
	// C99：[ static qual-list? N ]、VLA [ * ] / [ qual * ]；qualifier 列表（const/volatile/restrict/_Atomic）跳过。
	// vla_dims（可选出参）与 dims 一一对应：非空 = 该维为非常量表达式（VLA），供 codegen 运行期求维度。
	std::vector<std::size_t> parse_bracket_dims(std::vector<ExprPtr>* vla_dims = nullptr)
	{
		std::vector<std::size_t> dims;
		while (check_punct("["))
		{
			advance();
			match_keyword("static");
			while (match_keyword("const") || match_keyword("volatile") ||
				match_keyword("restrict") || match_keyword("_Atomic"))
			{
			}
			std::size_t sz = 0;
			ExprPtr vla;
			if (!check_punct("]"))
			{
				if (check_punct("*"))
				{
					advance(); // VLA 未定大小 [*]：按柔性/未定处理（sz=0）
				}
				else
				{
					// 维度为一般常量表达式：字面量(含 0x/0b/0o/八/十)、sizeof/offsetof/枚举/算术等
					ExprPtr dim = parse_assign();
					if (dim && !is_const_int_expr(dim))
					{
						// 非常量维度 = VLA（P208）：旧实现 eval_const_int 落 default 返 0 →
						// sz=0 → LLVM `alloca [0 x T]`（零存储）→ 写入即越界 AV。
						// 尺寸改由 codegen 运行期求值（携 vla_dim）。
						vla = dim;
					}
					else
					{
						int64_t v = dim ? eval_const_int(dim) : -1;
						sz = (v > 0) ? static_cast<std::size_t>(v) : 0; // <=0 视为柔性/未定大小
					}
				}
			}
			expect_punct("]");
			dims.push_back(sz);
			if (vla_dims) vla_dims->push_back(vla);
		}
		return dims;
	}

	// 数组后缀薄封装: [N] [N][M] ... → Array(N, Array(M, elem))（反向构建，最内层维先）
	CTypePtr parse_array_suffix(CTypePtr elem_type)
	{
		std::vector<ExprPtr> vlads;
		std::vector<std::size_t> dims = parse_bracket_dims(&vlads);
		for (int i = static_cast<int>(dims.size()) - 1; i >= 0; --i)
		{
			auto arr = std::make_shared<CType>();
			arr->kind = TypeKind::Array;
			arr->base = elem_type;
			arr->array_size = dims[i];
			if (i < static_cast<int>(vlads.size()) && vlads[i])
			{
				arr->is_vla = true;
				arr->vla_dim = vlads[i];
			}
			elem_type = arr;
		}
		return elem_type;
	}

	// —— 统一 C 声明符（Decorator 递归下降，Phase A）——
	// 声明符解析为「类型装饰函数」D: CTypePtr->CTypePtr，并回填最内层 name；最终类型 = D(base)。
	// 组合律（正确区分 *p[4]=Array(Ptr) 与 (*p)[4]=Ptr(Array)）：
	//   前缀 *   : 结果(X) = rest(Pointer(X))
	//   后缀 [N] : 结果(X) = prev(Array(X,N))
	//   后缀 ()  : 结果(X) = prev(Function(X,params,variadic))
	//   分组 ()  : prev = 递归解析括号内声明符
	using DeclaratorFn = std::function<CTypePtr(CTypePtr)>;

	DeclaratorFn parse_declarator_rec(std::string &name, std::vector<Param>* own = nullptr)
	{
		// 位点5/6：GNU __attribute__ 可出现于声明符各嵌套层（如函数指针类型 `int(ATTR *)(void)`）。
		//   必须在入口消费，否则 `__attribute__`（词法为 Identifier）会被 parse_direct_rec 误读为声明符名
		//   （即 00210 记录的 GCC 式误解析）。此语境 aligned 无对象可施加，值丢弃。
		consume_gnu_attributes();
		if (check_punct("*"))
		{
			advance();
			// P253：与 parse_pointer_suffix 同一口径逐位记账（修前除 const 外全挤进 is_const 占位）
			bool qc = false, qv = false, qr = false, qa = false;
			for (;;)
			{
				if (match_keyword("const")) { qc = true; continue; }
				if (match_keyword("volatile")) { qv = true; continue; }
				if (match_keyword("restrict")) { qr = true; continue; }
				if (match_keyword("_Atomic")) { qa = true; continue; }
				break;
			}
			DeclaratorFn rest = parse_declarator_rec(name, own);
			return [rest, qc, qv, qr, qa](CTypePtr X) -> CTypePtr
			{
				CTypePtr p = make_pointer(X);
				p->is_const = qc;
				p->is_volatile = qv;
				p->is_restrict = qr;
				p->is_atomic = qa;
				return rest(p);
			};
		}
		return parse_direct_rec(name, own);
	}

	DeclaratorFn parse_direct_rec(std::string &name, std::vector<Param>* own = nullptr)
	{
		DeclaratorFn cur;
		bool ident_here = false; // 本帧是否消费了标识符（其紧邻参数表=被定义函数自身参数）
		if (check_punct("("))
		{
			advance();
			cur = parse_declarator_rec(name, own); // 括号内是一个完整声明符
			expect_punct(")");
		}
		else if (peek().kind == TokKind::Identifier)
		{
			last_ident_loc_ = peek().loc; // P252：标识符位置（跨声明对齐一致性诊断指此列）
			name = advance().text;
			ident_here = true;
			cur = [](CTypePtr X) -> CTypePtr { return X; };
		}
		else
		{
			// 抽象声明符（无名）
			cur = [](CTypePtr X) -> CTypePtr { return X; };
		}
		for (;;)
		{
			if (check_punct("["))
			{
				std::vector<ExprPtr> vlads;
				std::vector<std::size_t> dims = parse_bracket_dims(&vlads);
				DeclaratorFn prev = cur;
				cur = [prev, dims, vlads](CTypePtr X) -> CTypePtr
				{
					CTypePtr t = X;
					for (int i = static_cast<int>(dims.size()) - 1; i >= 0; --i)
					{
						auto arr = std::make_shared<CType>();
						arr->kind = TypeKind::Array;
						arr->base = t;
						arr->array_size = dims[i];
						if (i < static_cast<int>(vlads.size()) && vlads[i])
						{
							arr->is_vla = true;   // VLA（P208）：维度交 codegen 运行期求值
							arr->vla_dim = vlads[i];
						}
						t = arr;
					}
					return prev(t);
				};
			}
			else if (check_punct("("))
			{
				advance(); // '('
				std::vector<Param> ps;
				bool variadic = false;
				parse_param_list(ps, variadic);
				expect_punct(")");
				// 紧邻标识符的参数表 = 被定义函数自身参数（带名），回填供函数定义绑定体作用域。
				//   仅记首个（分组外层 (c,d) 因 ident_here=false 不入选）。
				if (ident_here && own && own->empty()) *own = ps;
				std::vector<CTypePtr> ptypes;
				for (auto &p : ps) ptypes.push_back(p.type);
				DeclaratorFn prev = cur;
				cur = [prev, ptypes, variadic](CTypePtr X) -> CTypePtr
				{
					return prev(make_func_type(X, ptypes, variadic));
				};
			}
			else break;
		}
		return cur;
	}

	// 前瞻：声明符是否为「(*[qual]... Identifier」且该标识符后紧跟 '('（函数声明/定义形式）。
	// 命中走保留具名参数与函数体的专用路径；其余（分组/指针到数组/函数指针对象等）走 Decorator。
	bool is_function_form() const
	{
		std::size_t k = 0;
		for (;;)
		{
			const Token &t = peek(k);
			if (t.kind == TokKind::Punct && t.text == "*") { ++k; continue; }
			if (t.kind == TokKind::Keyword &&
				(t.text == "const" || t.text == "volatile" || t.text == "restrict" || t.text == "_Atomic"))
			{ ++k; continue; }
			break;
		}
		if (peek(k).kind != TokKind::Identifier) return false;
		const Token &nx = peek(k + 1);
		return nx.kind == TokKind::Punct && nx.text == "(";
	}

	// 解析一个全局声明符项（对象 / 数组 / 函数指针对象 / 函数原型），登记入 tu。
	// 不消费分隔 ',' 或结尾 ';'（由调用方循环处理）。
	void parse_global_declarator_item(TranslationUnit &tu, CTypePtr base_type,
		bool is_static, bool is_extern, unsigned ga, const SourceLoc &loc,
		bool is_public = false, const DeclAttributes &attrs = DeclAttributes(),
		bool ga_has = false, const SourceLoc &ga_loc = SourceLoc())
	{
		std::string nm;
		std::vector<Param> own; // 紧邻标识符的具名参数（分组/返回函数指针的函数定义用）
		// 位点1/4：本次声明 parse_decl_specifiers 累积的 GNU aligned 提升值须在 parse_declarator_rec 之前捕获
		//   （函数指针声明符的参数表会再入 parse_decl_specifiers 并清零该通道）。
		const unsigned gnu_pre = gnu_decl_align_;
		DeclaratorFn d = parse_declarator_rec(nm, &own);
		CTypePtr type = d(base_type);
		// 位点后置：声明符尾 GNU __attribute__（aligned 记入提升通道；其余属性就地警告忽略）。
		const unsigned gnu_post = consume_gnu_attributes();
		const unsigned gnu_here = gnu_pre > gnu_post ? gnu_pre : gnu_post;
		if (type->kind == TypeKind::Function)
		{
			reject_alignas_only_objects(ga_has, ga_loc); // P250：函数不得带 _Alignas
			// 尾缀构成函数 → 函数原型或定义（分组形 int (*f1(int a,int b))(int c,int d){...}）
			FuncDecl fn;
			fn.loc = loc;
			fn.name = std::move(nm);
			fn.return_type = type->base;
			if (!own.empty() && own.size() == type->params.size())
				fn.params = own; // 带名参数（供函数体绑定 a/b）
			else
				for (auto &pt : type->params)
				{
					Param p;
					p.type = pt;
					fn.params.push_back(std::move(p));
				}
			fn.is_variadic = type->is_variadic;
			fn.is_static = is_static;
			fn.is_public = is_public;
			fn.is_deprecated = attrs.deprecated;
			fn.deprecated_msg = attrs.deprecated_msg;
			fn.is_nodiscard = attrs.nodiscard;      // P249
			fn.nodiscard_msg = attrs.nodiscard_msg;
			fn.is_noreturn = attrs.noreturn_attr;   // P249
			fn.lib_name = attrs.lib_name;
			fn.subsystem_windows = attrs.subsystem_windows;
			func_return_types_[fn.name] = fn.return_type;
			if (check_punct("{")) // 函数定义：消费函数体（原型/声明以 ; 结尾，不进此分支）
			{
				pending_param_names_.clear();
				for (const auto &p : fn.params) pending_param_names_.push_back(p.name);
				fn.body = parse_compound_stmt();
			}
			tu.funcs.push_back(std::move(fn));
			return;
		}
		GlobalVar gv;
		gv.loc = loc;
		gv.name = std::move(nm);
		gv.type = type;
		if (is_void_object_type(gv.type))
			error(loc, "variable has incomplete type 'void'");
		check_alignas_min(ga, ga_loc, gv.type); // P250：强制对齐不得弱于自然对齐（仅 _Alignas；GNU aligned 纯提升不触发）
		// GNU aligned 纯提升（取 max、绝不降）：抬升类型节点对齐（供 sizeof/_Alignof/codegen 感知）。
		if (gnu_here) gv.type = apply_type_align(gv.type, gnu_here);
		gv.align = ga > gnu_here ? ga : gnu_here;
		// P252：跨声明一致性所需事实（比对在 sema 侧，那里能看到同一对象的全部声明记录）：
		//   「出现过 _Alignas」与「结果对齐」分开存——_Alignas(0) 属「出现过但对齐无抬升」，
		//   clang 的「A vs B」比的是结果对齐（= max(给定值, 自然对齐)）。
		gv.align_specified = ga_has;
		gv.align_spec_loc = ga_loc;
		gv.ident_loc = last_ident_loc_;
		{
			const int64_t nat = const_alignof_type(gv.type);
			const int64_t given = gv.align; // 已并入 _Alignas 与 GNU aligned 的最大提升
			const int64_t eff = given > nat ? given : nat;
			gv.align_effective = eff > 0 ? static_cast<unsigned>(eff) : 0;
		}
		gv.is_static = is_static;
		gv.is_extern = is_extern;
		gv.is_public = is_public;
		// P214 C23 属性
		gv.is_deprecated = attrs.deprecated;
		gv.deprecated_msg = attrs.deprecated_msg;
		gv.lib_name = attrs.lib_name;
		if (match_punct("="))
			gv.init = check_punct("{") ? parse_init_list() : parse_assign();
		deduce_array_size(gv.type, gv.init);
		// P256：public const int 全局折值入 const_globals，供跨模块 harvest_const_seeds 采集
		//   （消费方 parse 期把 import 常量作 case 标签/常量表达式）。仅 public、非 extern、有初值、整数、可折叠者。
		if (is_public && gv.type && gv.type->is_const && !gv.is_extern && gv.init
			&& is_int_like_type(gv.type) && is_const_int_expr(gv.init))
			tu.const_globals.push_back({gv.name, eval_const_int(gv.init)});
		tu.globals.push_back(std::move(gv));
	}

	// 按 tag 查 StructDef（brace elision 维度推导用）。
	const StructDef *find_structdef(const std::string &tag)
	{
		if (tag.empty()) return nullptr;
		for (const auto &s : cur_tu_->structs)
			if (s.tag == tag) return &s;
		return nullptr;
	}

	// brace elision 用：一个该类型元素在「无外层花括号」扁平初始化中消耗的标量叶子数。
	// 标量/指针/函数/枚举/位域=1；数组=array_size×leaves(base)；struct=Σ成员；union=首成员。
	std::size_t leaves_per_type(const CTypePtr &t, int depth = 0)
	{
		if (!t || depth > 32) return 1;
		switch (t->kind)
		{
		case TypeKind::Array:
		{
			std::size_t n = t->array_size ? t->array_size : 1; // 内层未定维(elision 罕见)按 1
			return n * leaves_per_type(t->base, depth + 1);
		}
		case TypeKind::Struct:
		case TypeKind::Union:
		{
			const StructDef *sd = find_structdef(t->tag);
			if (!sd || sd->members.empty()) return 1;
			if (sd->is_union) return leaves_per_type(sd->members[0].type, depth + 1);
			std::size_t sum = 0;
			for (const auto &m : sd->members) sum += leaves_per_type(m.type, depth + 1);
			return sum;
		}
		default:
			return 1;
		}
	}

	// 省略大小的数组 T name[] = {…} / char name[] = "str"：从初始化式推导大小。
	// 仅处理最外层维且当前大小为 0（显式给定大小不覆盖；柔性数组成员无初始化式不受影响）。
	void deduce_array_size(CTypePtr &ty, const ExprPtr &init)
	{
		if (!ty || ty->kind != TypeKind::Array) return;
		if (ty->array_size != 0) return;
		if (!init) return;
		if (init->kind == ExprKind::InitList)
		{
			// 指定式护栏：任一 desig_index/desig_field → 维持既有「max(下标)+1」逻辑（P169 等逐字不变）。
			bool has_desig = false;
			for (const auto &ae : init->args)
				if (ae && (ae->desig_index >= 0 || !ae->desig_field.empty())) { has_desig = true; break; }
			if (!has_desig)
			{
				// §6.7.9p14/p19：单字符串元素的花括号 char s[] = {"str"} ≡ = "str"，
				// 大小按字符串内容 +1（'\0'）；否则会落到下方 args.size()=1。
				// 仅字符型元素生效（排除指针数组 char *p[]={"abc"} → 应 size=1、非 strlen+1）。
				if (init->args.size() == 1 && init->args[0] &&
					init->args[0]->kind == ExprKind::StringLiteral &&
					ty->base && (ty->base->kind == TypeKind::Char || ty->base->kind == TypeKind::Char8 ||
								  ty->base->kind == TypeKind::WChar || ty->base->kind == TypeKind::Char16 ||
								  ty->base->kind == TypeKind::Char32))
				{
					const int64_t ew = ty->base ? const_sizeof_type(ty->base) : 1;
					if (ew >= 4)      ty->array_size = static_cast<int64_t>(co_decode_utf32(init->args[0]->str_val).size() + 1);
					else if (ew >= 2) ty->array_size = static_cast<int64_t>(co_decode_utf16(init->args[0]->str_val).size() + 1);
					else              ty->array_size = static_cast<int64_t>(co_decode_string(init->args[0]->str_val).size() + 1);
					return;
				}
				// brace elision 计数（P173）：元素为聚合且首项非花括号 → 扁平标量按每元素 L 个叶子分组。
				std::size_t L = leaves_per_type(ty->base);
				if (L > 1 && !init->args.empty() && init->args[0]->kind != ExprKind::InitList)
					ty->array_size = (init->args.size() + L - 1) / L;
				else
					ty->array_size = init->args.size(); // 标量元素或逐元素带括号 → 每项一元素
				return;
			}
			// §6.7.9: 省略大小时，数组大 = (最大下标)+1；需考虑 `[N]` 指定符。
			//   位置式初始化按序占位，指定式 `[N]=` 跳转至 N且下一位置从 N+1 起。
			//   不能用 args.size()（如 {5,[2]=2,3} 为 3 个元素但最大下标 3 → 应 4）。
			std::size_t next_pos = 0, maxv = 0;
			for (const auto &ae : init->args)
			{
				std::size_t idx = (ae && ae->desig_index >= 0)
					? static_cast<std::size_t>(ae->desig_index) : next_pos;
				next_pos = idx + 1;
				if (next_pos > maxv) maxv = next_pos;
			}
			ty->array_size = static_cast<long long>(maxv);
			return;
		}
		// §6.7.9p14：字符数组由字符串字面量初始化时，大小为内容元素数 + 1（含 '\0'）。
		// 按数组元素宽度分派编码（前缀无关，沿用 P177「按元素宽度驱动」）：
		//   1 字节（char/char8_t）→ UTF-8 字节数；2 字节（wchar_t/char16_t）→ UTF-16 码元数；
		//   4 字节（char32_t）→ UTF-32 码点数（修 plct 00220 同域、补全 charN_t）。
		if (init->kind == ExprKind::StringLiteral)
		{
			const int64_t ew = ty->base ? const_sizeof_type(ty->base) : 1;
			if (ew >= 4)      ty->array_size = static_cast<int64_t>(co_decode_utf32(init->str_val).size() + 1);
			else if (ew >= 2) ty->array_size = static_cast<int64_t>(co_decode_utf16(init->str_val).size() + 1);
			else              ty->array_size = static_cast<int64_t>(co_decode_string(init->str_val).size() + 1);
			return;
		}
	}

	bool is_type_start() const
	{
		const Token &t = peek();
		if (t.kind != TokKind::Keyword && t.kind != TokKind::Identifier)
			return false;
		if (t.text == "void" || t.text == "char" || t.text == "wchar_t" ||
			t.text == "char8_t" || t.text == "char16_t" || t.text == "char32_t" ||
			t.text == "short" ||
			t.text == "int" || t.text == "long" || t.text == "float" ||
			t.text == "double" || t.text == "signed" || t.text == "unsigned" ||
			t.text == "const" || t.text == "volatile" || t.text == "struct" || t.text == "union" ||
			t.text == "enum" || t.text == "typedef" ||
			t.text == "static" || t.text == "extern" || t.text == "register" ||
			t.text == "_Bool" || t.text == "inline" || t.text == "restrict" ||
			t.text == "_Inline" || t.text == "_Noreturn" ||
			t.text == "_Alignas" || t.text == "alignas" ||
			t.text == "_Thread_local" || t.text == "thread_local" ||
			t.text == "_Static_assert" || t.text == "_Atomic" ||
			t.text == "auto" || t.text == "bool" || t.text == "constexpr" ||
			t.text == "typeof" || t.text == "typeof_unqual" || t.text == "_BitInt" ||
			t.text == "thread_local" || t.text == "public")
			return true;
		// typedef 名
		if (typedefs_.count(t.text)) return true;
		// P232 Co 层：省去 struct/union/enum 关键字——裸标签名作类型（仅 .co 生效、纯 C 档不进此分支）
		if (co_mode_ && t.kind == TokKind::Identifier &&
			(struct_tags_.count(t.text) > 0 || enum_tag_names_.count(t.text) > 0))
			return true;
		return false;
	}

	// 判断当前 '('_后_一个_token_是否是类型关键字或 typedef 名（用于 cast 消歧）
	bool is_type_lead(const Token &t) const
	{
		if (t.kind != TokKind::Keyword && t.kind != TokKind::Identifier)
			return false;
		const std::string &s = t.text;
		if (s == "void" || s == "char" || s == "wchar_t" ||
			s == "char8_t" || s == "char16_t" || s == "char32_t" ||
			s == "short" ||
			s == "int" || s == "long" || s == "float" ||
			s == "double" || s == "signed" || s == "unsigned" ||
			s == "const" || s == "volatile" || s == "struct" || s == "union" ||
			s == "enum" || s == "_Bool" || s == "bool" ||
			s == "_BitInt" || s == "typeof" || s == "typeof_unqual" ||
			s == "constexpr" || s == "_Atomic")
			return true;
		if (typedefs_.count(s)) return true;
		// P232 Co 层：裸标签名作 cast/参数类型（仅 .co 生效）
		if (co_mode_ && t.kind == TokKind::Identifier &&
			(struct_tags_.count(s) > 0 || enum_tag_names_.count(s) > 0))
			return true;
		return false;
	}
	bool is_type_after_paren() const
	{
		return is_type_lead(peek(1));
	}
	// 站在 '(' 处：判断是否为强制类型转换的左括号。除常规类型起始外，额外越过前置的
	//   GNU __attribute__((...)) 组再判定（位点5：`((ATTR int(*)(void)) fp)` 中 ATTR 后才是类型）。
	bool is_cast_open_paren() const
	{
		std::size_t off = 1;
		for (;;)
		{
			const Token &t = peek(off);
			const bool is_kw = (t.kind == TokKind::Keyword || t.kind == TokKind::Identifier);
			if (!is_kw || t.text != "__attribute__") break;
			++off; // 越过 __attribute__
			int depth = 0;
			while (off < toks.size())
			{
				const Token &x = peek(off);
				if (x.kind == TokKind::Punct && x.text == "(") ++depth;
				else if (x.kind == TokKind::Punct && x.text == ")")
				{
					--depth;
					if (depth == 0) { ++off; break; }
				}
				++off;
			}
		}
		return is_type_lead(peek(off));
	}

	// 站在声明符起始 '('：判断其后是否为函数指示符参数表（vs 分组声明符）。§6.7.6p6
	bool starts_parameter_type_list() const
	{
		const Token &n = peek(1);
		if (n.kind == TokKind::Punct && (n.text == ")" || n.text == "..."))
			return true; // 空参数表 / 仅省略号 → 函数型
		return is_type_after_paren(); // 类型关键字 / typedef 名 / const/_Atomic 等
	}

	// ============================================================
	// 表达式解析（逗号 → 赋值 → 三元 → 二元优先级爬升 → 一元 → 后缀 → 基元）
	// ============================================================
	ExprPtr parse_expr() { return parse_comma_expr(); }

	// 逗号表达式（仅用于顶层 expression-statement 和 return）
	ExprPtr parse_comma_expr()
	{
		auto first = parse_assign();
		if (!first) return nullptr;
		if (!check_punct(","))
			return first;
		auto e = std::make_shared<Expr>();
		e->kind = ExprKind::Comma;
		e->loc = first->loc;
		e->args.push_back(std::move(first));
		while (match_punct(","))
		{
			auto next = parse_assign();
			if (next) e->args.push_back(std::move(next));
		}
		return e;
	}

	ExprPtr parse_assign()
	{
		auto lhs = parse_conditional();
		if (!lhs) return nullptr;
		if (peek().kind == TokKind::Punct)
		{
			const std::string &op = peek().text;
			if (op == "=" || op == "+=" || op == "-=" || op == "*=" ||
				op == "/=" || op == "%=" || op == "<<=" || op == ">>=" ||
				op == "&=" || op == "|=" || op == "^=")
			{
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::Assign;
				e->loc = advance().loc;
				e->op = op;
				e->lhs = std::move(lhs);
				e->rhs = parse_assign(); // right-assoc
				return e;
			}
		}
		return lhs;
	}

	// 三元运算符: cond ? then_expr : else_expr
	ExprPtr parse_conditional()
	{
		auto lhs = parse_binary(1);
		if (!lhs) return nullptr;
		if (!check_punct("?"))
			return lhs;
		auto e = std::make_shared<Expr>();
		e->kind = ExprKind::Ternary;
		e->loc = lhs->loc;
		e->lhs = std::move(lhs);
		advance(); // consume '?'
		e->args.push_back(parse_expr()); // then 允许逗号
		expect_punct(":");
		e->args.push_back(parse_conditional()); // else 递归三元（无逗号）
		return e;
	}

	ExprPtr parse_binary(int min_prec)
	{
		auto lhs = parse_unary();
		while (lhs && peek().kind == TokKind::Punct)
		{
			const int prec = binop_prec(peek().text);
			if (prec == 0 || prec < min_prec) break;
			std::string op = advance().text;
			auto rhs = parse_binary(prec + 1);
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Binary;
			e->loc = peek().loc;
			e->op = op;
			e->lhs = std::move(lhs);
			e->rhs = std::move(rhs);
			lhs = std::move(e);
		}
		return lhs;
	}

	static int binop_prec(const std::string &op)
	{
		if (op == "*" || op == "/" || op == "%") return 13;
		if (op == "+" || op == "-") return 12;
		if (op == "<<" || op == ">>") return 11;
		if (op == "<" || op == ">" || op == "<=" || op == ">=") return 10;
		if (op == "==" || op == "!=") return 9;
		if (op == "&") return 8;
		if (op == "^") return 7;
		if (op == "|") return 6;
		if (op == "&&") return 5;
		if (op == "||") return 4;
		return 0;
	}

	ExprPtr parse_unary()
	{
		// 显式类型转换: (type-specifier) unary-expr
		if (check_punct("(") && is_cast_open_paren())
		{
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Cast;
			e->loc = peek().loc;
			advance(); // consume '('
			e->type = parse_decl_specifiers();
			// 抽象声明符（Decorator）：支持 void (*)(void) 等函数指针/数组 cast 类型
			{
				std::string dummy_nm;
				DeclaratorFn d = parse_declarator_rec(dummy_nm);
				e->type = d(e->type);
			}
			expect_punct(")");
			// 复合字面量: (type){init-list}
			if (check_punct("{"))
			{
				// 复合字面量: (type){init-list}
				auto lit = parse_init_list(); // parse_init_list 已处理 '{...}'
				lit->type = e->type;
				lit->loc = e->loc;
				return lit;
			}
			e->lhs = parse_unary();
			return e;
		}
		if (peek().kind == TokKind::Punct)
		{
			const std::string &op = peek().text;
			if (op == "-" || op == "+" || op == "!" || op == "~" || op == "*" || op == "&")
			{
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::Unary;
				e->loc = advance().loc;
				e->op = op;
				e->lhs = parse_unary();
				return e;
			}
			if (op == "++" || op == "--")
			{
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::Unary;
				e->loc = advance().loc;
				e->op = op;
				e->postfix = false;
				e->lhs = parse_unary();
				return e;
			}
		}
		// _Alignof(type) / alignof(type)
		if (check_keyword("_Alignof") || check_keyword("alignof"))
		{
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Unary;
			e->loc = advance().loc;
			e->op = "alignof";
			expect_punct("(");
			e->type = parse_decl_specifiers();
			e->type = parse_pointer_suffix(e->type);
			if (check_punct("["))
				e->type = parse_array_suffix(e->type);
			expect_punct(")");
			return e;
		}
		if (check_keyword("sizeof"))
		{
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Unary;
			e->loc = advance().loc;
			e->op = "sizeof";
			if (check_punct("("))
			{
				// 尝试 sizeof(type): ( 后跟类型名则为类型
				if (is_type_after_paren())
				{
					advance(); // consume '('
					e->type = parse_decl_specifiers();
					e->type = parse_pointer_suffix(e->type);
					expect_punct(")");
				}
				else
				{
					advance();
					e->lhs = parse_expr();
					expect_punct(")");
				}
			}
			else
				e->lhs = parse_unary();
			return e;
		}
		// _Generic(select, type1: expr1, type2: expr2, ..., default: exprN)
		if (check_keyword("_Generic"))
		{
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Generic;
			e->loc = advance().loc; // consume '_Generic'
			expect_punct("(");
			e->lhs = parse_assign(); // controlling expression
			while (match_punct(","))
			{
				auto assoc = std::make_shared<Expr>();
				if (check_keyword("default"))
				{
					advance(); // 'default'
					assoc->type = nullptr; // null = default
				}
				else
				{
					assoc->type = parse_decl_specifiers();
					assoc->type = parse_pointer_suffix(assoc->type);
					if (check_punct("["))
						assoc->type = parse_array_suffix(assoc->type);
				}
				expect_punct(":");
				assoc->rhs = parse_assign();
				e->args.push_back(std::move(assoc));
			}
			expect_punct(")");
			return apply_postfix(e); // _Generic 结果纳入 postfix：支持 _Generic(...)() 调用
		}
		return parse_postfix();
	}

	// 对已解析的前缀表达式 e 依次施加后缀运算符（调用/下标/成员/自增）。
	//   抽自 parse_postfix，使 _Generic 结果也能参与 postfix（如 _Generic(...)()）。
	ExprPtr apply_postfix(ExprPtr e)
	{
		for (;;)
		{
			if (check_punct("("))
			{
				auto call = std::make_shared<Expr>();
				call->kind = ExprKind::Call;
				call->loc = e->loc;
				if (e->kind == ExprKind::VarRef)
					call->name = e->name;
				else
					call->lhs = e; // 间接被调表达式（如 (*fp)(...) ）
				advance();
				// 变参内建 va_arg(ap, type)：第二实参是类型，按 sizeof(type) 同样方式解析
				if (call->name == "va_arg" && !call->lhs)
				{
					auto ap = parse_assign();
					if (ap) call->args.push_back(std::move(ap));
					if (!match_punct(","))
					{
						error(peek().loc, "va_arg expects (ap, type)");
						expect_punct(")");
						e = std::move(call);
						continue;
					}
					CTypePtr t = parse_decl_specifiers();
					t = parse_pointer_suffix(t);
					if (check_punct("["))
						t = parse_array_suffix(t);
					call->type = t;
					expect_punct(")");
					e = std::move(call);
					continue;
				}
				// 内建 __builtin_offsetof(type, designator)：编译期求字节偏移（供数组长度/case 等常量上下文）。
				// 直接展开为携带 size_t 类型的整型字面量常量，零 codegen 依赖。
				if (call->name == "__builtin_offsetof" && !call->lhs)
				{
					CTypePtr cur = parse_decl_specifiers();
					cur = parse_pointer_suffix(cur);
					if (check_punct("["))
						cur = parse_array_suffix(cur);
					if (!match_punct(","))
					{
						error(peek().loc, "__builtin_offsetof expects (type, member)");
						expect_punct(")");
						e = std::move(call);
						continue;
					}
					int64_t off = 0;
					bool bad = false;
					// 成员访问：在当前 struct/union 上累加对齐填充后的偏移，并切换到成员类型；
					// 直查不中时递归穿过匿名聚合（成员名提升，与 sema/codegen 同口径；P235）。
					auto access_member = [&](auto &&self, const std::string &mname, int depth) -> bool {
						if (depth > 16) return false;
						if (!cur || (cur->kind != TypeKind::Struct && cur->kind != TypeKind::Union)) return false;
						const StructDef *sd = nullptr;
						for (const auto &s : cur_tu_->structs)
							if (s.tag == cur->tag) { sd = &s; break; }
						if (!sd) return false;
						const bool isun = sd->is_union;
						// 一级：直命名成员（union 各成员偏移恒 0）
						int64_t moff = 0;
						for (const auto &m : sd->members)
						{
							if (!isun)
							{
								int64_t ma = const_member_align(m);
								if (ma < 1) ma = 1;
								moff = ((moff + ma - 1) / ma) * ma;
							}
							if (!m.name.empty() && m.name == mname) { off += moff; cur = m.type; return true; }
							if (!isun) moff += const_sizeof_type(m.type);
						}
						// 二级：穿过匿名聚合递归（内层偏移照加；union 级无偏移）
						moff = 0;
						for (const auto &m : sd->members)
						{
							if (!isun)
							{
								int64_t ma = const_member_align(m);
								if (ma < 1) ma = 1;
								moff = ((moff + ma - 1) / ma) * ma;
							}
							if (m.name.empty() && m.type
								&& (m.type->kind == TypeKind::Struct || m.type->kind == TypeKind::Union)
								&& !m.type->tag.empty())
							{
								int64_t saved_off = off;
								CTypePtr saved_cur = cur;
								off += moff; cur = m.type;
								if (self(self, mname, depth + 1)) return true;
								off = saved_off; cur = saved_cur;
							}
							if (!isun) moff += const_sizeof_type(m.type);
						}
						return false;
					};
					// 数组下标：按元素大小前移，并切换到元素类型
					auto access_index = [&](int64_t idx) -> bool {
						if (!cur || (cur->kind != TypeKind::Array && cur->kind != TypeKind::Pointer)) return false;
						off += idx * const_sizeof_type(cur->base);
						cur = cur->base; return true;
					};
					{
						auto id = expect_identifier();
						if (!access_member(access_member, id.text, 0)) bad = true;
					}
					while (!check_punct(")"))
					{
						if (match_punct("."))
						{
							auto id = expect_identifier();
							if (!access_member(access_member, id.text, 0)) bad = true;
						}
						else if (match_punct("["))
						{
							int64_t iv = eval_const_int(parse_assign());
							expect_punct("]");
							if (!access_index(iv)) bad = true;
						}
						else { bad = true; break; }
					}
					expect_punct(")");
					if (bad)
					{
						error(call->loc, "__builtin_offsetof: invalid type or member designator");
						off = 0;
					}
					auto lit = std::make_shared<Expr>();
					lit->kind = ExprKind::IntLiteral;
					lit->loc = call->loc;
					lit->int_val = off;
					auto st = std::make_shared<CType>();
					st->kind = TypeKind::LongLong; // size_t（LLP64 无符号 64 位）
					st->is_unsigned = true;
					lit->type = st;
					e = std::move(lit);
					continue;
				}
				if (!check_punct(")"))
				{
					for (;;)
					{
						auto arg = parse_assign(); // 实参不含逗号运算符
						if (arg) call->args.push_back(std::move(arg));
						if (!match_punct(",")) break;
					}
				}
				expect_punct(")");
				e = std::move(call);
				continue;
			}
			if (check_punct("["))
			{
				auto idx = std::make_shared<Expr>();
				idx->kind = ExprKind::Binary;
				idx->loc = e->loc;
				idx->op = "[]";
				idx->lhs = std::move(e);
				advance();
				idx->rhs = parse_expr();
				expect_punct("]");
				e = std::move(idx);
				continue;
			}
			if (check_punct("++") || check_punct("--"))
			{
				auto inc = std::make_shared<Expr>();
				inc->kind = ExprKind::Unary;
				// P254：loc 取**运算符 token**（clang 对 `z++` 的只读诊断定位在 `++`、而非操作数起点）
				inc->loc = peek().loc;
				inc->op = advance().text;
				inc->postfix = true;
				inc->lhs = std::move(e);
				e = std::move(inc);
				continue;
			}
			if (check_punct(".") || check_punct("->"))
			{
				auto mem = std::make_shared<Expr>();
				mem->kind = ExprKind::Member;
				mem->loc = e->loc;
				mem->op = advance().text; // "." or "->"
				mem->lhs = std::move(e);
				mem->name = expect_identifier().text;
				// type 字段存结构体类型（codegen 用）
				mem->type = nullptr; // 后续由 sema 或 codegen 推断
				e = std::move(mem);
				continue;
			}
			break;
		}
		return e;
	}

	ExprPtr parse_postfix()
	{
		auto e = parse_primary();
		if (!e) return nullptr;
		return apply_postfix(std::move(e));
	}

	// 解析十六进制浮点字面量 "0x1.8p3" → double
	static double parse_hex_float(const std::string &s)
	{
		// 去掉 0x/0X 前缀
		std::size_t pos = 2;
		double mantissa = 0.0;
		// 解析整数部分
		while (pos < s.size())
		{
			char c = s[pos];
			int d;
			if (c >= '0' && c <= '9') d = c - '0';
			else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
			else break;
			mantissa = mantissa * 16.0 + d;
			++pos;
		}
		// 小数部分
		if (pos < s.size() && s[pos] == '.')
		{
			++pos;
			double frac = 1.0 / 16.0;
			while (pos < s.size())
			{
				char c = s[pos];
				int d;
				if (c >= '0' && c <= '9') d = c - '0';
				else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
				else break;
				mantissa += d * frac;
				frac /= 16.0;
				++pos;
			}
		}
		// 指数部分 p/P[+/-]digits
		if (pos < s.size() && (s[pos] == 'p' || s[pos] == 'P'))
		{
			++pos;
			int exp_sign = 1;
			if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
			{
				if (s[pos] == '-') exp_sign = -1;
				++pos;
			}
			int expo = 0;
			while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
			{
				expo = expo * 10 + (s[pos] - '0');
				++pos;
			}
			mantissa *= std::ldexp(1.0, exp_sign * expo);
		}
		return mantissa;
	}

	ExprPtr parse_primary()
	{
		const Token &t = peek();
		if (t.kind == TokKind::Number)
		{
			advance();
			auto e = std::make_shared<Expr>();
			e->loc = t.loc;
			bool is_hex = (t.text.size() > 2 && t.text[0] == '0' &&
				(t.text[1] == 'x' || t.text[1] == 'X'));
			// 检测十六进制浮点: 0x...p/P...
			bool is_hexfloat = is_hex &&
				(t.text.find('p') != std::string::npos || t.text.find('P') != std::string::npos);
			// 检测普通浮点: 含 '.' 或 e/E指数(非 hex)
			bool is_float = is_hexfloat ||
				(t.text.find('.') != std::string::npos && !is_hex) ||
				(!is_hex && (t.text.find('e') != std::string::npos ||
					t.text.find('E') != std::string::npos));
			if (is_hexfloat || is_float)
			{
				e->kind = ExprKind::FloatLiteral;
				e->float_val = is_hexfloat ? parse_hex_float(t.text) : std::stod(t.text);
				// C §6.4.5 浮点后缀定型（P9.24）：f/F → float；无后缀 / l/L → double。
				//   （本机 Windows long double==double，故 L 亦落 double，与 clang -std=c23 一致。
				//    std::stod / parse_hex_float 均自然忽略尾随 f/L，值解析无须剥后缀。）
				const char lastc = t.text.empty() ? '\0' : t.text.back();
				if (lastc == 'f' || lastc == 'F')
					e->type = make_type(TypeKind::Float);
			}
			else
			{
				e->kind = ExprKind::IntLiteral;
				// C23 位精确整数字面量后缀：wb/WB → signed _BitInt(N)，ub/UB → unsigned _BitInt(N)。
				// N 为表示该值所需的最小位宽（signed 额外含符号位）。
				const std::string &txt = t.text;
				const std::size_t tl = txt.size();
				bool bi = false, bi_unsigned = false;
				std::string digits = txt;
				if (tl >= 2)
				{
					const char a = txt[tl - 2], b = txt[tl - 1];
					if ((b == 'b' || b == 'B') && (a == 'w' || a == 'W'))
					{
						bi = true; bi_unsigned = false; digits = txt.substr(0, tl - 2);
					}
					else if ((b == 'b' || b == 'B') && (a == 'u' || a == 'U'))
					{
						bi = true; bi_unsigned = true; digits = txt.substr(0, tl - 2);
					}
				}
				unsigned long long mag = 0ULL;
				{
					// 规范化 C23 前缀 0b/0B(二进制)、0o/0O(八进制)——std::stoull base0 不识别这两者；
					// 0x / 前导 0 八进制 / 十进制仍交给 base0 自动判定。
					std::string num = digits;
					int rbase = 0;
					if (num.size() >= 2 && num[0] == '0')
					{
						const char c1 = num[1];
						if (c1 == 'b' || c1 == 'B') { rbase = 2; num = num.substr(2); }
						else if (c1 == 'o' || c1 == 'O') { rbase = 8; num = num.substr(2); }
					}
					if (!num.empty()) mag = std::stoull(num, nullptr, rbase);
				}
				e->int_val = static_cast<long long>(mag);
				if (bi)
				{
					// bitlen(mag)：表示 mag 所需的二进制位数（mag==0 计 1）
					unsigned n = 1;
					for (unsigned long long x = mag; x >= 2; x >>= 1) ++n;
					// signed 额外含符号位（mag==0 保留 _BitInt(1)）
					if (!bi_unsigned && mag != 0) ++n;
					auto bt = std::make_shared<CType>();
					bt->kind = TypeKind::BitInt;
					bt->bit_width = n;
					bt->is_unsigned = bi_unsigned;
					e->type = bt;
				}
				else
				{
					// C §6.4.5.2：按后缀(u/U, l/L, ll/LL)与进制(十进制 vs 八/十六进制)，
					// 从候选类型序列中选出能容纳该值的最小类型（LLP64：int=32, long=32, long long=64）。
					// 十进制候选恒不含无符号型；八/十六进制可选到 unsigned int/long/long long。
					std::size_t sb = tl;
					while (sb > 0)
					{
						char c = txt[sb - 1];
						if (c == 'u' || c == 'U' || c == 'l' || c == 'L') --sb;
						else break;
					}
					bool uns = false; unsigned lcnt = 0;
					if (sb < tl)
					{
						std::string sfx = txt.substr(sb);
						for (char c : sfx) { if (c == 'u' || c == 'U') uns = true; else ++lcnt; }
					}
					// 十进制字面量不以 '0' 开头（单独 "0" 视作十进制）
					bool decimal = !(txt.size() > 1 && txt[0] == '0');
					struct Cand { TypeKind tk; bool uns; unsigned bits; };
					std::vector<Cand> list;
					if (uns && lcnt == 0)
						list = { {TypeKind::Int,true,32},{TypeKind::Long,true,32},{TypeKind::LongLong,true,64} };
					else if (uns && lcnt >= 2) // ull / ULL 等（两个 l + u）：§6.4.5.2 候选仅 unsigned long long
						list = { {TypeKind::LongLong,true,64} };
					else if (uns) // ul / uL（一个 l + u）
						list = { {TypeKind::Long,true,32},{TypeKind::LongLong,true,64} };
					else if (lcnt >= 2) // ll / LL
						list = { {TypeKind::LongLong,false,64} };
					else if (lcnt == 1) // l / L
						list = { {TypeKind::Long,false,32},{TypeKind::LongLong,false,64} };
					else if (decimal) // 无后缀十进制
						list = { {TypeKind::Int,false,32},{TypeKind::Long,false,32},{TypeKind::LongLong,false,64} };
					else // 无后缀八/十六进制
						list = { {TypeKind::Int,false,32},{TypeKind::Int,true,32},{TypeKind::Long,false,32},
						         {TypeKind::Long,true,32},{TypeKind::LongLong,false,64},{TypeKind::LongLong,true,64} };
					Cand chosen = list.back();
					for (const Cand &cd : list)
					{
						bool fits;
						if (cd.bits == 64)
							fits = cd.uns ? true : (mag <= 0x7FFFFFFFFFFFFFFFULL);
						else
							fits = cd.uns ? (mag <= 0xFFFFFFFFULL) : (mag <= 0x7FFFFFFFULL);
						if (fits) { chosen = cd; break; }
					}
					auto lt = std::make_shared<CType>();
					lt->kind = chosen.tk;
					lt->is_unsigned = chosen.uns;
					e->type = lt;
				}
			}
			return e;
		}
		if (t.kind == TokKind::String)
		{
			advance();
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::StringLiteral;
			e->loc = t.loc;
			e->str_val = t.text;
			// 通用字符名（\uXXXX/\UXXXXXXXX）语法/码点校验（ISO C §6.4.3）；
			//   展开为 UTF-8 字节由 co_decode_string 入口统一完成（单一真值源）。
			const std::string ucn = co_ucn_validate(t.text);
			if (!ucn.empty()) error(t.loc, ucn);
			return e;
		}
		// C23: nullptr / true / false
		if (t.kind == TokKind::Keyword)
		{
			if (t.text == "nullptr")
			{
				advance();
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::IntLiteral;
				e->loc = t.loc;
				e->int_val = 0;
				e->type = make_pointer(make_type(TypeKind::Void));
				return e;
			}
			if (t.text == "true")
			{
				advance();
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::IntLiteral;
				e->loc = t.loc;
				e->int_val = 1;
				return e;
			}
			if (t.text == "false")
			{
				advance();
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::IntLiteral;
				e->loc = t.loc;
				e->int_val = 0;
				return e;
			}
		}
		if (t.kind == TokKind::Identifier)
		{
			// enum 常量名优先匹配——但内层同名局部变量/形参隐藏外层 enumerator（C §6.2.1：同一命名空间）。
			//   缺此判定时 `int tks04 = 0; tks04++;` 被折叠成字面量 103，store 到常量触发 verifier
			//   「Store operand must be a pointer」（CTS 0151 族）。
			auto ec = enum_constants_.find(t.text);
			if (ec != enum_constants_.end() && !name_in_scope(t.text))
			{
				advance();
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::IntLiteral;
				e->loc = t.loc;
				e->int_val = ec->second;
				return e;
			}
			advance();
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::VarRef;
			e->loc = t.loc;
			e->name = t.text;
			return e;
		}
		if (t.kind == TokKind::Char)
		{
			advance();
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::CharLiteral;
			e->loc = t.loc;
			e->str_val = t.text;
			// 带前缀字符常量按 ISO C §6.4.4.4 定型：L→wchar_t、u→char16_t、U→char32_t、
			//   u8→char8_t；无前缀仍为 int。类型随字面量携带，供 sizeof/_Generic/常用算术转换
			//   取真实宽度与符号（旧实现不记前缀、一律当 int，sizeof(L'A') 误得 4）。
			e->char_prefix = co_char_lit_prefix(t.text);
			// 通用字符名（\uXXXX/\UXXXXXXXX）语法/码点校验（ISO C §6.4.3）——
			//   位数不足、码点落在代理区或越过 0x10FFFF 均在此报错。口径取 C23（本项目为单一
			//   C 超集方言，不按 -std 门控）：C11 曾禁止用 UCN 表示 <0xA0 的普通字符，C23 已放宽。
			{
				const std::string ucn = co_ucn_validate(t.text);
				if (!ucn.empty()) error(t.loc, ucn);
			}
			// 字形与符号须与声明路径 parse_type_spec 一致（charN_t 恒无符号、wchar_t 取默认），
			//   否则 _Generic 关联匹配（types_compatible 比 is_unsigned）会漏命中。
			if (e->char_prefix == "L") e->type = make_type(TypeKind::WChar);
			else if (e->char_prefix == "u") { e->type = make_type(TypeKind::Char16); e->type->is_unsigned = true; }
			else if (e->char_prefix == "U") { e->type = make_type(TypeKind::Char32); e->type->is_unsigned = true; }
			else if (e->char_prefix == "u8") { e->type = make_type(TypeKind::Char8); e->type->is_unsigned = true; }
			// 取值：窄路（无前缀/u8）沿用 co_decode_char 既口径逐字不变；宽前缀（L/u/U）取完整
			//   码点（非 ASCII 不再错取 UTF-8 首字节）。码点超出字面量宽度时按 clang 口径报错。
			if (e->char_prefix.empty() || e->char_prefix == "u8")
			{
				// 取值前先展开 UCN（'\u0024' → '$'）；无 UCN 时文本逐字不变，行为与旧口径恒等。
				e->int_val = co_decode_char(co_expand_ucn(t.text));
				// 窄字符常量：char 为有符号 8 位（i8），UCN 码点 > 0x7F 不可表示——与 clang 同文报错。
				//   （直接书写的多字节源字符如 '中' 仍取首字节、维持旧宽松口径，不在此列。）
				if (e->char_prefix.empty())
				{
					uint32_t ucp = 0;
					if (co_ucn_first(t.text, ucp) && ucp > 0x7F)
						error(t.loc, "character too large for enclosing character literal type");
				}
				// u8 字符常量须为**单个 UTF-8 码元**：非 ASCII（码点 > 0x7F）的 UTF-8 表示≥ 2 字节，
				//   放不进 char8_t——与 clang 同文报错（u8'\u00e9' 亦被 clang 拒收）。
				if (e->char_prefix == "u8" && co_decode_char_wide(t.text) > 0x7F)
					error(t.loc, "character too large for enclosing character literal type");
			}
			else
			{
				e->int_val = co_decode_char_wide(t.text);
				// 宽前缀字面量宽度判据：
				//   u  → char16_t 恒 16 位（定宽、与 target 无关） → 0xFFFF
				//   L  → wchar_t 宽度随 target 分派（Win=2 → 0xFFFF；Linux=4 → 0x10FFFF，
				//         上限取 UTF-32 码点域顶，因码点本身就不超 0x10FFFF、不依赖
				//         sizeof_wchar*8 的理论上界 0xFFFFFFFF）
				//   U  → char32_t 恒 32 位（co_decode_char_wide 产的码点天然不超 0x10FFFF）→ 不校验
				// 修前（rev 58）：L 与 u 同写死 0xFFFF，Linux target 下 wchar_t=4 应容 `L'\U0001F600'`
				//   (0x1F600) UTF-32 码点、旧判错拒；Win target 下行为逐字不变（回归集恒等）。
				uint64_t max_val = 0;
				if (e->char_prefix == "u")                          max_val = 0xFFFF;
				else if (e->char_prefix == "L" && target.sizeof_wchar <= 2) max_val = 0xFFFF;
				else if (e->char_prefix == "L" /* && sizeof_wchar >= 3 */ ) max_val = 0x10FFFF; // UTF-32 码点域顶
				// U 前缀：max_val 保持 0，不触发下面报错
				if (max_val != 0 && static_cast<uint64_t>(e->int_val) > max_val)
					error(t.loc, "character too large for enclosing character literal type");
			}
			return e;
		}
		if (check_punct("("))
		{
			advance();
			auto e = parse_expr();
			expect_punct(")");
			return e;
		}
		error(t.loc, "unexpected token '" + t.text + "' in expression");
		return nullptr;
	}

	// —— 错误恢复 ——
	void skip_to_semi()
	{
		while (!at_end() && !check_punct(";"))
		{
			advance();
		}
		match_punct(";");
	}
	void skip_to_semi_or_brace()
	{
		while (!at_end() && !check_punct(";") && !check_punct("}"))
		{
			advance();
		}
		match_punct(";");
	}
};

}  // namespace

// ============================================================
// 对外接口
// ============================================================
bool parse(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors,
           const std::vector<ImportTagSeed> &seeds,
           const std::vector<ImportConstSeed> &const_seeds)
{
	Parser p(state.tokens, errors);
	p.target = opts.target; // 注入单一目标真值源（数据模型驱动 sizeof/_Alignof）
	// P232：以源文件后缀界定 Co「新面」；仅 .co 启用裸标签作类型，.c（纯 C 档）逐字维持 ISO C。
	{
		const std::string &sp = opts.source_path;
		p.co_mode_ = sp.size() >= 3 &&
			(sp.compare(sp.size() - 3, 3, ".co") == 0 || sp.compare(sp.size() - 3, 3, ".CO") == 0);
		// P233：仅 .co 且传入非空种子时，才把跨模块标签登记为类型种子（.c / 无种子逐字同改前）。
		if (p.co_mode_ && !seeds.empty())
			p.seeds_ = &seeds;
		// P256：仅 .co 且传入非空 const 种子时置位（.c / 无 import / 非种子路径逐字不变）。
		if (p.co_mode_ && !const_seeds.empty())
			p.const_seeds_ = &const_seeds;
	}
	state.ast = p.parse_translation_unit();
	// P214：只有致命错才能中断管线。旧口径 `errors.empty()` 会把任何 warning（包含
	//   预处理 #warning、以及本批新增的 unknown attribute 告警）当成失败、后续层根本不跑。
	//   与 driver/main 的 has_fatal 口径对齐。
	for (const auto &e : errors)
		if (!e.is_warning) return false;
	return true;
}

// P233：顶层 import 预扫入口。用临时 Parser + 独立 scratch 诊断，仅提取顶层 import 声明。
//   调用方（driver）据 root_imports 先 load_imports 采集标签种子、再带种子正式 parse 主源。
void collect_imports(const CompileState &state, std::vector<ImportDecl> &out)
{
	std::vector<Error> scratch;
	Parser p(state.tokens, scratch);
	p.collect_top_level_imports(out);
}

}  // namespace colang
