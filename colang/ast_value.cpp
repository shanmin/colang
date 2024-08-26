////////////////////////////////////////////////////////////////////////////////
//
// AST_value
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

//十六进制字符转换
char c2x(char c)
{
	switch (c)
	{
	case '0':return 0;
	case '1':return 1;
	case '2':return 2;
	case '3':return 3;
	case '4':return 4;
	case '5':return 5;
	case '6':return 6;
	case '7':return 7;
	case '8':return 8;
	case '9':return 9;
	case 'a':
	case 'A':return 10;
	case 'b':
	case 'B':return 11;
	case 'c':
	case 'C':return 12;
	case 'd':
	case 'D':return 13;
	case 'e':
	case 'E':return 14;
	case 'f':
	case 'F':return 15;
	}
	return -1;
}


AST_value::AST_value(std::vector<TOKEN>& tokens)
{
	value = tokens[0];
	tokens.erase(tokens.begin());
}


void AST_value::show(std::string pre)
{
	std::cout << pre << "#TYPE:value";
	token_echo(value, "");
	//std::cout << std::endl;
}


llvm::Value* AST_value::codegen()
{
	if (value.type == TOKEN_TYPE::string)
		return ir_builder->CreateGlobalStringPtr(value.Value);

	if (value.type == TOKEN_TYPE::number)
	{
		if (value.Value.size() > 1 && (value.Value[1] == 'X' || value.Value[1] == 'x'))
		{
			std::string str = value.Value.substr(2);
			if (str.size() > 16)
			{
				std::vector<TOKEN> tmp;
				tmp.push_back(value);
				ErrorExit("too big", tmp);
			}
			long long data = 0;
			for (int i = 0; i < str.size(); i++)
			{
				long long c = c2x(str[i]);
				if (c == -1)
				{
					std::vector<TOKEN> tmp;
					tmp.push_back(value);
					ErrorExit("error", tmp);
				}
				data |= c << ((str.size() - i - 1) * 4);
			}
			return ir_builder->getInt64(data);
		}
		else if (value.Value.size() > 9)
		{
			long long v = atoll(value.Value.c_str());
			return ir_builder->getInt64(v);
		}
		else
		{
			int v = atoi(value.Value.c_str());
			return ir_builder->getInt32(v);
		}
		//return _value;
	}

	//else if (current.right.value.empty()) //没有值节点的，计算下层节点
	//	current.right_value = ir_expr(current.right.body["body"], irinfo);
	//else
	//{

	//查找变量
	VAR_INFO vinfo = ir_var(value.Value, ir_varlist, value);
	return ir_var_load(vinfo);

	//if(current.right_value==NULL)

//	std::vector<TOKEN> tmp;
//	tmp.push_back(value);
//			ErrorExit("未识别的表达式类型", tmp);
	//}
	//return NULL;
}
