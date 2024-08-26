////////////////////////////////////////////////////////////////////////////////
//
//	AST_noncode
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"


AST_noncode::AST_noncode(std::vector<TOKEN>& tokens)
{
	value = tokens[0];
	tokens.erase(tokens.begin());
}


void AST_noncode::show(std::string pre)
{
	std::cout << pre << "\033[1m#TYPE:noncode\033[0m" << std::endl;
	token_echo(value, pre + "value:");
	std::cout << std::endl;
}


llvm::Value* AST_noncode::codegen()
{
	llvm::Function* function = ir_module->getFunction("printf");
	if (function)
	{
		llvm::Value* args = ir_builder->CreateGlobalStringPtr(value.Value);
		ir_builder->CreateCall(function, args);
	}
	else
		ErrorExit("未找到非代码输出函数", value);
	return NULL;
}

//	THE END