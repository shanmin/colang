////////////////////////////////////////////////////////////////////////////////
//
// AST_return
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_return::AST_return(std::vector<TOKEN>& tokens)
{
	tokens.erase(tokens.begin());
	if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value != ";")
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
	if (value)
		ir_builder->CreateRet(value->codegen());
	else
		ir_builder->CreateRetVoid();

	//llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	//llvm::BasicBlock* bbexpr = llvm::BasicBlock::Create(ir_context, "return over", func);
	//ir_builder->SetInsertPoint(bbexpr);

	return NULL;
}
