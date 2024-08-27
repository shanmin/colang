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
	scope::push("codeblock");

	llvm::Value* ret = nullptr;
	for (auto a : body)
		ret=a->codegen();

	scope::pop();

	return ret;
}
