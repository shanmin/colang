////////////////////////////////////////////////////////////////////////////////
//
// AST_expr
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

// 前置声明（struct 字段访问的递归 helper，在 =/复合赋值/运算 分支调用之前先定义实体）
static std::pair<llvm::Value*, llvm::Type*>
get_field_address(llvm::Value* base_addr, llvm::StructType* base_st, const TOKEN& op_tok,
	const std::string& field_name);
// 外部声明的子树解析函数（非 static，允许外部需要时调用）
std::pair<llvm::Value*, llvm::Type*>
ast_expr_field_address_and_type(AST_expr* dot_expr);


//根据 VARINFO 的类型反推类型名字符串（用于 check_literal_assignment_range 的警告文案）。
//如果将来用户自定义类型多了，再把 type_name 直接存进 VARINFO。
static std::string var_type_to_name(llvm::Type* t, bool un)
{
	if (t->isIntegerTy()) {
		unsigned w = t->getIntegerBitWidth();
		if (w == 1)  return "bool";
		if (w == 8)  return un ? "byte" : "char";
		if (w == 16) return un ? "ushort" : "short";
		if (w == 32) return un ? "uint" : "int";
		if (w == 64) return un ? "ulong" : "long";
	}
	else if (t->isHalfTy())   return "half";
	else if (t->isFloatTy())  return "float";
	else if (t->isDoubleTy()) return "double";
	return "unknown";
}

//如果 rhs 是字面量（AST_value + number token），对目标变量做字面量范围检查。
//不做任何语义改动，只打印 warning（方案 B）。
static void check_literal_rhs(const VARINFO& var_info, AST* right)
{
	AST_value* rv = dynamic_cast<AST_value*>(right);
	if (!rv) return;
	if (rv->value.type != TOKEN_TYPE::number) return;
	std::string tname = var_type_to_name(var_info.type, var_info.un);
	check_literal_assignment_range(tname, var_info.type, var_info.un, rv->value);
}

// ============================================================
// struct 字段访问 helper
// ============================================================

// 给定「base addr（alloca or field ptr i8* or struct ptr）」和「base 对应的 struct Type」
//   +「下一级字段名」→ 返回 <field_ptr, field_type>
//   field_ptr = GEP(base_st*, [0, idx])；返回指针类型 field_type*
static std::pair<llvm::Value*, llvm::Type*>
get_field_address(llvm::Value* base_addr, llvm::StructType* base_st, const TOKEN& op_tok,
	const std::string& field_name)
{
	if (!base_addr || !base_st) {
		std::vector<TOKEN> vt; vt.push_back(op_tok);
		ErrorExit("field access: base is not a struct type", vt);
	}

	// 如果 base_addr 类型不是 base_st*（比如是 i8* alloca 返回），cast 回 base_st*
	if (base_addr->getType() != base_st->getPointerTo())
	{
		base_addr = ir_builder->CreateBitCast(base_addr, base_st->getPointerTo());
	}
	unsigned idx = scope::get_struct_field_index(base_st, field_name);
	if (idx == (unsigned)-1) {
		std::vector<TOKEN> vt; vt.push_back(op_tok);
		ErrorExit((std::string("struct has no field: ") + field_name).c_str(), vt);
	}
	llvm::Value* field_addr = ir_builder->CreateStructGEP(base_st, base_addr, idx);
	llvm::Type*   field_ty  = base_st->getElementType(idx);
	return {field_addr, field_ty};
}

