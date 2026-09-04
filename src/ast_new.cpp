////////////////////////////////////////////////////////////////////////////////
//
//	AST_new  —  new ClassName(args) 表达式
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"

AST_new::AST_new(std::vector<TOKEN>& tokens)
{
	// eat "new"
	tokens.erase(tokens.begin());
	if (tokens.empty()) ErrorExit("new: missing class name", TOKEN{});

	// --- 可选模块限定：new mod.ClassName(args) ---
	//   parse-time 仅剥皮并保存 mod 名到 mod_prefix_tok，不做 import 注册表校验。
	//   原因同 AST_var：模块名 import_register_module 只在 AST_import::codegen 阶段写，
	//                  构造时（parse 阶段）一定空，检查会误判。
	//   真正 import 校验挪到 codegen() 开头。
	if (tokens.size() >= 3
		&& tokens[0].type == TOKEN_TYPE::code
		&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "."
		&& tokens[2].type == TOKEN_TYPE::code)
	{
		mod_prefix_tok = tokens[0];
		tokens.erase(tokens.begin(), tokens.begin() + 2); // 移除 mod + .
		if (tokens.empty()) ErrorExit("new: missing class name", mod_prefix_tok);
	}
	class_name_tok = tokens[0];
	tokens.erase(tokens.begin());

	// eat "("
	if (tokens.empty()) ErrorExit("new: missing '('", class_name_tok);
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("new: syntax error, expected '('", tokens);

	// 解析参数（同 AST_call 模式）
	//   注意：ast_parse_expr 在解析到 ')' 时会主动消费它（用于 (expr) 嵌套括号语义），
	//   所以参数解析完后 tokens[0] 可能已经是 ')' 之后的 token（如 ';'/','），而非 ')'。
	//   因此循环里需要在 ast_parse_expr 返回后再检查一次后续 token：
	//     - ','：继续下一个参数
	//     - ')'：ast_parse_expr 未消费，这里消费并结束
	//     - ';'：ast_parse_expr 已消费 ')'，遇到语句边界 → 结束
	while (!tokens.empty())
	{
		// 闭合 ')' 在开头 → 空参数列表或参数列表结束
		if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ")")
			break;
		// 读取参数表达式
		args.push_back(ast_parse_expr(tokens));
		// ast_parse_expr 可能已消费闭合 ')'，需判空
		if (tokens.empty())
			ErrorExit("new expression: missing closing ')'", class_name_tok);
		// 下一个 token 决定继续还是结束
		if (tokens[0].Value == ",")
		{
			tokens.erase(tokens.begin());
			// 继续循环解析下一个参数
		}
		else if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ")")
		{
			// ast_parse_expr 未消费 ')'，这里消费并结束
			tokens.erase(tokens.begin());
			return;
		}
		else if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ";")
		{
			// ast_parse_expr 已消费 ')'，遇到 ';' 语句边界 → 结束
			return;
		}
		else
		{
			ErrorExit("new expression: argument parse error", tokens);
		}
	}

	// eat ")"（仅在 while 循环 break 出来时执行，即 tokens[0]==")"）
	if (!tokens.empty() && tokens[0].Value == ")")
		tokens.erase(tokens.begin());
	else if (!tokens.empty())
		ErrorExit("new expression: missing closing ')'", class_name_tok);
}


void AST_new::show(std::string pre)
{
	std::cout << pre << "\033[1m#TYPE:new\033[0m" << std::endl;
	std::cout << pre << " class:";
	token_echo(class_name_tok, pre);
	std::cout << pre << " args:" << std::endl;
	for (auto& a : args)
		a->show(pre + "   ");
}


