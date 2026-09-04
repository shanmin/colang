////////////////////////////////////////////////////////////////////////////////
//
// AST_call
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"

AST_call::AST_call(std::vector<TOKEN>& tokens)
{
	// 模块限定调用：modulename.funcname(args)
	//   tokens[0]=模块名(code) tokens[1]="." tokens[2]=函数名(code) tokens[3]="("
	if (tokens.size() >= 4
		&& tokens[0].type == TOKEN_TYPE::code
		&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "."
		&& tokens[2].type == TOKEN_TYPE::code
		&& tokens[3].type == TOKEN_TYPE::opcode && tokens[3].Value == "(")
	{
		module_prefix = tokens[0].Value;
		tokens.erase(tokens.begin()); // 移除模块名
		tokens.erase(tokens.begin()); // 移除 "."
	}
	//名称
	name = tokens[0];
	tokens.erase(tokens.begin());

	//参数
	// FIX（2026-09-02，P0#1）：erase(name) 后 tokens 可能为空，先判空再访问 tokens[0]
	if (tokens.empty()) ErrorExit("function call: missing '('", name);
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("function call: parse error", tokens);
	//解析参数
	while (!tokens.empty())
	{
		//读取到函数结束，则退出循环
		if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ")")
			break;
		//读取表达式
		args.push_back(ast_parse_expr(tokens));
		// FIX（2026-09-02，P0#1）：ast_parse_expr 可能消费掉闭合 ')' 并把 tokens 弄空，必须先判空再访问 tokens[0]
		if (tokens.empty())
		ErrorExit("function call: missing closing ')'", name);
		if (tokens[0].Value == ",")
			tokens.erase(tokens.begin());
		else if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ")")
		{
			tokens.erase(tokens.begin());
			return;
		}
		else if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ";")
			return;
		else
			ErrorExit("function call: argument parse error (1)", tokens);
	}
	if (tokens.empty())
		ErrorExit("function call: missing closing ')'", name);
	if (tokens[0].Value == ")")
		tokens.erase(tokens.begin());
	else
		ErrorExit("function call: argument parse error", tokens);
}


void AST_call::show(std::string pre)
{
	std::cout << pre << "\033[1m#TYPE:call\033[0m" << std::endl;
	std::cout << pre << " name:";
	token_echo(name, "           ");
	std::cout << pre << " args:";
	bool first = true;
	for (auto a : args)
		if (first)
		{
			a->show("");
			first = false;
		}
		else
			a->show(pre + "      ");
	std::cout << std::endl;
}


