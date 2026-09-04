////////////////////////////////////////////////////////////////////////////////
//
// AST_var
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"


AST_var::AST_var(std::vector<TOKEN>& tokens)
{
	//变量声明现在的形态：
	//   <type> <name>;              — type = 单 token code（int/float/Vec 等 struct 名）
	//   <type>* <name>; / <type>** <name>; — 1+ 颗 *
	//   <mod>.<type> <name> / <mod>.<type>* <name>; — 模块限定类型名（mod 必须是已 import 的模块）
	// 不支持 "struct T name" 两 token（按用户规则），所以 type 里没有「struct + NAME」双 token 分支；
	//   struct NAME 字段类型专用（self forward）只在 AST_struct 字段解析阶段出现，不会走这里的 AST_var 入口。

	// --- 可选：模块前缀 mod.ClassName；消费掉 mod 和 . 两颗，剩下 tokens 开头 = ClassName ---
	//   注意：此处只记录模块前缀、剥皮，**不校验** import 注册表。
	//     原因：AST_var 构造是 parse-time（co2bc 阶段），而 import 的模块名登记在 AST_import::codegen（ir 阶段），
	//           parse 时 ir 还没跑、注册表必然空 → 任何 import 后文件里的 mod.ClassName 都会误判"尚未 import"。
	//     真正校验放在 AST_var::codegen() 入口（codegen 必然发生在 ir() 内部，按 AST_list 顺序：
	//     import AST 在变量 AST 之前完成 codegen → 注册表已填充）。
	if (tokens.size() >= 3
		&& tokens[0].type == TOKEN_TYPE::code
		&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "."
		&& tokens[2].type == TOKEN_TYPE::code)
	{
		mod_prefix_tok = tokens[0];   // 记住 mod 名（含行号，等下 codegen 报错定位）
		// 剥掉 mod + .
		tokens.erase(tokens.begin(), tokens.begin() + 2);
	}

	type.push_back(tokens[0]);
	tokens.erase(tokens.begin());
	// 可选 pointer-star(s)：* 或 &（此处 & 仅用于星号式兼容，原代码当 * 处理）
	while (tokens.size() >= 1 && tokens[0].type != TOKEN_TYPE::string &&
		   (tokens[0].Value == "*" || tokens[0].Value == "&"))
	{
		type.push_back(tokens[0]);
		tokens.erase(tokens.begin());
	}
	//名称
	// FIX（2026-09-02，P0#1）：用户只写 "int;" 或 "int**;"，tokens 空 → tokens[0] UB
	if (tokens.empty()) ErrorExit("variable declaration: missing variable name", type.empty() ? TOKEN{} : type.back());
	name = tokens[0];

	// ---- 可选：= ClassName(args) 栈分配构造初始化式 ----
	//   形态: CLASS c = CLASS(args);  或  mod.CLASS c = mod.CLASS(args);
	//   仅当 = 后紧跟与 type[0] 同名的 ClassName + ( 才由 AST_var 处理；
	//   否则 (= expr) 不消费 name,走 ast_parse_expr 赋值表达式（原有行为不变）。
	bool is_ctor_init = false;
	if (tokens.size() >= 4
		&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "="
		&& tokens[2].type == TOKEN_TYPE::code
		&& tokens[2].Value == type[0].Value
		&& tokens[3].type == TOKEN_TYPE::opcode && tokens[3].Value == "(")
	{
		is_ctor_init = true;
	}
	// mod.ClassName(args) 形式
	else if (tokens.size() >= 6
		&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "="
		&& tokens[2].type == TOKEN_TYPE::code
		&& tokens[3].type == TOKEN_TYPE::opcode && tokens[3].Value == "."
		&& tokens[4].type == TOKEN_TYPE::code
		&& tokens[4].Value == type[0].Value
		&& tokens[5].type == TOKEN_TYPE::opcode && tokens[5].Value == "(")
	{
		is_ctor_init = true;
	}

	if (is_ctor_init)
	{
		TOKEN eq_tok = tokens[1];
		tokens.erase(tokens.begin());      // eat name
		tokens.erase(tokens.begin());      // eat =

		// 可选 mod. 前缀
		if (tokens.size() >= 3
			&& tokens[0].type == TOKEN_TYPE::code
			&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "."
			&& tokens[2].type == TOKEN_TYPE::code)
		{
			init_mod_prefix_tok = tokens[0];
			tokens.erase(tokens.begin(), tokens.begin() + 2);
		}

		// ClassName
		if (tokens.empty() || tokens[0].type != TOKEN_TYPE::code)
			ErrorExit("variable initializer: expected ClassName after '='", eq_tok);
		init_class_name_tok = tokens[0];
		tokens.erase(tokens.begin());

		// 校验:ClassName 必须与 type[0] 一致;mod 前缀必须与声明侧一致
		if (init_class_name_tok.Value != type[0].Value)
			ErrorExit("variable initializer: ClassName mismatch with declared type", init_class_name_tok);
		if (mod_prefix_tok.Value != init_mod_prefix_tok.Value)
			ErrorExit("variable initializer: module qualifier mismatch", init_class_name_tok);

		// eat (
		if (tokens.empty() || tokens[0].Value != "(")
			ErrorExit("variable initializer: expected '(' after ClassName", init_class_name_tok);
		tokens.erase(tokens.begin());

		// 参数解析（同 AST_new L46-77 模式）
		while (!tokens.empty())
		{
			if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ")")
				break;
			init_args.push_back(ast_parse_expr(tokens));
			if (tokens.empty())
				ErrorExit("variable initializer: missing closing ')'", init_class_name_tok);
			if (tokens[0].Value == ",") {
				tokens.erase(tokens.begin());
			}
			else if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ")") {
				tokens.erase(tokens.begin());
				break;
			}
			else if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ";") {
				break;  // ast_parse_expr 已消费 ')'
			}
			else {
				ErrorExit("variable initializer: argument parse error", tokens);
			}
		}
		// eat )
		if (!tokens.empty() && tokens[0].Value == ")")
			tokens.erase(tokens.begin());
		else if (!tokens.empty() && tokens[0].Value != ";")
			ErrorExit("variable initializer: missing closing ')'", init_class_name_tok);
		has_ctor_init = true;
	}
	else if (tokens.size() >= 2 && tokens[1].Value == ";")
	{
		// 原有逻辑:如果后面是 ;,消费 name(让 ; 成为 tokens[0])
		tokens.erase(tokens.begin());
	}

	// 命名共享：变量名 与 已登记 struct tag 同名 → 冲突（ctor 行号更近）
	if (scope::has_struct_type(name.Value)) {
		ErrorExit("variable name conflicts with struct name (they share one namespace)", name);
	}
}

