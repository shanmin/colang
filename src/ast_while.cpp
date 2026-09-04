////////////////////////////////////////////////////////////////////////////////
//
// AST_while
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_while::AST_while(std::vector<TOKEN>& tokens)
{
	//名称
	name = tokens[0];
	tokens.erase(tokens.begin());
	//参数
	// FIX（2026-09-02，P0#1）：tokens.erase(name) 后空，先判空
	if (tokens.empty()) ErrorExit("while: missing '('", name);
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("while: condition parse error", tokens);

	//解析参数
	expr = ast_parse_expr(tokens);

	//判断后续是否存在函数体
	// FIX（2026-09-02，P0#1）：ast_parse_expr 消费到 tokens 空，tokens[0] UB
	if (tokens.empty()) return;
	if (tokens[0].Value == ";")
	{
		tokens.erase(tokens.begin());
		return;
	}
	body = ast1(tokens);
	if (tokens.empty())
		return;

	if (tokens[0].Value == ";")
	{
		tokens.erase(tokens.begin());
	}
}
void AST_while::show(std::string pre)
{
	std::cout << pre << "#TYPE:while" << std::endl;
	std::cout << pre << " expr:" << std::endl;
	expr->show(pre + "      ");
	if (body)
	{
		std::cout << pre << " body:" << std::endl;
		body->show(pre + "      ");
	}
	std::cout << std::endl;
}
llvm::Value* AST_while::codegen()
{
	//while(expr)
	//	code;
	//
	// bbstart:                       <- continue -> bbstart (重算 expr)
	//	 v = expr -> condbr bbbody, bbover
	// bbbody:
	//	 code;
	//	 br bbstart
	// bbover:                        <- break -> bbover

	llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	llvm::BasicBlock* bbstart = llvm::BasicBlock::Create(ir_context, "while_start", func);
	llvm::BasicBlock* bbbody = llvm::BasicBlock::Create(ir_context, "while_body", func);
	llvm::BasicBlock* bbover = llvm::BasicBlock::Create(ir_context, "while_over", func);

	scope::push_loop_bb(bbover, bbstart);

	ir_builder->CreateBr(bbstart);

	ir_builder->SetInsertPoint(bbstart);
	llvm::Value* expr2v = ir_type_conver(expr->codegen(), llvm::Type::getInt1Ty(ir_context));
	ir_builder->CreateCondBr(expr2v, bbbody, bbover);

	ir_builder->SetInsertPoint(bbbody);
	if (body)
		body->codegen();
	ir_builder->CreateBr(bbstart);

	ir_builder->SetInsertPoint(bbover);

	scope::pop_loop_bb();
	return nullptr;
}
