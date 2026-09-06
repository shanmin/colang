////////////////////////////////////////////////////////////////////////////////
//
//	AST_function
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"

AST_function::AST_function(std::vector<TOKEN>& tokens)
{
	// 访问修饰符：可选 public + 返回类型 + 函数名 + ( …
	//   private 关键字已移除（2026-09-06）：不写修饰符默认即为私有（is_private=true，InternalLinkage）；
	//   写了 private 会在 ast1() 分派入口被拦截报错，不会走到这里。
	if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::code
		&& tokens[0].Value == "public")
	{
		is_private = false;
		tokens.erase(tokens.begin());
	}
	//返回值
	// FIX（2026-09-02，P0#1）：修饰符 erase 后 tokens 可能为空，判空再取返回类型 token
	if (tokens.empty()) ErrorExit("function definition: missing return type", TOKEN{});
	rett.push_back(tokens[0]);
	tokens.erase(tokens.begin());
	// FIX（2026-09-02，P0#1）：tokens.erase(rett 首项) 后 tokens 可能为空，判空再访问 tokens[0]
	// 指针返回类型可带多级 *（char* / char** / void*），与 ast1 分派时跳过 [*…] 的逻辑对齐
	while (!tokens.empty() && tokens[0].type != TOKEN_TYPE::string && (tokens[0].Value == "*" || tokens[0].Value == "&"))
	{
		rett.push_back(tokens[0]);
		tokens.erase(tokens.begin());
	}
	//函数名
	// FIX（2026-09-02，P0#1）：判空再取函数名 token
	if (tokens.empty()) ErrorExit("function definition: missing name", rett.empty() ? TOKEN{} : rett.back());
	name = tokens[0];
	tokens.erase(tokens.begin());

	// 命名共享：函数名 与 结构体名 同名 → 冲突（用户规则"结构体名称与变量名称不能重复"广义到任何全局符号）
	if (scope::has_struct_type(name.Value)) {
		ErrorExit("function name conflicts with struct name (they share one namespace)", name);
	}
	//参数
	// FIX（2026-09-02，P0#1）：tokens.empty() 判空避免 tokens[0] UB
	if (tokens.empty()) ErrorExit("function definition: missing '(' for parameter list", name);
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("function definition: parameter list parse error", tokens);

	//解析参数
	while (!tokens.empty())
		if (tokens[0].Value == ")")
		{
			break;
		}
		else if (tokens[0].Value == "...")
		{
			// 可变参数省略号：C 语义要求它前面至少有一个命名参数，且它必须是参数列表最后一项（后面只能是 ')'）
			//   codegen / 重载登记循环靠 args 中的 "..." token 识别 isVarArg，这里只需把好语法关
			if (args.empty())
				ErrorExit("function definition: '...' (variadic) requires at least one named parameter before it", tokens[0]);
			if (tokens.size() < 2 || tokens[1].Value != ")")
				ErrorExit("function definition: '...' (variadic) must be the last parameter", tokens[0]);
			args.push_back(tokens[0]);
			tokens.erase(tokens.begin());
		}
		else
		{
			// 命名共享：参数名 与 结构体名 同名 → 冲突（codegen 时 scope::set(param) 还会再检查一次，这里给更近行号诊断）
			// 注意：参数 token 序列当前是单 token（int/...类型名会跟在后面）所以真正的"参数名"在 "," 或 ")" 之前那一个 code token —— 我们用"下一个 token 非 type code 就是类型名"来识别
			// 由于现在还未修好 U2（struct T 参数 2-token），保持"每 1 token 推入 args"的旧行为；同时对"看起来是参数名（code type、非 ,/*/&/.../;）、且它在 struct 表里"直接报错。
			if (tokens[0].type == TOKEN_TYPE::code
				&& scope::has_struct_type(tokens[0].Value)
				&& tokens.size() >= 2
				&& (tokens[1].Value == "," || tokens[1].Value == ")")) {
				ErrorExit("parameter name conflicts with struct name (they share one namespace)", tokens[0]);
			}
			args.push_back(tokens[0]);
			tokens.erase(tokens.begin());
		}
	//移除)
	// FIX（2026-09-02，P0#1）：while 自然退出可能 tokens 空（缺 ")"），判空再访问
	if (tokens.empty()) ErrorExit("function definition: missing closing ')'", name);
	if (tokens[0].Value == ")")
		tokens.erase(tokens.begin());
	else
		ErrorExit("function definition: closing parse error", tokens);
	//判断后续是否存在函数体
	// FIX（2026-09-02，P0#1）：")" erase 之后 tokens 可能空，先判空
	if (tokens.empty()) ErrorExit("function definition: missing terminating ';' or '{'", name);
	if (tokens[0].Value == ";")
	{
		tokens.erase(tokens.begin());
	}
	else if (tokens[0].Value == "{")
	{
		//tokens.erase(tokens.begin());
		//body = ast(tokens);
		////ErrorExit("函数体部分还未实现", tokens);
		body = new AST_codeblock(tokens);
	}
	else
		ErrorExit("function definition: parse error", tokens);
}


