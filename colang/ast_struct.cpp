////////////////////////////////////////////////////////////////////////////////
//
//	AST_struct
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"

AST_struct::AST_struct(std::vector<TOKEN>& tokens)
{
	tokens.erase(tokens.begin());
	name = tokens[0];
	tokens.erase(tokens.begin());
	//参数
	if (tokens[0].Value == "{")
		tokens.erase(tokens.begin());
	else
		ErrorExit("结构体定义解析错误", tokens);

	//解析参数
	while (!tokens.empty())
		if (tokens[0].Value == "}")
		{
			tokens.erase(tokens.begin());
			return;
		}
		else if(tokens.size()>3 && tokens[0].type==TOKEN_TYPE::code && tokens[1].type==TOKEN_TYPE::code && tokens[2].type==TOKEN_TYPE::opcode && tokens[2].Value==";")
		{
			std::vector<TOKEN> vv;
			vv.push_back(tokens[0]);
			tokens.erase(tokens.begin());
			vv.push_back(tokens[0]);
			tokens.erase(tokens.begin());
			value.push_back(vv);
		}
		else
			ErrorExit("结构体定义解析错误2", tokens);
}


void AST_struct::show(std::string pre)
{
	std::cout << pre << "\033[1m#TYPE:struct\033[0m" << std::endl;
	std::cout << pre << " name:";
	token_echo(name, pre);
	std::cout << pre << "value:\n";
	for (int i = 0; i < value.size(); i++)
	{
		std::cout<<pre<<"      " << value[i][0].Value << " " << value[i][1].Value << std::endl;
	}
}


llvm::Value* AST_struct::codegen()
{
	llvm::StructType* structType = llvm::StructType::create(ir_context, name.Value);

	std::vector<llvm::Type*> elements;
	for (int i = 0; i < value.size(); i++)
		elements.push_back(ir_type(value[i]));

	

	return nullptr;
}


//	THE END