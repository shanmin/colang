////////////////////////////////////////////////////////////////////////////////
//
// AST_do
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_do::AST_do(std::vector<TOKEN>& tokens)
{
	//名称
	name = tokens[0];
	tokens.erase(tokens.begin());

	//判断后续是否存在函数体
	body = ast1(tokens);
	if (tokens.empty())
		return;

	if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ";")
		tokens.erase(tokens.begin());

	//参数
	// FIX（2026-09-02，P0#1）：上一步 tokens[0] 不是 ; 且用户没写; tokens 可能空
	if (tokens.empty()) ErrorExit("do: missing 'while(...)'", name);
	if (tokens[0].Value == "while" && tokens.size() > 1 && tokens[1].Value == "(")
	{
		tokens.erase(tokens.begin());
		tokens.erase(tokens.begin());
	}
	else
		ErrorExit("do..while: condition parse error", tokens);

	//解析参数
	expr = ast_parse_expr(tokens);

	// FIX（2026-09-02，P0#1）：ast_parse_expr 消费到 tokens 空时跳过分号检查
	if (!tokens.empty() && tokens[0].Value == ";")
	{
		tokens.erase(tokens.begin());
	}
}
void AST_do::show(std::string pre)
{
	std::cout << pre << "#TYPE:do" << std::endl;
	if (body)
	{
		std::cout << pre << " body:" << std::endl;
		body->show(pre + "      ");
	}
	if (expr)
	{
		std::cout << pre << " expr:" << std::endl;
		expr->show(pre + "      ");
	}
	std::cout << std::endl;
}
llvm::Value* AST_do::codegen()
{
	// do
	//	 code;
	// while(expr)
	//
	// bbbody:
	//	 code;                 <- break -> bbover  ; continue -> bbcond_expr (re-eval expr, 等价跳 expr3 之前)
	// bbcond_expr:
	//	 v = expr
	//	 condbr v, bbbody, bbover
	// bbover:

	llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	llvm::BasicBlock* bbbody = llvm::BasicBlock::Create(ir_context, "do_body", func);
	llvm::BasicBlock* bbcond = llvm::BasicBlock::Create(ir_context, "do_cond", func);
	llvm::BasicBlock* bbover = llvm::BasicBlock::Create(ir_context, "do_over", func);

	// do: continue 语义：跳到 while 条件判定之前（do 无 expr3，"下一轮循环" = 重算条件）
	scope::push_loop_bb(bbover, bbcond);

	ir_builder->CreateBr(bbbody);

	ir_builder->SetInsertPoint(bbbody);
	if (body)
		body->codegen();
	ir_builder->CreateBr(bbcond);

	ir_builder->SetInsertPoint(bbcond);
	if (expr)
	{
		llvm::Value* expr2v = ir_type_conver(expr->codegen(), llvm::Type::getInt1Ty(ir_context));
		ir_builder->CreateCondBr(expr2v, bbbody, bbover);
	}

	ir_builder->SetInsertPoint(bbover);

	scope::pop_loop_bb();
	return nullptr;
}