llvm::Value* AST_new::codegen()
{
	// 代码生成开始：若 parse 时识别了模块限定（new mod.ClassName(...)），
	//   在这里校验 import 注册表（parse 阶段模块尚未 codegen，注册表为空，不能放在构造函数）。
	// 别名解析:mod_prefix 可能是模块别名,解析回原 stem 用于 mangle/bucket
	std::string mod_stem = import_resolve_module_alias(mod_prefix_tok.Value);
	if (!mod_prefix_tok.Value.empty()) {
		bool is_mod_alias = (mod_stem != mod_prefix_tok.Value);
		if (!is_mod_alias && !import_has_registered_module(mod_prefix_tok.Value)) {
			ErrorExit((std::string("module-qualified 'new' failed: module \"") + mod_prefix_tok.Value + "\" not yet imported or is not a module name").c_str(), mod_prefix_tok);
		}
	}
	// 0. 预声明 malloc/free 外部函数（若尚未声明）
	//    AST_class 代码生成时也会声明，但 AST_new 在 import 场景下可能在
	//    AST_class（import 模块的）没被执行之前被调用，所以这里兜底。
	if (!ir_module->getFunction("malloc"))
	{
		std::vector<llvm::Type*> malloc_args;
		malloc_args.push_back(ir_builder->getInt64Ty());
		llvm::FunctionType* malloc_type = llvm::FunctionType::get(
			llvm::PointerType::get(ir_builder->getInt8Ty(), 0), malloc_args, false);
		llvm::Function::Create(malloc_type, llvm::Function::ExternalLinkage, "malloc", ir_module);
	}
	if (!ir_module->getFunction("free"))
	{
		std::vector<llvm::Type*> free_args;
		free_args.push_back(llvm::PointerType::get(ir_builder->getInt8Ty(), 0));
		llvm::FunctionType* free_type = llvm::FunctionType::get(
			ir_builder->getVoidTy(), free_args, false);
		llvm::Function::Create(free_type, llvm::Function::ExternalLinkage, "free", ir_module);
	}

	// 1. 获取类的 StructType
	llvm::StructType* st = scope::get_struct_type(class_name_tok.Value);
	if (!st)
		ErrorExit(("new: class definition not found: " + class_name_tok.Value).c_str(), class_name_tok);

	// 2. malloc(sizeof(class))
	llvm::Function* malloc_fn = ir_module->getFunction("malloc");
	if (!malloc_fn)
		ErrorExit("new: 'malloc' is not declared", class_name_tok);
	uint64_t alloc_size = ir_module->getDataLayout().getTypeAllocSize(st);
	llvm::Value* size_val = ir_builder->getInt64(alloc_size);
	llvm::Value* mem = ir_builder->CreateCall(malloc_fn, {size_val});
	// mem = i8*

	// 3. bitcast 到 ClassName*
	llvm::Type* class_ptr_ty = st->getPointerTo();
	llvm::Value* obj = ir_builder->CreateBitCast(mem, class_ptr_ty);

	// ========================================================================
	//  Step 6：构造函数重载决议（与 AST_call 同样流程：前置实参 codegen + 合成 TCType →
	//           overload_resolve → 找不到 fallback 旧命名 → 没参数不报错就 zero-init）
	// ========================================================================

	// Step 6.1：先把所有用户实参 codegen → 同时合成 TCType
	std::vector<llvm::Value*> user_args;
	std::vector<TCType>        arg_tctypes;
	user_args.reserve(args.size());
	arg_tctypes.reserve(args.size());
	for (unsigned i = 0; i < args.size(); i++)
	{
		llvm::Value* v = args[i]->codegen();
		user_args.push_back(v);
		TCType tc;
		bool filled = false;
		AST_value* av = dynamic_cast<AST_value*>(args[i]);
		if (av && av->value.type == TOKEN_TYPE::number) {
			bool isf = av->value.Value.find('.') != std::string::npos;
			tc.ty = v->getType(); tc.un = false; tc.source_name = isf ? "double" : "int"; filled = true;
		} else if (av && av->value.type == TOKEN_TYPE::string) {
			tc.ty = v->getType(); tc.un = false; tc.source_name = "char*"; filled = true;
		} else if (av && (av->value.Value == "true" || av->value.Value == "false")) {
			tc.ty = llvm::Type::getInt1Ty(ir_context); tc.un = false; tc.source_name = "bool"; filled = true;
		} else if (av && av->value.type == TOKEN_TYPE::code) {
			VARINFO vi; bool fv = false;
			try { vi = scope::get(av->value); fv = (vi.value!=nullptr || vi.type!=nullptr); } catch (...) { fv = false; }
			if (fv && !vi.type_source_name.empty()) { tc.ty=vi.type; tc.un=vi.un; tc.source_name=vi.type_source_name; filled=true; }
			else if (fv && vi.type)                 { tc.ty=vi.type; tc.un=vi.un; filled=true; }
		}
		if (!filled && v) { tc.ty=v->getType(); tc.un=args[i]->is_un(); }
		arg_tctypes.push_back(tc);
	}

	// Step 6.2：构造函数决议 bucket_key = ClassName##__init__
	llvm::Function* ctor = nullptr;
	std::string ctor_class = class_name_tok.Value;
	// 模块限定（new mod.ClassName）：bucket_key 还要带 mod 前缀
	std::string mod_for_bucket = mod_prefix_tok.Value.empty() ? "" : mod_stem;

	std::string bkey = overload_bucket_key("__init__", ctor_class, mod_for_bucket);
	bool found_bucket = false;
	std::string resolved_ctor = overload_resolve(bkey, arg_tctypes, class_name_tok, &found_bucket);
	if (found_bucket && !resolved_ctor.empty()) {
		ctor = ir_module->getFunction(resolved_ctor);
	}
	// Fallback（0 回归生命线）：找不到或 bucket 为空 → 走旧 "ClassName.__init__" 拼串（模块限定时再加 mod. 前缀）
	if (!ctor) {
		std::string old_ctor = mod_prefix_tok.Value.empty()
			? (ctor_class + ".__init__")
			: (mod_stem + "." + ctor_class + ".__init__");
		ctor = ir_module->getFunction(old_ctor);
		// 类方法不走 ir_mangle_symbol → LLVM 函数名无模块前缀 → 模块限定时再尝试无前缀名
		if (!ctor && !mod_prefix_tok.Value.empty())
			ctor = ir_module->getFunction(ctor_class + ".__init__");
	}

	if (ctor)
	{
		std::vector<llvm::Value*> call_args;
		call_args.push_back(obj);
		// 构造函数的 this 已经是首位（不参与重载决议），用户参数顺序与 user_args 一一对应。
		// 注：当前 AST_function codegen 不会对构造函数参数做 ir_type_conver 提升（但 AST_call 里是靠 function arg_size 对齐
		//     + CreateCall 前直接 push——这里同样没有用户层的类型提升，依赖 LLVM FunctionType 匹配 CreateCall。
		//     overload_resolve 已保证用户参数类型兼容；但 LLVM 侧如果实参类型与 ctor FunctionType 形参类型严格不同（
		//     如用户传 short，形参 int），需要手动 CreateIntCast。简单但正确：复用 AST_call 下方的 vararg 提升？
		//     不，vararg 只针对 printf 类。常规隐式提升（short→int、float→double、uint<->int 扩宽）在 AST_call 里其实
		//     没写代码做显式 cast——而是靠 ir_type_conver（哦，AST_call 里没有 ir_type_conver 调用？当前 ast_call codegen 下方
		//     也只对 vararg 参数做了 CreateIntCast / CreateFPExt。所以对非 vararg 参数，CreateCall 时 LLVM 如果实参型 !=
		//     形参型 → 要么 assert，要么 LLVM 自动 cast（LLVM 不允许：不同整型宽 CreateCall 会抛 verify error）。
		//     这是个已有问题，不是 Step 6 引入的（当前 AST_call 没有隐式 cast，12 个测试仍过，是因为用户代码里的实参类型
		//     恰好与 FunctionType 形参一致——如 add_vec(Vec,Vec) 的实参都是 Vec load 结果。）。
		//     为避免 Step 6 引入新隐患：对每个 user_arg，若类型与 ctor 形参不匹配，手动生成 ZExt / SExt / FPExt：
		llvm::FunctionType* FT = ctor->getFunctionType();
		unsigned num_params_user = FT->getNumParams();
		// ctor 的首位参数是 this（Class*），用户参数从 index 1 开始
		for (unsigned i = 0; i < user_args.size() && (i+1) < num_params_user; i++) {
			llvm::Type* expected = FT->getParamType(i+1);
			llvm::Value* &v = user_args[i]; // 引用用于就地 cast
			if (v->getType() != expected) {
				if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
					unsigned ew = expected->getIntegerBitWidth();
					unsigned vw = v->getType()->getIntegerBitWidth();
					if (vw < ew) {
						v = arg_tctypes[i].un
							? ir_builder->CreateZExt(v, expected)
							: ir_builder->CreateSExt(v, expected);
					} else if (vw > ew) {
						// 窄化（收缩）：当前隐式转换不允许，但 overload_resolve 已经在类型层把关；
						//   这里仍防御性 CreateIntCast(Trunc)
						v = ir_builder->CreateIntCast(v, expected, !arg_tctypes[i].un);
					} else { // 等宽不同符号：int↔uint 不处理（i32==i32）
					}
				} else if (expected->isFloatingPointTy() && v->getType()->isFloatingPointTy()) {
					unsigned ew = expected->getFPMantissaWidth();
					unsigned vw = v->getType()->getFPMantissaWidth();
					if (vw < ew)
						v = ir_builder->CreateFPExt(v, expected);
					else if (vw > ew)
						v = ir_builder->CreateFPTrunc(v, expected);
				} else if (expected->isPointerTy() && v->getType()->isPointerTy()) {
					// 指针类型相同（opaque pointer 下都是 ptr）— 无需 cast
				} else if (expected->isStructTy() && v->getType()->isStructTy()) {
					// struct 值类型：直接传（AST_call 里也是直接传）
				}
				// 跨大类（int↔float / struct↔int 等）不做自动 cast，交给 CreateCall 报错暴露问题（正常 overload_resolve 已排除）
			}
		}
		for (auto& v : user_args) call_args.push_back(v);
		ir_builder->CreateCall(ctor, call_args);
	}
	else if (!args.empty())
	{
		// 有参数但无构造函数 → 报错
		ErrorExit(("new: class '" + class_name_tok.Value + "' has no constructor, but arguments were provided").c_str(), class_name_tok);
	}

	return obj;
}


//	THE END
