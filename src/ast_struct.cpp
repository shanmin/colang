////////////////////////////////////////////////////////////////////////////////
//
//	AST_struct
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"

AST_struct::AST_struct(std::vector<TOKEN>& tokens)
{
	// 可选顶层可见性修饰符：[public|private] struct NAME {
	//   解析（与 AST_class 构造函数同样规则）：显式 public → toplevel_public=true；显式 private/不写 → false
	//   （方案 B 默认不写视为 private：import 方不能用，与 class 行为对称）
	if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::code)
	{
		if (tokens[0].Value == "public") {
			toplevel_public = true;
			tokens.erase(tokens.begin());
		} else if (tokens[0].Value == "private") {
			toplevel_public = false;
			tokens.erase(tokens.begin());
		}
	}
	tokens.erase(tokens.begin());           // eat "struct"
	// FIX（2026-09-02，P0#1）："struct" 后没名字，tokens[0] UB
	if (tokens.empty()) ErrorExit("struct: missing name", TOKEN{});
	name = tokens[0];
	tokens.erase(tokens.begin());           // eat struct-name (code token)

	// 命名共享：struct name 与任何已存在的作用域的变量同名 → 直接报错（ctor 更早、行号更近）
	if (scope::has_var_any_scope(name.Value)) {
		ErrorExit("struct name conflicts with variable name (they share one namespace)", name);
	}

	//参数
	// FIX（2026-09-02，P0#1）：eat struct-name 后空，tokens[0] UB
	if (tokens.empty()) ErrorExit("struct: missing '{'", name);
	if (tokens[0].Value == "{")
		tokens.erase(tokens.begin());
	else
		ErrorExit("struct definition: parse error", tokens);

	//字段解析循环：形如
	//    <类型头> [*] <字段名> ;                       （单-token 类型：int / float / char ...）
	//    struct  <T名> [*] <字段名> ;                  （两-token 类型：struct T）
	//末尾遇到 "}" 时 erase 并 return，"{} 后可选 ; 由外部 ast 循环吃掉"。
	while (!tokens.empty())
	{
		if (tokens[0].Value == "}")
		{
			tokens.erase(tokens.begin());
			return;
		}

		// ----- 解析 <类型> -----
		std::vector<TOKEN> fld_typ;
		if (tokens.size() >= 1 && tokens[0].type == TOKEN_TYPE::code && tokens[0].Value == "struct")
		{
			// struct T [*] ...
			if (tokens.size() < 2 || tokens[1].type != TOKEN_TYPE::code)
				ErrorExit("struct field type: 'struct NAME' missing NAME", tokens);
			fld_typ.push_back(tokens[0]); tokens.erase(tokens.begin());   // struct
			fld_typ.push_back(tokens[0]); tokens.erase(tokens.begin());   // NAME
		}
		else if (tokens.size() >= 1 && tokens[0].type == TOKEN_TYPE::code)
		{
			fld_typ.push_back(tokens[0]); tokens.erase(tokens.begin());   // int/float/...
		}
		else
			ErrorExit("struct field type: parse error", tokens);

		// 可选 pointer-stars："*" 位于类型之后、字段名之前（允许「int* f;」）
		while (tokens.size() >= 1 && tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == "*")
		{
			fld_typ.push_back(tokens[0]);
			tokens.erase(tokens.begin());
		}

		// ----- 字段名 -----
		if (tokens.empty() || tokens[0].type != TOKEN_TYPE::code)
			ErrorExit("struct field: missing name", tokens);
		TOKEN fname = tokens[0];
		tokens.erase(tokens.begin());

		// ----- 必须以 ";" 结尾本字段 -----
		if (tokens.empty() || !(tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == ";"))
			ErrorExit("struct field: missing terminating ';'", tokens);
		tokens.erase(tokens.begin());  // FIX: 之前的版本漏了 erase 这个分号，导致下一轮必报"解析错误2"

		field_types.push_back(fld_typ);
		field_names.push_back(fname);
	}
}


void AST_struct::show(std::string pre)
{
	std::cout << pre << "\033[1m#TYPE:struct\033[0m" << std::endl;
	std::cout << pre << " name:";
	token_echo(name, pre);
	std::cout << pre << "fields:\n";
	for (size_t i = 0; i < field_types.size(); i++)
	{
		std::cout << pre << "   [" << i << "] type=";
		for (auto& t : field_types[i]) std::cout << t.Value;
		std::cout << "  name=" << field_names[i].Value << "\n";
	}
}


llvm::Value* AST_struct::codegen()
{
	// 先把"当前名字"登记/复用 forward：保证接下来字段类型里遇到"struct 同名*" forward，拿的就是同一个 StructType*
	llvm::StructType* structType = scope::get_struct_type(name.Value);
	if (structType) {
		// 已存在（forward opaque）：复用它（同一个指针，后面 self 字段 forward 查 map 拿的就是它）
		if (!structType->isOpaque())
			ErrorExit("struct redefinition", name);
	} else {
		structType = llvm::StructType::create(ir_context, name.Value);
		// 先 forward 登记一次（用当前 name token → 冲突时给出带行/列/文件名的真实诊断），避免下面字段 self-reference 时 ir_type forward 生成另一个同名新指针导致重名。
		scope::register_struct_type_forward(structType, name.Value, name);
	}

	std::vector<llvm::Type*> elements;
	for (size_t i = 0; i < field_types.size(); i++)
	{
		auto typ = field_types[i];
		elements.push_back(ir_type(typ));
	}

	// 落地真实 layout
	structType->setBody(elements, false);

	// 再入正式表：structType 与 forward 登记的指针相同 → register_struct_type 同指针 noop；重名时若不同指针会已报错
	scope::register_struct_type(structType, name);
	scope::set_struct_field_indexes(structType, field_names);
	// struct 与 class 对称（方案 B）：
	//   写了 public struct NAME → toplevel_public=true → import 侧类型名可见；
	//   没写 或 private struct NAME → toplevel_public=false → import 侧类型名隐藏，报 undefined type name。
	scope::mark_struct_import_public(name.Value, toplevel_public);
	return nullptr;
}


//	THE END