// 对一段 AST_expr(op==".") 子树（可能链式 a.b.c.d）递归解析：
//   返回最末级字段的 <地址指针，值类型>（地址即 alloca-slot-like 存储地址，Load 可得字段值）
//   底层 base（最深 left）若是简单变量：scope::get 拿 alloca + type；struct alloca 上 cast 到 struct*
std::pair<llvm::Value*, llvm::Type*> ast_expr_field_address_and_type(AST_expr* dot_expr)
{
	if (!dot_expr || dot_expr->op.Value != ".")
		return {nullptr, nullptr};
	// --- 左：AST_value(code) 或 另一个「.」链 ---
	llvm::Value* base_addr = nullptr;
	llvm::StructType* base_st = nullptr;
	const TOKEN& op_tok = dot_expr->op;
	auto lv = dynamic_cast<AST_value*>(dot_expr->left);
	auto le = dynamic_cast<AST_expr*>(dot_expr->left);
	if (lv && lv->value.type == TOKEN_TYPE::code)
	{
		VARINFO vi = scope::get(lv->value);
		base_addr = vi.value;              // T* alloca slot
		llvm::Type* t = vi.type;
		base_st = llvm::dyn_cast<llvm::StructType>(t);
		// 指针到 struct（如 this 是 ClassName*）：先 load 指针，用 pointee_st 做 GEP
		if (!base_st && vi.pointee_st)
		{
			base_st = vi.pointee_st;
			base_addr = ir_builder->CreateLoad(t, vi.value);  // load 出 ClassName*
		}
		if (!base_st) {
			std::vector<TOKEN> vt; vt.push_back(lv->value);
			ErrorExit((std::string("variable '")+lv->value.Value+"' is not a struct; cannot use '.' to access field").c_str(), vt);
		}
	}
	else if (le && le->op.Value == ".")
	{
		llvm::Type* prev_field_ty = nullptr;
		std::tie(base_addr, prev_field_ty) = ast_expr_field_address_and_type(le);
		base_st = llvm::dyn_cast<llvm::StructType>(prev_field_ty);
		if (!base_st) {
			std::vector<TOKEN> vt; vt.push_back(op_tok);
			ErrorExit("chained field access: intermediate node is not a struct", vt);
		}
	}
	else
	{
		std::vector<TOKEN> vt; vt.push_back(op_tok);
		ErrorExit("field access: left-hand side is not a struct variable/field", vt);
	}

	// --- 右：字段名（纯 code token AST_value）---
	auto rv = dynamic_cast<AST_value*>(dot_expr->right);
	if (!rv || rv->value.type != TOKEN_TYPE::code) {
		std::vector<TOKEN> vt; vt.push_back(op_tok);
		ErrorExit("'.' right-hand side must be a field identifier", vt);
	}
	std::string fname = rv->value.Value;
	return get_field_address(base_addr, base_st, op_tok, fname);
}


