////////////////////////////////////////////////////////////////////////////////
//
// AST_codeblock
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_codeblock::AST_codeblock(std::vector<TOKEN>& tokens)
{
	if (tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "{")
		tokens.erase(tokens.begin());
	body = ast(tokens);
	//while (!tokens.empty())
	//{
	//	if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == "}")
	//		break;
	//	body.push_back(ast1(tokens));
	//}
}
void AST_codeblock::show(std::string pre)
{
	std::cout << pre << "#TYPE:codeblock" << std::endl;
	for (AST* a : body)
	{
		a->show(pre + "      ");
	}
	std::cout << std::endl;
}
llvm::Value* AST_codeblock::codegen()
{
	//设置当前变量作用域
	VAR_LIST varlist;
	varlist.zone = "codeblock";
	ir_varlist.push_back(varlist);

	//llvm::Function* TheFunction = ir_builder->GetInsertBlock()->getParent();
	//llvm::BasicBlock* bb = llvm::BasicBlock::Create(ir_context,"codeblock",TheFunction);
	//ir_builder->SetInsertPoint(bb);
	//llvm::Value* bb = nullptr;

	for (auto a : body)
		a->codegen();

	ir_varlist.pop_back();
	//return bb;
	return nullptr;
}
