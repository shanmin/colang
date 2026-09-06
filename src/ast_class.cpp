////////////////////////////////////////////////////////////////////////////////
//
//	AST_class
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"

// 收集方法 token（从当前 tokens 头到匹配的 } 结束，含方法头和 body）
// 传入 tokens 从方法返回类型或构造函数名开始，直到 body 的匹配 } 结束
static std::vector<TOKEN> collect_method_tokens(std::vector<TOKEN>& tokens)
{
	std::vector<TOKEN> result;
	int brace_depth = 0;
	bool found_open_brace = false;
	while (!tokens.empty())
	{
		TOKEN t = tokens[0];
		result.push_back(t);
		tokens.erase(tokens.begin());
		if (t.Value == "{")
		{
			brace_depth++;
			found_open_brace = true;
		}
		else if (t.Value == "}")
		{
			brace_depth--;
			if (found_open_brace && brace_depth == 0)
				break;
		}
	}
	return result;
}

AST_class::AST_class(std::vector<TOKEN>& tokens)
{
	// 可选顶层访问修饰符：public class Name {
	//   private 关键字已移除（2026-09-06）：不写修饰符默认即为私有（类仅当前模块可见）；
	//   显式写 private 由 ast1() 分派入口统一拦截报错，这里做防御性检查。
	//   （顶层 public 的用途：作为 scope::mark_struct_import_public 的开关，决定 import 方能否通过类型名访问。）
	toplevel_public = false;
	if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::code
		&& tokens[0].Value == "private")
	{
		ErrorExit("'private' 关键字已移除：class 默认即为私有（仅当前模块内可见）；需要跨模块公开时请使用 'public class'", tokens[0]);
	}
	if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::code
		&& tokens[0].Value == "public")
	{
		toplevel_public = true;
		tokens.erase(tokens.begin());
	}

	// eat "class"
	if (tokens.empty() || tokens[0].Value != "class")
		ErrorExit("class definition: missing 'class' keyword", tokens);
	tokens.erase(tokens.begin());
	if (tokens.empty()) ErrorExit("class: missing name", TOKEN{});
	name = tokens[0];
	tokens.erase(tokens.begin());

	// 类名与变量名冲突检查（同 struct 规则）
	if (scope::has_var_any_scope(name.Value))
		ErrorExit("class name conflicts with variable name (they share one namespace)", name);

	// eat "{"
	if (tokens.empty()) ErrorExit("class: missing '{'", name);
	if (tokens[0].Value == "{")
		tokens.erase(tokens.begin());
	else
		ErrorExit("class definition: parse error, expected '{'", tokens);

	// 类体解析循环
	while (!tokens.empty())
	{
		if (tokens[0].Value == "}")
		{
			tokens.erase(tokens.begin());
			return;
		}

		// 可选访问修饰符 public（类方法/构造/析构/字段前缀；private 关键字已移除——
		//   不写修饰符默认即为私有：方法 InternalLinkage 仅模块内可见）。
		//   保存下来：AST_function 构造器自己认识 public 前缀；
		//   构造/析构场景也要把修饰符插进 method_tokens 开头。
		bool has_vis = false;
		TOKEN vis_tok;
		if (tokens.size() >= 1 && tokens[0].type == TOKEN_TYPE::code
			&& tokens[0].Value == "private")
		{
			ErrorExit("'private' 关键字已移除：类成员默认即为私有（仅当前模块内可见）；需要公开时请使用 'public'，并删掉 private 修饰符", tokens[0]);
		}
		if (tokens.size() >= 1 && tokens[0].type == TOKEN_TYPE::code
			&& tokens[0].Value == "public")
		{
			has_vis = true;
			vis_tok = tokens[0];
			tokens.erase(tokens.begin());
		}

		// 检测构造函数：[public] ClassName(
		//   无修饰：tokens[0]=name tokens[1]='('
		//   有修饰：tokens[0]=name tokens[1]='('（修饰符已在上方消费）
		if (tokens.size() >= 2
			&& tokens[0].type == TOKEN_TYPE::code && tokens[0].Value == name.Value
			&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "(")
		{
			// 构造函数：前置 void 返回类型 token，使 AST_function 构造器能正常解析
			std::vector<TOKEN> method_tokens;
			// 先放访问修饰符（若有）
			if (has_vis)
				method_tokens.push_back(vis_tok);
			TOKEN void_tok;
			void_tok.type = TOKEN_TYPE::code;
			void_tok.Value = "void";
			void_tok.filename = tokens[0].filename;
			void_tok.row_index = tokens[0].row_index;
			void_tok.col_index = tokens[0].col_index;
			method_tokens.push_back(void_tok);
			// 收集 ClassName ( params ) { body }
			auto rest = collect_method_tokens(tokens);
			method_tokens.insert(method_tokens.end(), rest.begin(), rest.end());
			AST_function* ctor = new AST_function(method_tokens);
			ctor->class_name = name.Value;
			ctor->is_constructor = true;
			methods.push_back(ctor);
			continue;
		}

		// 检测析构函数：[public] ~ ClassName (
		//   tokens 形式：[~] ClassName [( ... ] { body }
		//   有修饰：修饰符已在上方消费，tokens[0]=~ tokens[1]=ClassName tokens[2]=(
		//   无修饰：tokens[0]=~ tokens[1]=ClassName tokens[2]=(
		if (tokens.size() >= 3
			&& tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "~"
			&& tokens[1].type == TOKEN_TYPE::code && tokens[1].Value == name.Value
			&& tokens[2].type == TOKEN_TYPE::opcode && tokens[2].Value == "(")
		{
			// 析构不允许参数：下一个不是 ')' 也必须是空参数（tokens[2]='(' tokens[3]=')'）
			//   先做一个浅校验：把后续 tokens 存到临时容器内扫一遍，允许空白（空格换行由 lexer 消除为 noncode，noncode 类型不会进入 tokens）。
			//   更准确的做法交给 collect_method_tokens 收集完，再 AST_function 构造阶段校验空参数。
			TOKEN tilde = tokens[0];
			// 析构：前置 void 返回类型 token，再把 ~ 和 ClassName 去掉，只留下 ClassName ( params ) { body }，
			//   AST_function 会把 ClassName 当作方法名，然后我们再打标 is_destructor=true。
			std::vector<TOKEN> method_tokens;
			if (has_vis)
				method_tokens.push_back(vis_tok);
			TOKEN void_tok;
			void_tok.type = TOKEN_TYPE::code;
			void_tok.Value = "void";
			void_tok.filename = tilde.filename;
			void_tok.row_index = tilde.row_index;
			void_tok.col_index = tilde.col_index;
			method_tokens.push_back(void_tok);
			// 吃掉 ~，让后续 tokens 变成 ClassName ( ) { body }
			tokens.erase(tokens.begin());
			// 校验 ClassName 存在（若 tokens 空说明语法错误）
			if (tokens.empty() || tokens[0].type != TOKEN_TYPE::code || tokens[0].Value != name.Value)
				ErrorExit("destructor: expected ~ClassName() after '~'", tilde);
			auto rest = collect_method_tokens(tokens);
			method_tokens.insert(method_tokens.end(), rest.begin(), rest.end());
			AST_function* dtor = new AST_function(method_tokens);
			dtor->class_name = name.Value;
			dtor->is_destructor = true;
			// 参数校验（析构不允许带参）放在 AST_function::codegen 入口（args/name 是 private，AST_function 内部可读）
			methods.push_back(dtor);
			continue;
		}

		// 解析字段或方法：先读类型
		std::vector<TOKEN> fld_typ;
		if (tokens.size() >= 1 && tokens[0].type == TOKEN_TYPE::code && tokens[0].Value == "struct")
		{
			// struct T [*] ...
			if (tokens.size() < 2 || tokens[1].type != TOKEN_TYPE::code)
				ErrorExit("class member type: 'struct NAME' missing NAME", tokens);
			fld_typ.push_back(tokens[0]); tokens.erase(tokens.begin());
			fld_typ.push_back(tokens[0]); tokens.erase(tokens.begin());
		}
		else if (tokens.size() >= 1 && tokens[0].type == TOKEN_TYPE::code)
		{
			fld_typ.push_back(tokens[0]); tokens.erase(tokens.begin());
		}
		else
			ErrorExit("class body: type parse error", tokens);

		// 可选指针星号
		while (!tokens.empty() && tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == "*")
		{
			fld_typ.push_back(tokens[0]);
			tokens.erase(tokens.begin());
		}

		// 名称
		if (tokens.empty() || tokens[0].type != TOKEN_TYPE::code)
			ErrorExit("class body: missing field/method name", tokens);
		TOKEN member_name = tokens[0];
		tokens.erase(tokens.begin());

		// 判断字段还是方法
		if (tokens.empty())
			ErrorExit("class member: expected ';' or '('", member_name);

		if (tokens[0].Value == ";")
		{
			// 字段（访问修饰符当前无效，未来可扩展字段可见性）
			tokens.erase(tokens.begin());
			field_types.push_back(fld_typ);
			field_names.push_back(member_name);
		}
		else if (tokens[0].Value == "(")
		{
			// 方法：收集 [vis] [type] [name] ( params ) { body }
			std::vector<TOKEN> method_tokens;
			if (has_vis) method_tokens.push_back(vis_tok);
			for (auto& t : fld_typ) method_tokens.push_back(t);
			method_tokens.push_back(member_name);
			auto rest = collect_method_tokens(tokens);
			method_tokens.insert(method_tokens.end(), rest.begin(), rest.end());
			AST_function* m = new AST_function(method_tokens);
			m->class_name = name.Value;
			methods.push_back(m);
		}
		else
			ErrorExit("class member: parse error, expected ';' or '('", tokens);
	}
}


