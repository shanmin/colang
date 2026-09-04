////////////////////////////////////////////////////////////////////////////////
//
// AST_return
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_return::AST_return(std::vector<TOKEN>& tokens)
{
	this->token = tokens[0];
	tokens.erase(tokens.begin());
	// FIX（2026-09-02，P0#1）：erase(return) 后 tokens 可能空（return; 合法），tokens[0] UB
	//   return; 的情况：tokens 为空，或下一个就是 ";" → 都不解析表达式。
	if (!tokens.empty() && tokens[0].type != TOKEN_TYPE::string && tokens[0].Value != ";")
		value = ast_parse_expr(tokens);
}


void AST_return::show(std::string pre)
{
	std::cout << pre << "#TYPE:return" << std::endl;
	if (value)
	{
		std::cout << pre << "value:";
		value->show(pre + "      ");
	}
	std::cout << std::endl;
}


llvm::Value* AST_return::codegen()
{
	static const bool _dbg_expr = (std::getenv("COEXPRDBG") != nullptr);
	if (_dbg_expr) {
		llvm::BasicBlock* bb = ir_builder->GetInsertBlock();
		fprintf(stderr, "[DBG] AST_return codegen START: BB=%s  value?=%s\n",
			bb ? bb->getName().data() : "(null)",
			value ? "YES" : "NO");
		fflush(stderr);
	}
	llvm::Value* retv = NULL;
	if (value)
		retv = value->codegen();

	// ===== 析构：提前 return 出口，必须在 CreateRet 前把"当前到 function 入口"所有作用域栈 class 对象反序析构 =====
	scope::generate_all_dtor_calls_to_leave_functions();

	// 注意：**必须 value->codegen() 全部执行完再调用 CreateRet**（不能写 CreateRet(value->codegen()) 这种复合式）。
	//   因为：C++ 函数参数的求值顺序是未定义的，而 LLVM IRBuilder::CreateRet 在 InsertBlock 插入 terminator 可能
	//   发生在"参数求值的子表达式之间"（某些编译器/代码布局下实参右半子树的 codegen 会被放到 CreateRet 之后），
	//   导致 struct-fn 的 return a.x*b.x + a.y*b.y 等复合表达式：左半 (*) 生成后插 Ret，右半 (*) 被塞到
	//   __co__ret_after landing pad → 后半丢失只返回前半。
	// 修复形态：显式两步，1) 表达式 codegen 完成拿到 retv；2) 再 CreateRet(retv)。
	if (retv)
		(void)ir_builder->CreateRet(retv);
	else
		(void)ir_builder->CreateRetVoid();
	if (_dbg_expr) {
		llvm::BasicBlock* bb = ir_builder->GetInsertBlock();
		fprintf(stderr, "[DBG] AST_return after CreateRet: BB=%s (BEFORE switch)\n",
			bb ? bb->getName().data() : "(null)");
		fflush(stderr);
	}

	// 关键修复：当前 basic block 在 CreateRet 之后已经有"终止指令"（terminator）了。
	// 若后续仍有 AST 语句（return 不是写在函数的最后一个语义；或 codeblock 默认补 ret 等），它们继续用同一个 ir_builder
	//   InsertBlock 会把新指令插在 terminator 后面，引发 LLVM Verify：
	//   「Terminator found in the middle of a basic block! label %X」（AST_return.cpp 原注释里就描述过这个坑）
	// 修复方式：CreateRet 之后立刻创建一个新的空 basic block 作为 InsertionPoint，不先挂 terminator。
	//   等函数 codegen 尾部 AST_function 会再做一轮扫描：凡是"仍缺 terminator"的 BB 都补一条 unreachable。
	//   这样可以保证：
	//     1) 新 BB 是空的 → 尾部扫到补 unreachable。
	//     2) 新 BB 被后续语句写入指令、但最后没有 terminator → 尾部扫到补 unreachable。
	//     3) 新 BB 被后续另一条 return/br 再次覆盖一个 terminator → 尾部扫到已经有 terminator 不再重复挂。
	llvm::BasicBlock* current_bb = ir_builder->GetInsertBlock();
	llvm::Function* fn = current_bb ? current_bb->getParent() : nullptr;
	if (fn) {
		llvm::BasicBlock* next_bb = llvm::BasicBlock::Create(ir_context, "__co__ret_after", fn);
		ir_builder->SetInsertPoint(next_bb);
	}
	return NULL;
}

//	THE END