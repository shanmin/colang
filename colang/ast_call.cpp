////////////////////////////////////////////////////////////////////////////////
//
// AST_call
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"

AST_call::AST_call(std::vector<TOKEN>& tokens)
{
	//名称
	name = tokens[0];
	tokens.erase(tokens.begin());

	//参数
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("函数调用解析错误", tokens);
	//解析参数
	while (!tokens.empty())
	{
		//读取到函数结束，则退出循环
		if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ")")
			break;
		//读取表达式
		args.push_back(ast_parse_expr(tokens));
		if (tokens[0].Value == ",")
			tokens.erase(tokens.begin());
		else if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ")")
		{
			tokens.erase(tokens.begin());
			return;
		}
		else if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ";")
			return;
		else
			ErrorExit("函数调用参数解析错误(1)", tokens);
	}
	if (tokens[0].Value == ")")
		tokens.erase(tokens.begin());
	else
		ErrorExit("函数调用参数解析错误", tokens);
}


void AST_call::show(std::string pre)
{
	std::cout << pre << "\033[1m#TYPE:call\033[0m" << std::endl;
	std::cout << pre << " name:";
	token_echo(name, "           ");
	std::cout << pre << " args:";
	bool first = true;
	for (auto a : args)
		if (first)
		{
			a->show("");
			first = false;
		}
		else
			a->show(pre + "      ");
	std::cout << std::endl;
}


llvm::Value* AST_call::codegen()
{
	//函数调用
	//Ex:	printf("abc");
	std::vector<llvm::Value*> fargs;
	for (auto a : args)
		fargs.push_back(a->codegen());
	llvm::Function* function = ir_module->getFunction(name.Value);
	if (function)
	{
		return ir_builder->CreateCall(function, fargs);
	}
	else
		ErrorExit("未找到函数定义", name);
	return NULL;
}

//	THE END