void AST_function::show(std::string pre)
{
	std::cout << pre << "\033[1m#TYPE:function\033[0m" << std::endl;
	std::cout << pre << " rett:";
	token_echo(rett, pre + "      ");
	std::cout << pre << " name:";
	token_echo(name, pre);
	std::cout << pre << " args:";
	token_echo(args, pre + "      ");
	//if (!body.empty())
	if(body)
	{
		std::cout << pre << " body:" << std::endl;
		//for (auto a : body)
		//	//token_echo(body, pre + "      ");
		//	a->show(pre + "    ");
		body->show(pre+"    ");
	}
	std::cout << std::endl;
}

bool AST_function::is_int_main_function_def() {
	// 简单匹配：返回类型是「单个 int」 + 名称 == "main" + 参数为空（用户形式上 args 空 tokens）
	// 这是顶层 ir() 用来做「要不要预建空 main 占位函数」的预检查，要求保守一点：
	//   若匹配不准，顶层层 ir() 仍会建空 main，后续真正 main 被 Create 时得到 main.1 仍然挂掉 → 所以这里需要准确
	// rett 的第一项的 Value == "int" 即认为 int（允许未来 int* 等则不匹配）
	if (rett.size() != 1) return false;
	if (rett[0].Value != "int") return false;
	if (name.Value != "main") return false;
	// 参数：args 可以是空（C 风格 int main()）或 int argc + char** argv（暂时不处理 argv 形，简单都当 main）
	// 这里"严格为空"作为匹配——允许 argc 变体在未来被视为 main 变体：先保守 true，只要 rett=int 且 name=main。
	// 因为就算有 argc 参数，用户写的 int main(int,char**) 仍然是主函数，ir() 开头不能建空 main。
	return true;
}

