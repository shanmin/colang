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


//运算数符号性：变量按其声明类型（VARINFO.un），字面量/字符串常量视为有符号
bool AST_value::is_un()
{
	if (value.type == TOKEN_TYPE::code)
	{
		//true/false 是布尔字面量，不是变量，不查 scope（否则报"变量不存在"）
		if (value.Value == "true" || value.Value == "false") return false;
		return scope::get(value).un;
	}
	return false;
}


llvm::Value* AST_value::codegen()
{
	if (value.type == TOKEN_TYPE::string)
		return ir_builder->CreateGlobalString(value.Value);

	if (value.type == TOKEN_TYPE::number)
	{
		//浮点字面量（含小数点），按 double 生成，赋值/运算时再按目标类型转换
		if (value.Value.find('.') != std::string::npos)
			return llvm::ConstantFP::get(ir_context, llvm::APFloat(atof(value.Value.c_str())));

		if (value.Value.size() > 1 && (value.Value[1] == 'X' || value.Value[1] == 'x'))
		{
			std::string str = value.Value.substr(2);
			if (str.size() > 16)
			{
				std::vector<TOKEN> tmp;
				tmp.push_back(value);
				ErrorExit("too big", tmp);
			}
			//必须用 unsigned long long 移位：c2x 返回值若按 char→int 提升后左移，
			//移位数超过 32 位是 UB（如 15 << 60）；无符号 64 位移位才是定义良好的
			unsigned long long data = 0;
			for (int i = 0; i < str.size(); i++)
			{
				long long c = c2x(str[i]);
				if (c == -1)
				{
					std::vector<TOKEN> tmp;
					tmp.push_back(value);
					ErrorExit("error", tmp);
				}
				data |= (unsigned long long)c << ((str.size() - i - 1) * 4);
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

	//true/false 布尔字面量：code token，返回 i1 常量 1/0
	//  放在"查找变量"之前，避免 true/false 被当作变量名查找
	if (value.type == TOKEN_TYPE::code)
	{
		if (value.Value == "true")  return llvm::ConstantInt::getTrue(ir_context);
		if (value.Value == "false") return llvm::ConstantInt::getFalse(ir_context);
	}

	//查找变量
	VARINFO vinfo = scope::get(value);
	//return ir_var_load(vinfo);
	return ir_builder->CreateLoad(vinfo.type, vinfo.value);

	//if(current.right_value==NULL)

//	std::vector<TOKEN> tmp;
//	tmp.push_back(value);
//			ErrorExit("未识别的表达式类型", tmp);
	//}
	//return NULL;
}