void AST_var::show(std::string pre)
{
	std::cout << pre << "#TYPE:var" << std::endl;
	std::cout << pre << " type:";
	token_echo(type, pre + "      ");
	std::cout << pre << " name:";
	token_echo(name, pre);
	std::cout << std::endl;
}

// ==================== 栈分配构造函数查找 + 调用 ====================
//   复用 AST_new（ast_new.cpp L154-260）的实参 codegen + TCType 合成 + overload_resolve +
//   fallback + 隐式类型提升 + ctor 调用逻辑。
//   obj = this 指针（alloca 出的 struct 存储地址）
//   has_args = true → 有参构造;找不到报错
//   has_args = false → 无参构造;找不到静默跳过（memset 兜底）
static void resolve_and_call_ctor(
	llvm::StructType* st,
	const std::string& class_name,
	const std::string& mod_for_bucket,
	std::vector<AST*>& init_args,
	llvm::Value* obj,
	const TOKEN& diag_tok)
{
	// Step 1：实参 codegen + 合成 TCType（同 AST_new L154-180）
	std::vector<llvm::Value*> user_args;
	std::vector<TCType>        arg_tctypes;
	user_args.reserve(init_args.size());
	arg_tctypes.reserve(init_args.size());
	for (unsigned i = 0; i < init_args.size(); i++)
	{
		llvm::Value* v = init_args[i]->codegen();
		user_args.push_back(v);
		TCType tc;
		bool filled = false;
		AST_value* av = dynamic_cast<AST_value*>(init_args[i]);
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
		if (!filled && v) { tc.ty=v->getType(); tc.un=init_args[i]->is_un(); }
		arg_tctypes.push_back(tc);
	}

	// Step 2：bucket_key + overload_resolve + fallback（同 AST_new L182-200）
	llvm::Function* ctor = nullptr;
	std::string bkey = overload_bucket_key("__init__", class_name, mod_for_bucket);
	bool found_bucket = false;
	std::string resolved_ctor = overload_resolve(bkey, arg_tctypes, diag_tok, &found_bucket);
	if (found_bucket && !resolved_ctor.empty()) {
		ctor = ir_module->getFunction(resolved_ctor);
	}
	if (!ctor) {
		std::string old_ctor = mod_for_bucket.empty()
			? (class_name + ".__init__")
			: (mod_for_bucket + "." + class_name + ".__init__");
		ctor = ir_module->getFunction(old_ctor);
		// 类方法不走 ir_mangle_symbol → LLVM 函数名无模块前缀 → 模块限定时再尝试无前缀名
		if (!ctor && !mod_for_bucket.empty())
			ctor = ir_module->getFunction(class_name + ".__init__");
	}

	// Step 3：调用或报错
	if (ctor)
	{
		std::vector<llvm::Value*> call_args;
		call_args.push_back(obj);
		// 隐式类型提升（同 AST_new L218-252）
		llvm::FunctionType* FT = ctor->getFunctionType();
		unsigned num_params_user = FT->getNumParams();
		for (unsigned i = 0; i < user_args.size() && (i+1) < num_params_user; i++) {
			llvm::Type* expected = FT->getParamType(i+1);
			llvm::Value* &v = user_args[i];
			if (v->getType() != expected) {
				if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
					unsigned ew = expected->getIntegerBitWidth();
					unsigned vw = v->getType()->getIntegerBitWidth();
					if (vw < ew) {
						v = arg_tctypes[i].un
							? ir_builder->CreateZExt(v, expected)
							: ir_builder->CreateSExt(v, expected);
					} else if (vw > ew) {
						v = ir_builder->CreateIntCast(v, expected, !arg_tctypes[i].un);
					}
				} else if (expected->isFloatingPointTy() && v->getType()->isFloatingPointTy()) {
					unsigned ew = expected->getFPMantissaWidth();
					unsigned vw = v->getType()->getFPMantissaWidth();
					if (vw < ew)
						v = ir_builder->CreateFPExt(v, expected);
					else if (vw > ew)
						v = ir_builder->CreateFPTrunc(v, expected);
				}
			}
		}
		for (auto& v : user_args) call_args.push_back(v);
		ir_builder->CreateCall(ctor, call_args);
	}
	else if (!init_args.empty())
	{
		// 有参数但无构造函数 → 报错
		ErrorExit((std::string("variable declaration: class '") + class_name +
			"' has no matching constructor for the provided arguments").c_str(), diag_tok);
	}
	// 无参且找不到 → 静默跳过（memset 兜底）
}