llvm::Value* AST_function::codegen()
{
	// 析构函数：不允许有用户参数（仅隐式 this）—— AST_class 构造析构节点时校验不到 args/name（private），改在 codegen 入口校验。
	if (is_destructor && !args.empty()) {
		ErrorExit("destructor must have no parameters", name);
	}

	// ================================================================
	//  Step 3（修复版）：codegen 阶段开头做重载 bucket 登记（仅 1 次/函数）
	//    —— 为什么 codegen 开头而不在构造？因为 AST_struct / AST_class 的
	//       scope::register_struct_type 全部发生在 codegen 阶段（晚于 parse）。
	//       放到 codegen 这里时，前面所有 AST_struct/AST_class codegen 都跑完，
	//       ir_type("Vec") 等 struct 参数类型解析必然命中，不会 undefined type name。
	//    —— 只登记 1 次：嵌套 import / 多次 codegen 同一 AST_function（虽罕见）避免同签名重定义报错。
	// ================================================================
	if (!overload_registered) {
		OverloadEntry entry;
		// 构造函数 raw_name 统一替换为 "__init__"（与 ast_new/ast_var 的 overload_bucket_key("__init__", ...) 对齐）
		// 析构函数 raw_name 统一替换为 "__dtor"（与析构调用侧对齐）
		//   否则注册 key = ClassName##ClassName，查找 key = ClassName##__init__/__dtor → 永远找不到桶，靠 fallback 兜底
		//   → 重载函数时 fallback 只返回第一个 → 调错函数
		if (is_constructor)       entry.raw_name = "__init__";
		else if (is_destructor)   entry.raw_name = "__dtor";
		else                      entry.raw_name = name.Value;
		entry.class_name = class_name;
		entry.is_ctor    = is_constructor;
		entry.is_dtor    = is_destructor;

		// ret TCType
		{
			std::vector<TOKEN> rc = rett;
			entry.ret.ty = ir_type(rc);
			for (auto& t : rett) entry.ret.source_name += t.Value;
			entry.ret.un = ir_type_unsigned(rett[0].Value);
		}
		// params TCType：再 parse 一遍 args 副本（与构造中旧代码相同算法，只是移到这里）
		//   用户签名不含 this（类方法的 this 是实现细节，下面 while 后会手动注入 fatype 首位）
		{
			std::vector<TOKEN> a = args;
			while (!a.empty()) {
				if (a[0].Value == ")") break;
				if (a[0].Value == "...") { entry.is_vararg = true; break; }
				if (a[0].Value == ",") { a.erase(a.begin()); continue; }
				std::vector<TOKEN> snap = a;
				size_t before = a.size();
				bool un = ir_type_unsigned(a[0].Value);
				llvm::Type* ty = ir_type(a);
				size_t consumed = before - a.size();
				TCType p;
				p.ty = ty; p.un = un;
				for (size_t k=0; k<consumed; k++) p.source_name += snap[k].Value;
				entry.params.push_back(p);
				if (!a.empty()) a.erase(a.begin());
			}
		}
		// mangled 名：与 codegen 阶段旧命名完全一致（低风险过渡命名）
		bool is_cls = !class_name.empty();
		std::string fn;
		if      (is_cls && is_constructor) fn = class_name + ".__init__";
		else if (is_cls && is_destructor)  fn = class_name + ".__dtor";
		else if (is_cls)                   fn = class_name + "." + name.Value;
		else                               fn = ir_mangle_symbol(name.Value);
		entry.mangled         = fn;
		entry.is_public_entry = !is_private;
		self_mangled_name     = fn;
		overload_register(entry, name);
		overload_registered   = true;
	}

	//args
	llvm::Type* frtype = ir_type(rett);
	std::string fname = name.Value;

	// === 类方法上下文处理 ===
	// 若 class_name 非空：函数名为 "class_name.method_name"（构造函数用 "class_name.__init__"，析构用 "class_name.__dtor"）
	//   并在参数列表首位插入 this 指针（ClassName*）
	bool is_class_method = !class_name.empty();
	if      (is_class_method && is_constructor)  fname = class_name + ".__init__";
	else if (is_class_method && is_destructor)   fname = class_name + ".__dtor";
	else if (is_class_method)                    fname = class_name + "." + fname;

	// 共享名字空间：函数名（全局符号）与 struct tag 同名 → 冲突（scope::set 不拦函数名，因为函数名不入 varlist；所以此处单独拦）
	// 类方法已 mangle 为 ClassName.method，不会与 struct 同名 → 跳过此检查
	if (!is_class_method && scope::has_struct_type(fname)) {
		ErrorExit("function name conflicts with struct name (they share one namespace)", name);
	}

	std::vector<llvm::Type*> fatype;
	std::vector<std::string> faname;
	std::vector<TOKEN> fatoken; //这个与faname配对使用，用于生成变量时保存TOKEN信息
	std::vector<bool> faun;
	// 与 fatype 等长：每个参数的原始类型 token 列表（Point* p → [Point, *]），
	//  供 codegen 阶段识别 "struct T* 参数" → 设置 VARINFO.pointee_st。
	std::vector<std::vector<TOKEN>> params_typetoks;
	bool isVarArg = false;

	// 类方法：首位插入 this 参数（ClassName*）
	if (is_class_method)
	{
		llvm::StructType* class_st = scope::get_struct_type(class_name);
		if (!class_st)
			ErrorExit(("class method: class definition not found: " + class_name).c_str(), name);
		fatype.push_back(class_st->getPointerTo());
		faname.push_back("this");
		TOKEN this_tok;
		this_tok.type = TOKEN_TYPE::code;
		this_tok.Value = "this";
		this_tok.filename = name.filename;
		this_tok.row_index = name.row_index;
		this_tok.col_index = name.col_index;
		fatoken.push_back(this_tok);
		faun.push_back(false);
	}

	while (!args.empty())
		if (args[0].Value == "...")
		{
			isVarArg = true;
			break;
		}
		else if (args[0].Value == ",")
		{
			args.erase(args.begin());
		}
		else
		{
			// 快照 args：ir_type 会消费类型 token，按前后差值拿到这个参数的类型 token 列表
			std::vector<TOKEN> args_snapshot = args;
			size_t size_before = args.size();
			faun.push_back(ir_type_unsigned(args[0].Value));
			fatype.push_back(ir_type(args));
			size_t consumed = size_before - args.size();
			// 提取这个参数的类型 tokens = snapshot 的前 consumed 个
			std::vector<TOKEN> typ_toks;
			for (size_t k = 0; k < consumed; k++)
				typ_toks.push_back(args_snapshot[k]);
			params_typetoks.push_back(typ_toks);
			if (!args.empty())
			{
				faname.push_back(args[0].Value);
				fatoken.push_back(args[0]);
				args.erase(args.begin());
			}
			else
				ErrorExit("ERROR: argument name not set", args);
		}
	llvm::FunctionType* functionType = llvm::FunctionType::get(frtype, fatype, isVarArg);

	// 函数名 mangling：类方法已自行 mangle；普通函数走 ir_mangle_symbol
	std::string mangled = is_class_method ? fname : ir_mangle_symbol(fname);

	// Linkage：main 入口强制 ExternalLinkage；其余按 is_private 区分
	//   - class 方法同样遵守：public → ExternalLinkage（可被 import 的外部模块调用）
	//     private → InternalLinkage（仅当前模块内可见，默认）
	llvm::GlobalValue::LinkageTypes linkage;
	if (mangled == "main")
		linkage = llvm::GlobalValue::ExternalLinkage;
	else if (is_private)
		linkage = llvm::GlobalValue::InternalLinkage;
	else
		linkage = llvm::GlobalValue::ExternalLinkage;
	llvm::Function* function = llvm::Function::Create(functionType, linkage, mangled, ir_module);
	// FIX: 重载函数同名时 LLVM 自动改名（如 Counter.__init__ → Counter.__init__.1）
	//   重载桶里存的是原始名 mangled → overload_resolve 返回的名字与 ir_module 实际名不一致
	//   → getFunction 找到第一个 → 调错函数。这里取 LLVM 实际名回写重载桶最后一条目。
	if (overload_registered) {
		std::string actual = function->getName().str();
		if (actual != mangled) {
			const char* raw = name.Value.c_str();
			if      (is_constructor) raw = "__init__";
			else if (is_destructor)  raw = "__dtor";
			overload_sync_last_mangled(raw, class_name, actual);
			self_mangled_name = actual;
		}
	}
	// llvm::Function* function = llvm::Function::Create(functionType, llvm::GlobalValue::PrivateLinkage, mangled, ir_module);
	//////arg name
	////unsigned i = 0;
	////for (auto& a : function->args())
	////{
	////	if(faname[i]!="")
	////		a.setName(faname[i]);
	////}
	//if (!body.empty())
	if (body)
	{
		//设置当前变量作用域
		scope::push("function");


		//当前标签域
		LABEL_LIST lablelist;
		ir_labellist.push_back(lablelist);

		llvm::BasicBlock* old = ir_builder->GetInsertBlock();
		//创建进入标签
		llvm::BasicBlock* entry = llvm::BasicBlock::Create(ir_context, "", function);
		ir_builder->SetInsertPoint(entry);

		////创建 RETURN 返回值变量
		//VAR_INFO return_val;
		//if (!frtype->isVoidTy())
		//{
		//	return_val.type = frtype;
		//	return_val.value = ir_builder->CreateAlloca(function->getReturnType(), NULL, NULL, "__co__RETURN_VAL");
		//	varlist.info["__co__RETURN_VAL"] = return_val;
		//}
		//ir_varlist.push_back(varlist);

		////创建 RETURN 标签
		//llvm::BasicBlock* bb_return = llvm::BasicBlock::Create(ir_context, "__co__RETURN", function);
		//ir_builder->SetInsertPoint(bb_return);
		////提前插入返回指令
		//if (frtype->isVoidTy())
		//	ir_builder->CreateRetVoid();
		//else
		//{
		//	llvm::Value* v=ir_var_load(return_val);
		//	ir_builder->CreateRet(v);
		//}

		ir_builder->SetInsertPoint(entry);

		//处理函数接收的变量
		llvm::Function::arg_iterator args = function->arg_begin();
		for (int i = 0; i < fatype.size(); i++)
		{
			VARINFO var_info;
			var_info.token = fatoken[i];
			var_info.type = fatype[i];
			var_info.un = faun[i];
			// 识别 struct 指针参数 → 设置 VARINFO.pointee_st
			//   this 参数（类方法 i==0）：从 class_name 查
			//   普通参数：从 params_typetoks[] 识别 struct 名 + * → 查 scope::get_struct_type
			if (is_class_method && i == 0)
			{
				var_info.pointee_st = scope::get_struct_type(class_name);
			}
			else
			{
				int pti = i - (is_class_method ? 1 : 0);
				if (pti >= 0 && pti < (int)params_typetoks.size())
				{
					auto& typ = params_typetoks[pti];
					if (typ.size() >= 2
						&& typ.back().type != TOKEN_TYPE::string && typ.back().Value == "*")
					{
						std::string st_name;
						if (typ.size() >= 3 && typ[0].Value == "struct"
							&& typ[1].type == TOKEN_TYPE::code)
							st_name = typ[1].Value;
						else if (typ[0].type == TOKEN_TYPE::code)
							st_name = typ[0].Value;
						if (!st_name.empty() && scope::has_struct_type(st_name))
							var_info.pointee_st = scope::get_struct_type(st_name);
					}
				}
			}
			var_info.value = ir_builder->CreateAlloca(var_info.type);
			ir_builder->CreateStore(args, var_info.value);
			scope::set(var_info);
			args++;
		}

		//for (auto& a : body)
		//	a->codegen();
		body->codegen();

		//获取当前函数各 basic block 是否已存在至少一条 Ret 指令（如果 user 写了显式 return，但因为 AST_return 之后把 InsertPoint 切到了新的空 BB，当前 BB 查 terminator 会是空，误以为缺 return → 此补丁先全函数扫 Ret）
		//FIX（2026-09-01）：LLVM 23 的 getTerminator() 在块无 terminator 时返回 &InstList.back()（非空！），
		//   判空/判 terminator 必须用 getTerminatorOrNull()；否则空块还会触发 UB。
		bool has_any_ret = false;
		for (llvm::BasicBlock& bb : *function) {
			llvm::Instruction* term = bb.getTerminatorOrNull();
			if (term && llvm::isa<llvm::ReturnInst>(term)) { has_any_ret = true; break; }
		}
		llvm::Instruction* last_instruction = ir_builder->GetInsertBlock() ? ir_builder->GetInsertBlock()->getTerminatorOrNull() : nullptr;
		const bool cur_bb_has_term = last_instruction != NULL && (
			last_instruction->getOpcode() == llvm::Instruction::TermOps::Ret ||
			last_instruction->getOpcode() == llvm::Instruction::TermOps::UncondBr ||
			last_instruction->getOpcode() == llvm::Instruction::TermOps::CondBr);
		// 判断当前 InsertPoint 是否是 AST_return 创建的"着陆块"__co__ret_after（无任何前驱 + 命名匹配）：
		//   这种 BB 是 return 之后的占位空块，不需要补 fallthrough return（也没有析构可做），
		//   后面的统一扫尾会给它挂 unreachable 即可。
		bool cur_is_ret_after_pad = false;
		if (!cur_bb_has_term && ir_builder->GetInsertBlock()) {
			llvm::BasicBlock* cur = ir_builder->GetInsertBlock();
			if (cur->getName().starts_with("__co__ret_after")) {
				// 没有前驱 → 确认为空着陆块
				if (cur->hasNPredecessors(0))
					cur_is_ret_after_pad = true;
			}
		}
		if (!cur_bb_has_term && !cur_is_ret_after_pad)
		{
			// ===== 析构：函数尾无显式 return（或显式 return 后仍有后续语句落在当前 BB，但 BB 无 terminator），
			//   在 CreateRet 前把当前 function 作用域（以及嵌套 codeblock/for 若仍在栈上）的栈对象反序析构。
			//   注：一般嵌套 codeblock/for 在 body codegen 尾已各自 scope::pop 且生成过对应析构；这里主要清理 function 作用域自己声明的栈对象。
			//   背景：不再简单依赖 has_any_ret 跳过补 return：
			//     • has_any_ret=true 但 cur 是真实 fallthrough 末端（无 terminator + 有前驱 / 不是 __co__ret_after）→ 必须补 return + 析构，
			//       否则 fallthrough 会被后段扫成 unreachable，运行触发 STATUS_BREAKPOINT（典型：函数内有 early return if 分支）。
			//     • 返回类型是 aggregate（struct/array/vector 等）无法"零值补 return"：这种情况下如果 has_any_ret，
			//       而 cur 是真实 fallthrough → 理论上 fallthrough 也是非法（struct 没写 return），但为了不影响已有 struct
			//       返回的 legacy 测试（它们在 has_any_ret 时 fallthrough 末端实际上靠 ast.cpp sweep 最后补 ret null / unreachable 兜底），
			//       这里只在 has_any_ret=false 时对非 int/ptr/void 返回报错，has_any_ret=true 时 skip 由 ast.cpp 最后 sweep 兜底。
			scope::generate_all_dtor_calls_to_leave_functions();
			llvm::Type* rty = function->getReturnType();
			if (rty->isVoidTy())
				ir_builder->CreateRetVoid();
			else if (rty->isIntOrPtrTy())
				ir_builder->CreateRet(llvm::Constant::getNullValue(rty));
			else
			{
				// aggregate 等非 primitive：如果函数完全没有写 return → 报错；
				// 如果有 return（但 fallthrough 仍走到这里）→ 保守不报错，跳过补 return，交给尾部 unreachable/ast.cpp 兜底
				if (!has_any_ret)
					ErrorExit("ERROR: function has no return value set", this->name);
			}
		}
		// 再扫一遍函数所有 BB：任何缺少 terminator 的 BB 都挂一个 unreachable。
		//   （典型场景：显式 return 之后 AST_return 切到 __co__ret_after 新 BB，后续语句又写入它，但最后没再 terminator；LLVM Verifier 要求每个 BB 都有 terminator）
		//   注意：同时 ast.cpp 还有模块级 sweep（verifyModule 前）双保险，避免局部漏扫。
		for (llvm::BasicBlock& bb : *function) {
			if (!bb.getTerminatorOrNull()) {
				llvm::IRBuilder<> tmp(ir_context);
				tmp.SetInsertPoint(&bb);
				tmp.CreateUnreachable();
			}
		}

		//清理变量、标签作用域
		scope::pop();
		ir_labellist.pop_back();
		ir_builder->SetInsertPoint(old);
	}
	return function;
}


//	THE END