void AST_class::show(std::string pre)
{
	std::cout << pre << "\033[1m#TYPE:class\033[0m" << std::endl;
	std::cout << pre << " name:";
	token_echo(name, pre);
	std::cout << pre << " fields:\n";
	for (size_t i = 0; i < field_types.size(); i++)
	{
		std::cout << pre << "   [" << i << "] type=";
		for (auto& t : field_types[i]) std::cout << t.Value;
		std::cout << "  name=" << field_names[i].Value << "\n";
	}
	std::cout << pre << " methods:\n";
	for (size_t i = 0; i < methods.size(); i++)
		methods[i]->show(pre + "   ");
}


llvm::Value* AST_class::codegen()
{
	// 1. 创建 StructType + setBody（同 AST_struct::codegen）
	llvm::StructType* structType = scope::get_struct_type(name.Value);
	if (structType) {
		if (!structType->isOpaque())
			ErrorExit("class redefinition", name);
	} else {
		structType = llvm::StructType::create(ir_context, name.Value);
		scope::register_struct_type_forward(structType, name.Value, name);
	}

	std::vector<llvm::Type*> elements;
	for (size_t i = 0; i < field_types.size(); i++)
		elements.push_back(ir_type(field_types[i]));

	structType->setBody(elements, false);
	scope::register_struct_type(structType, name);
	scope::set_struct_field_indexes(structType, field_names);
	// 登记 import 侧类型可见性：仅显式 `public class X` → 允许 import 模块 forward 类型名
	//   未写修饰符（默认私有）→ import 方查不到类型名 → 报 "未定义的类型名"
	scope::mark_struct_import_public(name.Value, toplevel_public);

	// 2. 预声明 malloc/free 外部函数（若尚未声明）
	if (!ir_module->getFunction("malloc"))
	{
		// void* malloc(size_t) — size_t = i64 on 64-bit
		std::vector<llvm::Type*> malloc_args;
		malloc_args.push_back(ir_builder->getInt64Ty());
		llvm::FunctionType* malloc_type = llvm::FunctionType::get(
			llvm::PointerType::get(ir_builder->getInt8Ty(), 0), malloc_args, false);
		llvm::Function::Create(malloc_type, llvm::Function::ExternalLinkage, "malloc", ir_module);
	}
	if (!ir_module->getFunction("free"))
	{
		// void free(void*)
		std::vector<llvm::Type*> free_args;
		free_args.push_back(llvm::PointerType::get(ir_builder->getInt8Ty(), 0));
		llvm::FunctionType* free_type = llvm::FunctionType::get(
			ir_builder->getVoidTy(), free_args, false);
		llvm::Function::Create(free_type, llvm::Function::ExternalLinkage, "free", ir_module);
	}

	// 3. 生成所有方法的 IR（class_name 非空时 AST_function::codegen 自动处理 this + mangle）
	for (auto& m : methods)
		m->codegen();

	return nullptr;
}


//	THE END