llvm::Value* AST_var::codegen()
{
	// 代码生成开始：若 parse 阶段识别到模块限定（mod.ClassName v = ... 或 mod.ClassName *v），
	//   在这里校验 import 注册表。放在 codegen 而不是构造（parse-time）是因为模块登记发生在
	//   AST_import::codegen，parse 时 import 还没跑，注册表必然为空 → 会误判。
	// 别名解析:mod_prefix 可能是模块别名,解析回原 stem
	std::string mod_stem = import_resolve_module_alias(mod_prefix_tok.Value);
	if (!mod_prefix_tok.Value.empty()) {
		bool is_mod_alias = (mod_stem != mod_prefix_tok.Value);
		if (!is_mod_alias && !import_has_registered_module(mod_prefix_tok.Value)) {
			ErrorExit((std::string("module-qualified type failed: module \"") + mod_prefix_tok.Value + "\" not yet imported or is not a module name").c_str(), mod_prefix_tok);
		}
	}

	VARINFO var_info;
	var_info.token = name;

	// 判断 struct 类型（指针 or 值），在 ir_type 消费 type tokens 之前做
	bool is_struct_ptr = false;
	bool is_struct_val = false;
	llvm::StructType* struct_st = nullptr;
	if (type.size() >= 2 && type[0].type == TOKEN_TYPE::code
		&& type[1].type != TOKEN_TYPE::string && type[1].Value == "*"
		&& scope::has_struct_type(type[0].Value))
	{
		is_struct_ptr = true;
		struct_st = scope::get_struct_type(type[0].Value);
	}
	else if (type.size() == 1 && type[0].type == TOKEN_TYPE::code
		&& scope::has_struct_type(type[0].Value))
	{
		is_struct_val = true;
		struct_st = scope::get_struct_type(type[0].Value);
	}

	// 判断是否有构造函数（通过 ir_module 查 ClassName.__init__）
	std::string class_name = type[0].Value;
	std::string mod_for_ctor = mod_prefix_tok.Value.empty() ? "" : mod_stem;
	std::string ctor_fallback_name = mod_for_ctor.empty()
		? (class_name + ".__init__")
		: (mod_for_ctor + "." + class_name + ".__init__");
	bool has_implicit_ctor = is_struct_val && (ir_module->getFunction(ctor_fallback_name) != nullptr);

	// type 副本（ir_type 会消费 type tokens）
	std::vector<TOKEN> type_copy_for_name = type;

	// ====== 新路径：struct 值类型 + 有构造函数 → 指针槽模式 ======
	if (is_struct_val && (has_implicit_ctor || has_ctor_init))
	{
		// 1. 真实存储 alloca
		llvm::Value* storage = ir_builder->CreateAlloca(struct_st);
		// 2. memset 清零（兜底；构造函数未覆盖的字段至少是 0）
		{
			using namespace llvm;
			Type* i8  = Type::getInt8Ty(ir_context);
			Type* i64 = Type::getInt64Ty(ir_context);
			const DataLayout& DL = ir_module->getDataLayout();
			uint64_t sz = DL.getTypeAllocSize(struct_st).getFixedValue();
			ir_builder->CreateMemSet(storage,
				ConstantInt::get(i8, 0),
				ConstantInt::get(i64, sz),
				MaybeAlign(1));
		}
		// 3. 指针槽 + store
		llvm::Type* ptr_ty = ir_builder->getPtrTy();
		llvm::Value* slot = ir_builder->CreateAlloca(ptr_ty);
		ir_builder->CreateStore(storage, slot);
		// 4. VARINFO 统一为指针槽（与 CLASS *c = new CLASS() 形式一致）
		var_info.type        = ptr_ty;
		var_info.value       = slot;
		var_info.pointee_st   = struct_st;
		var_info.un          = false;
		// type_source_name（用 type 副本拼接，保持与旧逻辑一致）
		{
			std::string tsn;
			if (!mod_prefix_tok.Value.empty()) { tsn += mod_stem; tsn += "."; }
			for (size_t i = 0; i < type_copy_for_name.size(); i++) tsn += type_copy_for_name[i].Value;
			var_info.type_source_name = tsn;
		}
		scope::set(var_info);
		// 5. 调用构造函数
		resolve_and_call_ctor(struct_st, class_name, mod_for_ctor,
			init_args, storage, name);
		return slot;
	}

	// ====== 旧路径：int/float/struct* 或 struct 无构造函数 ======
	var_info.un   = ir_type_unsigned(type[0].Value);
	var_info.type = ir_type(type);
	{
		std::string tsn;
		if (!mod_prefix_tok.Value.empty()) {
			tsn += mod_stem;
			tsn += ".";
		}
		for (size_t i = 0; i < type_copy_for_name.size(); i++) tsn += type_copy_for_name[i].Value;
		var_info.type_source_name = tsn;
	}
	// 对于指向 struct 的指针变量，设置 pointee_st 供字段访问/方法调用使用
	if (is_struct_ptr && struct_st)
		var_info.pointee_st = struct_st;
	var_info.value = ir_builder->CreateAlloca(var_info.type);
	scope::set(var_info);

	// struct 类型（非指针）自动 zero-initialize（MemSet）防 undef 字段读 → UB
	//   scalar（i8..i64 / fp / ptr）保持原有行为（首次 store 前是 undef，与项目现状一致）
	//   struct* p = ... 则指针 alloca i8* 指向槽不 memset，struct 本体另行初始化（当前不做）
	if (var_info.type->isStructTy())
	{
		using namespace llvm;
		LLVMContext& ctx = ir_context;
		Type* i8 = Type::getInt8Ty(ctx);
		Type* i64 = Type::getInt64Ty(ctx);
		Constant* zero_c = ConstantInt::get(i8, 0);
		ConstantInt* zero_val = cast<ConstantInt>(zero_c);
		const DataLayout& DL = ir_module->getDataLayout();
		TypeSize sz = DL.getTypeAllocSize(var_info.type);
		// TypeSize::getFixed() (LLVM 17+) 取 uint64_t（若不是 FixedSizeTypeSize 则 assert），当前 struct 定义是 fixed layout。
		uint64_t sz_bytes = sz.getFixedValue();
		Constant* len_c_c = ConstantInt::get(i64, sz_bytes);
		ConstantInt* len_c = cast<ConstantInt>(len_c_c);
		ir_builder->CreateMemSet(var_info.value, zero_val, len_c, MaybeAlign(1));
	}
	return var_info.value;
}