llvm::Value* AST_call::codegen()
{
	//函数查找：
	//  module_prefix 非空 → 先检测是否为对象方法调用（obj.method()），再回退到模块限定调用
	//  module_prefix 为空 → 走三步查找链（原名→当前模块mangled→跨模块注册表）
	llvm::Function* function = nullptr;
	bool is_method_call = false;
	llvm::Value* this_ptr = nullptr;
	// 若重载决议命中，记录"用户签名中参数个数（不含类方法的 this、不含 vararg 的省略号部分）"
	//   用于与下方 fixed_args 的值做 sanity 校验（决议选中的 Function 的参数个数是否匹配）
	//   暂不需要（决议返回 mangled 对应 Function 的签名与 entry 参数一致，CreateCall 会天然命中）

	// ========================================================================
	//  Step 5：先把所有用户实参 codegen 完成 + 同时合成 TCType（供 overload_resolve 使用）
	//    顺序：先于 Function 查找执行——因为 overload_resolve 依赖 arg_tctypes_list
	//    注意：用户实参不包括 this（this 是方法调用的注入项）、不包括 vararg C 默认提升的扩展——TCType
	//          取的是 codegen 后 v 的"原类型"：提升后的整型/浮点是扩展类型，但在 overload 语义上我们
	//          仍按"提升前"记（这样传 char 不会误选 int 参数；但当前我们没有提升前类型记录，
	//          所以直接用提升后的 Type。等将来 Step 5 的改进时再细化。现在的副作用是：
	//          传 char 字面量 'a' 给 f(int,char) 可能被解析为提升后 int 然后决议"有 int 参数"，
	//          但只要 f(char) 也定义，implicit_convert_cost("int"→char) 是 -1 不可行，只选中
	//          f(int,char) 的第一个参数是 int，第二个是提升前 char（但提升后是 int，导致 cost 判定失误）。
	//          简单但正确的解法：如果 vararg，我们在合成 TCType 时记录的是「提升前的类型」。
	//          这里先不区分 vararg 提升：统一用 args[i] 的 AST 层信息（AST_value 的字面量类型、
	//          AST_expr 无法拿到的话就用 args[i]->codegen() 返回后的 Value->getType() 合成）。
	// ========================================================================
	std::vector<llvm::Value*> user_arg_values;  // 仅用户参数（不含 this、不含 vararg 扩展、不含 C 提升）
	std::vector<TCType>        arg_tctypes_list;
	user_arg_values.reserve(args.size());
	arg_tctypes_list.reserve(args.size());
	for (unsigned i = 0; i < args.size(); i++)
	{
		llvm::Value* v = args[i]->codegen();
		user_arg_values.push_back(v);

		// 合成 TCType：按 arg AST 的"来源级别"精确性从高到低尝试
		TCType arg_tc;
		bool filled = false;

		// 尝试 1：AST 的 is_un() + 字面量原始 token → 精确合成 source_name
		AST_value* av = dynamic_cast<AST_value*>(args[i]);
		if (av && av->value.type == TOKEN_TYPE::number) {
			// 数字字面量：含 '.' → double，否则 → int（Co 当前默认语义；不支持 u/l/f 后缀）
			const std::string& s = av->value.Value;
			bool is_float = (s.find('.') != std::string::npos);
			arg_tc.ty = v->getType();
			arg_tc.un = false; // 默认字面量都是有符号整型 / 浮点
			arg_tc.source_name = is_float ? std::string("double") : std::string("int");
			filled = true;
		} else if (av && av->value.type == TOKEN_TYPE::string) {
			arg_tc.ty = v->getType();
			arg_tc.un = false;
			arg_tc.source_name = "char*";
			filled = true;
		} else if (av && (av->value.Value == "true" || av->value.Value == "false")) {
			arg_tc.ty = llvm::Type::getInt1Ty(ir_context);
			arg_tc.un = false;
			arg_tc.source_name = "bool";
			filled = true;
		} else if (av && av->value.type == TOKEN_TYPE::code) {
			// 变量名：查 VARINFO 拿 type_source_name
			VARINFO vi;
			bool found_var = false;
			try {
				vi = scope::get(av->value);
				found_var = (vi.value != nullptr || vi.type != nullptr);
			} catch (...) {
				found_var = false;
			}
			if (found_var && !vi.type_source_name.empty()) {
				arg_tc.ty = vi.type;
				arg_tc.un = vi.un;
				arg_tc.source_name = vi.type_source_name;
				filled = true;
			} else if (found_var && vi.type) {
				arg_tc.ty = vi.type;
				arg_tc.un = vi.un;
				filled = true;
			}
		}

		// 尝试 2：无法精确拿到 → 用 codegen 返回的 Value::getType() 兜底
		if (!filled && v) {
			arg_tc.ty = v->getType();
			arg_tc.un = args[i]->is_un();
			// source_name 留空（tctype_equal 会退化为 LLVM Type* 指针相等 + un 比较）
		}

		arg_tctypes_list.push_back(arg_tc);
	}

	if (!module_prefix.empty())
	{
		// 别名解析:module_prefix 可能是模块别名,解析回原 stem 用于 mangle/bucket
		std::string mod_stem = import_resolve_module_alias(module_prefix);

		// 1. 检查 module_prefix 是否是作用域中的变量（对象方法调用 obj.method()）
		if (scope::has_var_any_scope(module_prefix))
		{
			TOKEN prefix_tok;
			prefix_tok.type = TOKEN_TYPE::code;
			prefix_tok.Value = module_prefix;
			prefix_tok.filename = name.filename;
			prefix_tok.row_index = name.row_index;
			prefix_tok.col_index = name.col_index;
			VARINFO vi = scope::get(prefix_tok);
			if (vi.value)
			{
				// 2. 变量类型是否为 struct 或指向 struct 的指针？
				llvm::StructType* st = llvm::dyn_cast<llvm::StructType>(vi.type);
				if (!st && vi.pointee_st)
					st = vi.pointee_st;
				if (st)
				{
					// ================================================================
					//  Step 5 新路径：重载决议 类方法 ClassName##method
					// ================================================================
					std::string class_name = st->getName().str();
					std::string bkey = overload_bucket_key(name.Value, class_name);
					bool found = false;
					std::string resolved = overload_resolve(bkey, arg_tctypes_list, name, &found);
					if (found && !resolved.empty()) {
						llvm::Function* m = ir_module->getFunction(resolved);
						if (m) {
							function = m;
							is_method_call = true;
							this_ptr = ir_builder->CreateLoad(vi.type, vi.value);
						}
					}
					// Fallback（0 回归生命线）：bucket 为空 / 没找到 / resolved 不对，走旧 "ClassName.method" 拼串直查
					if (!function) {
						std::string mangled = class_name + "." + name.Value;
						llvm::Function* m = ir_module->getFunction(mangled);
						if (m) {
							function = m;
							is_method_call = true;
							this_ptr = ir_builder->CreateLoad(vi.type, vi.value);
						}
					}
				}
			}
		}

		// 回退：模块限定调用（前缀不是变量 / 前缀是变量但找不到对应类方法 → 统一走模块名.函数名查找）
		if (!function)
		{
			// 校验:前缀不是变量且不是已注册模块名/别名 → 报错
			bool is_mod_alias = (mod_stem != module_prefix);
			if (!is_mod_alias && !import_has_registered_module(module_prefix)) {
				ErrorExit((std::string("module-qualified call failed: module \"") + module_prefix + "\" not yet imported or is not a module name").c_str(), name);
			}
			// ================================================================
			//  Step 5 新路径：重载决议 模块限定 ModuleName##func
			// ================================================================
			std::string bkey = overload_bucket_key(name.Value, /*class_name=*/"", mod_stem);
			bool found = false;
			std::string resolved = overload_resolve(bkey, arg_tctypes_list, name, &found);
			if (found && !resolved.empty()) {
				function = ir_module->getFunction(resolved);
			}
			// Fallback（0 回归生命线）：旧 "Module.Function" 拼串直查
			if (!function) {
				std::string mangled = mod_stem + "." + name.Value;
				function = ir_module->getFunction(mangled);
				if (!function)
					ErrorExit(("module-qualified function or method not found: " + mangled).c_str(), name);
			}
		}
	}
	else
	{
		// ================================================================
		//  Step 5 新路径：重载决议 普通函数（bucket_key = 函数名本身）
		// ================================================================
		std::string bkey = overload_bucket_key(name.Value, "");
		bool found = false;
		std::string resolved = overload_resolve(bkey, arg_tctypes_list, name, &found);
		if (found && !resolved.empty()) {
			function = ir_module->getFunction(resolved);
		}
		// Fallback（0 回归生命线 + C extern 白名单 printf/scanf/malloc/free 等）：旧三步查找链
		if (!function) {
			function = ir_find_function_or_nul(name.Value);
			if (!function)
				ErrorExit("function definition not found", name);
		}
	}

	// vararg C 默认提升：必须在 codegen 完 user_arg_values 之后再做（与原逻辑一致）。
	//   原逻辑先 CreateCall 前做 vararg 提升；这里保持同样顺序。
	std::vector<llvm::Value*> fargs;
	if (is_method_call)
		fargs.push_back(this_ptr);

	unsigned fixed_args = (unsigned)function->arg_size();
	unsigned arg_offset = is_method_call ? 1 : 0;
	for (unsigned i = 0; i < user_arg_values.size(); i++)
	{
		llvm::Value* v = user_arg_values[i];
		if (function->isVarArg() && (i + arg_offset) >= fixed_args)
		{
			llvm::Type* t = v->getType();
			if (t->isIntegerTy(1))
				v = ir_builder->CreateZExt(v, llvm::Type::getInt32Ty(ir_context));
			else if (t->isIntegerTy() && t->getIntegerBitWidth() < 32)
				v = ir_builder->CreateIntCast(v, llvm::Type::getInt32Ty(ir_context), !args[i]->is_un());
			else if (t->isFloatingPointTy() && !t->isDoubleTy())
				v = ir_builder->CreateFPExt(v, llvm::Type::getDoubleTy(ir_context));
		}
		fargs.push_back(v);
	}
	return ir_builder->CreateCall(function, fargs);
}

//	THE END