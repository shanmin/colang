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

	if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ";")
		tokens.erase(tokens.begin());

	//参数
	if (tokens[0].Value == "while" && tokens.size() > 1 && tokens[1].Value == "(")
	{
		tokens.erase(tokens.begin());
		tokens.erase(tokens.begin());
	}
	else
		ErrorExit("do..while定义参数部分解析错误", tokens);

	//解析参数
	expr = ast_parse_expr(tokens);

	if (tokens[0].Value == ";")
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
	std::cout << pre << " expr:" << std::endl;
	expr->show(pre + "      ");
	std::cout << std::endl;
}
llvm::Value* AST_do::codegen()
{
	// do
	//	 code;
	// while(expr)
	//
	// start:
	//	 code;
	// if(expr)
	//	 goto start;
	// over:

	llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	llvm::BasicBlock* bbbody = llvm::BasicBlock::Create(ir_context, "", func);
	llvm::BasicBlock* bbover = llvm::BasicBlock::Create(ir_context, "", func);

	ir_builder->CreateBr(bbbody);

	ir_builder->SetInsertPoint(bbbody);
	if (body)
		body->codegen();

	llvm::Value* expr2v = ir_type_conver(expr->codegen(), llvm::Type::getInt1Ty(ir_context));
	ir_builder->CreateCondBr(expr2v, bbbody, bbover);

	ir_builder->SetInsertPoint(bbover);

	return nullptr;
}