//AST_expr::AST_expr(std::vector<TOKEN>& tokens)
//{
//	while (true)
//	{
//		AST_BINARYOP bop;
//		if (tokens[0].type == TOKEN_TYPE::opcode)
//		{
//			if (tokens[0].Value == ";" || tokens[0].Value == ",")
//				break;
//			if (tokens[0].Value == ")")
//			{
//				tokens.erase(tokens.begin());
//				break;
//			}
//			bop.op = tokens[0];
//			bop.op_pri = IR_EXPR_PRI[bop.op.Value];
//			tokens.erase(tokens.begin());
//		}
//		if (tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "(")
//		{
//			tokens.erase(tokens.begin());
//			bop.right = new AST_expr(tokens);
//		}
//		else
//			bop.right = new AST_value(tokens);
//		blist.push_back(bop);
//	}
//}
//AST_expr::AST_expr(AST* left, TOKEN op, AST* right)
//{
//	this->left = left;
//	this->op = op;
//	this->right = right;
//	//填充运算符优先级
//	if (IR_EXPR_PRI.find(op.Value) == IR_EXPR_PRI.end())
//		ErrorExit("未识别的运算符", op);
//	op_pri = IR_EXPR_PRI[op.Value];
//}
void AST_expr::show(std::string pre)
{
	std::cout << pre << "#TYPE:expr" << std::endl;
	std::cout << pre << " left:" << std::endl;
	left->show(pre + "      ");
	std::cout << pre << "   op:";
	token_echo(op, "           ");
	if (right)
	{
		std::cout << pre << "right:" << std::endl;
		right->show(pre + "      ");
	}
	std::cout << std::endl;
}
llvm::Value* AST_expr::codegen()
{
	//	////转换
	//	//ir_value(*current,irinfo);
	//	//只有一个值，不需计算的处理
	//	if (right == NULL)
	//	{
	//		value=current.right_value;
	//		list.erase(list.begin()+index-1);
	//		continue;
	//	}

	//判断赋值操作
	if (op.Value == "=")
	{
		// 左值有两类：1) 简单变量（AST_value code token）：旧实现用 scope::get 拿 alloca；
		//           2) 字段访问 s.f / outer.inner.v（AST_expr op=="."）：需要递归拿字段 alloca 指针再 store。
		// 统一抽象：下面用 helper「左值解析 → <llvm::Value* addr, Type* storety>」；对 AST_value simple var 等价于 scope::get。
		llvm::Value* lhs_addr = nullptr;
		llvm::Type*   lhs_type = nullptr;
		auto lv = dynamic_cast<AST_value*>(left);
		auto le = dynamic_cast<AST_expr*>(left);
		if (lv && lv->value.type == TOKEN_TYPE::code)
		{
			VARINFO vi = scope::get(lv->value);
			lhs_addr = vi.value;
			lhs_type = vi.type;
		}
		else if (le && le->op.Value == ".")
		{
			std::tie(lhs_addr, lhs_type) = ast_expr_field_address_and_type(le);
			if (!lhs_addr || !lhs_type)
				ErrorExit("assignment: left-hand side is not addressable (field access failed)", op);
		}
		else
			ErrorExit("assignment: left-hand side is not assignable", op);

		//字面量赋值范围检查（方案 B：warning，不改退出码）— 只在"简单变量且可拿 VARINFO"时做；字段访问暂时跳过字面量范围检查（避免 VARINFO 找不到）
		if (lv && lv->value.type == TOKEN_TYPE::code) {
			VARINFO var_info = scope::get(lv->value);
			check_literal_rhs(var_info, right);
		}

		llvm::Value* ret = right->codegen();
		//按变量类型转换右侧值（如十六进制字面量按 i64 生成、赋给 i32 变量需截断）
		// FIX（2026-09-02，P0#3）：传 right->is_un() 给 ir_type_conver，
		//   保证 byte(0xFF)→ushort 选 ZExt 而非 SExt、uint(2147483648u)→float 选 UIToFP 而非 SIToFP。
		ret = ir_type_conver(ret, lhs_type, right->is_un());
		ir_builder->CreateStore(ret, lhs_addr);
		return ret;
	}

	//复合赋值：+= -= *= /=，等价 A = A op B
	//  - 左值 = 简单变量 / 字段访问
	if (is_compound_assign(op.Value))
	{
		llvm::Value* lhs_addr = nullptr;
		llvm::Type*   lhs_type = nullptr;
		bool un = left->is_un();
		auto lv = dynamic_cast<AST_value*>(left);
		auto le = dynamic_cast<AST_expr*>(left);
		if (lv && lv->value.type == TOKEN_TYPE::code) {
			VARINFO vi = scope::get(lv->value);
			lhs_addr = vi.value;
			lhs_type = vi.type;
			check_literal_rhs(vi, right);
		}
		else if (le && le->op.Value == ".") {
			std::tie(lhs_addr, lhs_type) = ast_expr_field_address_and_type(le);
			if (!lhs_addr || !lhs_type) ErrorExit("compound assignment: left-hand side is not addressable (field access failed)", op);
			// 字段字面量范围检查略（同简单赋值，避免没有 VARINFO）
		}
		else ErrorExit("compound assignment: left-hand side is not assignable", op);

		llvm::Value* cur = ir_builder->CreateLoad(lhs_type, lhs_addr);
		llvm::Value* r = right->codegen();
		// FIX（2026-09-02，P0#3）：传 right->is_un()，同上避免符号扩展 / SIToFP 误用
		r = ir_type_conver(r, lhs_type, right->is_un());
		llvm::Value* res = NULL;
		bool fp = lhs_type->isFloatingPointTy();
		if (op.Value == "+=") res = fp ? ir_builder->CreateFAdd(cur, r) : ir_builder->CreateAdd(cur, r);
		else if (op.Value == "-=") res = fp ? ir_builder->CreateFSub(cur, r) : ir_builder->CreateSub(cur, r);
		else if (op.Value == "*=") res = fp ? ir_builder->CreateFMul(cur, r) : ir_builder->CreateMul(cur, r);
		else if (op.Value == "/=") res = fp ? ir_builder->CreateFDiv(cur, r) : (un ? ir_builder->CreateUDiv(cur, r) : ir_builder->CreateSDiv(cur, r));
		ir_builder->CreateStore(res, lhs_addr);
		return res;
	}

	llvm::Value* l = nullptr;
	llvm::Value* r = nullptr;

	// "." 字段读：返回字段值的 Load。
	// FIX（2026-09-01）：struct 类型字段旧实现"故意不 Load 返回地址"，但链式场景（o.v.x）实际
	//   走 ast_expr_field_address_and_type 递归（不经过本分支）；本分支唯一服务的是 rvalue 场景
	//   （赋值右侧 f = pp.v / 函数实参 add_vec(pp.v, v2) / return pp.v），返回指针全错：
	//   → ir_type_conver(ptr, Vec) "ERR! type conver!" 退出、call 实参类型不匹配 verify 报错。
	//   修复后统一 Load 返回字段值（struct 字段返回整块 struct 值，供 store/传参/返回使用）。
	if (op.Value == ".")
	{
		llvm::Value* fa = nullptr;
		llvm::Type*   ft = nullptr;
		std::tie(fa, ft) = ast_expr_field_address_and_type(this);
		if (!fa || !ft) ErrorExit("field read failed", op);
		return ir_builder->CreateLoad(ft, fa);
	}

	// === COLOG: COEXPRDBG env 打开时 trace 每个 binary op codegen（前后 InsertBlock） ===
	static const bool _dbg_expr = (std::getenv("COEXPRDBG") != nullptr);
	static std::atomic<int> _dbg_depth{0};
	int _depth = _dbg_depth.fetch_add(1) + 1;
	llvm::BasicBlock* _before_bb = ir_builder->GetInsertBlock();
	std::string _before_name = _before_bb ? (std::string("<")+_before_bb->getName().data()+">") : "<NULL>";
	llvm::Function* _fn = _before_bb ? _before_bb->getParent() : nullptr;
	std::string _fn_name = _fn ? (std::string("fn=")+_fn->getName().data()) : "fn=?";
	if (_dbg_expr) {
		fprintf(stderr, "[D%d] ENTER op='%s' %s BB=%s\n",
			_depth, op.Value.c_str(), _fn_name.c_str(), _before_name.c_str());
		fflush(stderr);
	}
	l = left->codegen();
	llvm::BasicBlock* _afterl_bb = ir_builder->GetInsertBlock();
	std::string _afterl_name = _afterl_bb ? (std::string("<")+_afterl_bb->getName().data()+">") : "<NULL>";
	if (_dbg_expr) {
		fprintf(stderr, "[D%d] after-left op='%s' BB=%s\n", _depth, op.Value.c_str(), _afterl_name.c_str());
		fflush(stderr);
	}
	r = right->codegen();
	llvm::BasicBlock* _afterr_bb = ir_builder->GetInsertBlock();
	std::string _afterr_name = _afterr_bb ? (std::string("<")+_afterr_bb->getName().data()+">") : "<NULL>";
	if (_dbg_expr) {
		fprintf(stderr, "[D%d] after-right op='%s' BB=%s%s\n", _depth, op.Value.c_str(), _afterr_name.c_str(),
			(_afterr_bb != _afterl_bb) ? " <--- BB changed after right!" : "");
		fflush(stderr);
	}
	(void)_before_name; (void)_afterl_name; (void)_afterr_name; (void)_fn_name; (void)_depth;

	//i1 整型提升（模拟 C 的 _Bool 在算术/位/比较运算中提升到 int 的语义）：
	//  逻辑运算符 && || 保留 i1（结果仍是布尔 0/1，CreateAnd/CreateOr 在 i1 上语义正确）；
	//  其余运算符（+ - * / % << >> 比较系列 & |）的 i1 操作数先 zext 到 i32，
	//  避免 1-bit 算术溢出（如 true + true 直接 add i1 1,1 会溢出成 0）。
	//  注：i1↔宽整型 的混合情形由下面"类型不一致提升"分支处理（已含 i1 zext）；
	//      此处只处理"两边都是 i1"或"i1 与其他整型"之前先把 i1 提到 i32，让算术按 int 语义执行。
	if (op.Value != "&&" && op.Value != "||")
	{
		if (l->getType()->isIntegerTy(1))
			l = ir_builder->CreateZExt(l, llvm::Type::getInt32Ty(ir_context));
		if (r->getType()->isIntegerTy(1))
			r = ir_builder->CreateZExt(r, llvm::Type::getInt32Ty(ir_context));
	}

	//操作数类型不一致时自动提升：
	//	整型↔整型：提升到较宽的一方（窄方按自身符号性扩展，字面量默认有符号）
	//	浮点↔浮点：提升到宽的一方（fpext/fptrunc）
	//	整型↔浮点：整型方转浮点（sitofp）
	if (l->getType() != r->getType())
	{
		llvm::Type* lt = l->getType();
		llvm::Type* rt = r->getType();
		bool li = lt->isIntegerTy(), ri = rt->isIntegerTy();
		bool lf = lt->isFloatingPointTy(), rf = rt->isFloatingPointTy();
		if (li && ri)
		{
			unsigned lw = lt->getIntegerBitWidth();
			unsigned rw = rt->getIntegerBitWidth();
			//FIX（2026-09-01）：i1 是比较/逻辑结果（布尔 0/1 语义），扩宽必须零扩展。
			//	旧代码 CreateIntCast(v, to, !is_un()) 走 sext，把 true(1) 扩成 -1：
			//	(1<2)==1 → sext(-1)==1 → false，比较链恒错。
			if (lw > rw)
				r = rt->isIntegerTy(1) ? ir_builder->CreateZExt(r, lt)
					: ir_builder->CreateIntCast(r, lt, !right->is_un());
			else if (rw > lw)
				l = lt->isIntegerTy(1) ? ir_builder->CreateZExt(l, rt)
					: ir_builder->CreateIntCast(l, rt, !left->is_un());
			else ErrorExit("expression type mismatch on both sides", op);
		}
		else if (lf && rf)
		{
			if (lt->getFPMantissaWidth() > rt->getFPMantissaWidth())
				r = ir_builder->CreateFPExt(r, lt);
			else
				l = ir_builder->CreateFPExt(l, rt);
		}
		else if (li && rf)
			l = ir_builder->CreateSIToFP(l, rt);
		else if (lf && ri)
			r = ir_builder->CreateSIToFP(r, lt);
		else
			ErrorExit("expression type mismatch on both sides", op);
	}

	//浮点操作数用 fadd/fcmp 系列指令，整型用 add/icmp 系列
	bool fp = l->getType()->isFloatingPointTy();
	if (fp)
	{
		if (op.Value == "+") return ir_builder->CreateFAdd(l, r);
		else if (op.Value == "-") return ir_builder->CreateFSub(l, r);
		else if (op.Value == "*") return ir_builder->CreateFMul(l, r);
		else if (op.Value == "/") return ir_builder->CreateFDiv(l, r);
		else if (op.Value == ">") return ir_builder->CreateFCmpOGT(l, r);
		else if (op.Value == "<") return ir_builder->CreateFCmpOLT(l, r);
		else if (op.Value == ">=") return ir_builder->CreateFCmpOGE(l, r);
		else if (op.Value == "<=") return ir_builder->CreateFCmpOLE(l, r);
		else if (op.Value == "==") return ir_builder->CreateFCmpOEQ(l, r);
		else if (op.Value == "!=") return ir_builder->CreateFCmpUNE(l, r);
		else ErrorExit("unsupported operator", op);
	}

	//除法与取模、右移、有序比较区分符号性：无符号操作数用 udiv/urem/lshr/icmp u 系列，
	//其余指令（+ - * << & | 与 == != 比较）在 LLVM IR 层不区分符号性，无需分支
	bool un = left->is_un();
	if (op.Value == "+") return ir_builder->CreateAdd(l, r);
	else if (op.Value == "-") return ir_builder->CreateSub(l, r);
	else if (op.Value == "*") return ir_builder->CreateMul(l, r);
	else if (op.Value == "/") return un ? ir_builder->CreateUDiv(l, r) : ir_builder->CreateSDiv(l, r);
	else if (op.Value == "%") return un ? ir_builder->CreateURem(l, r) : ir_builder->CreateSRem(l, r);
	else if (op.Value == "<<") return ir_builder->CreateShl(l, r);
	else if (op.Value == ">>") return un ? ir_builder->CreateLShr(l, r) : ir_builder->CreateAShr(l, r);
	else if (op.Value == ">") return un ? ir_builder->CreateICmpUGT(l, r) : ir_builder->CreateICmpSGT(l, r);
	else if (op.Value == "<") return un ? ir_builder->CreateICmpULT(l, r) : ir_builder->CreateICmpSLT(l, r);
	else if (op.Value == ">=") return un ? ir_builder->CreateICmpUGE(l, r) : ir_builder->CreateICmpSGE(l, r);
	else if (op.Value == "<=") return un ? ir_builder->CreateICmpULE(l, r) : ir_builder->CreateICmpSLE(l, r);
	else if (op.Value == "==") return ir_builder->CreateICmpEQ(l, r);
	else if (op.Value == "!=") return ir_builder->CreateICmpNE(l, r);
	else if (op.Value == "&" or op.Value == "&&") return ir_builder->CreateAnd(l, r);
	else if (op.Value == "|" or op.Value == "||") return ir_builder->CreateOr(l, r);

	ErrorExit("unsupported operator", op);
}
