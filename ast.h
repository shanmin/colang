//
//	AST语法分析
//
#pragma once

#include <map>
#include <vector>
#include "lexer.h"

struct AST
{
	std::string type;
	std::map<std::string, std::vector<TOKEN>> value;
	std::vector<AST> body;
};


//语法分析，主要区分代码层次、语句用途
std::vector<AST> ast(std::vector<TOKEN>& tokens)
{
	std::vector<AST> ast_list;
	while (!tokens.empty())	{
		//代码区结束,直接返回
		if (tokens[0].type != TOKEN_TYPE::string)
			if (tokens[0].Value == "{") //新作用域
			{
				tokens.erase(tokens.begin());
				AST a;
				a.type = "codeblock";
				a.body = ast(tokens);
				ast_list.push_back(a);
				continue;
			}
			else if (tokens[0].Value == "}")
			{
				tokens.erase(tokens.begin());
				break;
			}

		//非代码，直接输出
		if (tokens[0].type == TOKEN_TYPE::noncode)
		{
			AST a;
			a.type = "noncode";
			a.value["value"].push_back(tokens[0]);
			ast_list.push_back(a);
			tokens.erase(tokens.begin());
			continue;
		}

		//Function
		//	Ex: int function_name(int arg1)
		//		 |		|              + args
		//       |      + 函数名称
		//       +  返回值类型
		//判断依据：当前为字符，下一个token是一个字符，再下一个是(
		if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type == TOKEN_TYPE::block && tokens[2].type == TOKEN_TYPE::paren_open)
		{
			AST a;
			a.type = "function";
			//返回值
			a.value["ret"].push_back(tokens[0]);
			tokens.erase(tokens.begin());
			if (tokens[0].type != TOKEN_TYPE::string && (tokens[0].Value == "*" || tokens[0].Value == "&"))
			{
				a.value["ret"].push_back(tokens[0]);
				tokens.erase(tokens.begin());
			}
			//函数名
			a.value["name"].push_back(tokens[0]);
			tokens.erase(tokens.begin());

			if (tokens[0].Value == "(")
				tokens.erase(tokens.begin());
			else
				ErrorExit("函数定义参数部分解析错误", tokens);

			//解析参数
			while (!tokens.empty())
				if (tokens[0].Value == ")")
				{
					break;
				}
				else
				{
					a.value["args"].push_back(tokens[0]);
					tokens.erase(tokens.begin());
				}
			//移除)
			if (tokens[0].Value == ")")
				tokens.erase(tokens.begin());
			else
				ErrorExit("函数定义结束部分错误", tokens);
			//判断后续是否存在函数体
			if (tokens[0].Value == ";")
				tokens.erase(tokens.begin());
			else if (tokens[0].Value == "{")
			{
				tokens.erase(tokens.begin());
				a.body = ast(tokens);
			}
			else
				ErrorExit("函数定义解析错误", tokens);
			ast_list.push_back(a);
			continue;
		}

		//函数调用
		//Ex:	printf("abc");
		if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type == TOKEN_TYPE::paren_open)
		{
			AST a;
			a.type = "call";
			a.value["name"].push_back(tokens[0]);
			tokens.erase(tokens.begin());

			if (tokens[0].Value == "(")
				tokens.erase(tokens.begin());
			else
				ErrorExit("函数调用解析错误", tokens);

			while (!tokens.empty())
			{
				if (tokens[0].Value == ")")
				{
					break;
				}
				a.value["args"].push_back(tokens[0]);
				tokens.erase(tokens.begin());
			}

			if (tokens[0].Value == ")")
				tokens.erase(tokens.begin());
			else
				ErrorExit("函数调用解析错误", tokens);

			if (tokens[0].Value == ";")
				tokens.erase(tokens.begin());
			ast_list.push_back(a);
			continue;
		}
		//变量定义
		//Ex:	int a;
		//		int b=2;
		if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type == TOKEN_TYPE::block)
		{
			if (tokens[2].Value == ";") //变量定义
			{
				AST a;
				a.type = "var";
				a.value["type"].push_back(tokens[0]);
				tokens.erase(tokens.begin());
				a.value["name"].push_back(tokens[0]);
				tokens.erase(tokens.begin());
				tokens.erase(tokens.begin());
				ast_list.push_back(a);
				continue;
			}
			else if (tokens[2].Value == "=")
			{
				AST a;
				a.type = "var";
				a.value["type"].push_back(tokens[0]);
				tokens.erase(tokens.begin());
				a.value["name"].push_back(tokens[0]);
				ast_list.push_back(a);
				continue;
			}
		}

		//变量赋值
		if (tokens[1].type != TOKEN_TYPE::string && tokens[1].Value == "=")
		{
			AST a;
			a.type = "=";
			a.value["name"].push_back(tokens[0]);
			tokens.erase(tokens.begin());
			tokens.erase(tokens.begin());
			while (!tokens.empty() && tokens[0].type != TOKEN_TYPE::string && tokens[0].Value != ";")
			{
				a.value["value"].push_back(tokens[0]);
				tokens.erase(tokens.begin());
			}
			if(!tokens.empty())
				tokens.erase(tokens.begin());
			ast_list.push_back(a);
			continue;
		}

		ErrorExit("未识别标识", tokens);
		break;
	}
	return ast_list;
}

void ast_echo(std::vector<AST> ast_list, std::string pre,bool onlyone=false)
{
	for (AST a : ast_list)
	{
		std::cout << pre << "#TYPE: " << a.type << std::endl;
		std::cout <<pre << "VALUE: "<<std::endl;
		for (auto it : a.value)
		{
			std::cout << pre << "    " << it.first << std::endl;
			for (auto t : it.second)
			{
				printf(pre.c_str());
				printf("        [r:%3d,c:%3d]%2d : %s\n", t.row_index, t.col_index, t.type, t.Value.c_str());
			}
		}
		if (!a.body.empty())
		{
			std::cout << pre << " BODY:";
			ast_echo(a.body, pre + "    ");
			std::cout << std::endl;
		}
		std::cout << std::endl;
		if (onlyone)
			break;
	}
}


//class AST
//{
//public:
//	virtual ~AST() = default;
//	virtual void show(std::string pre)=0;
//};
//
//
//std::vector<AST*> ast(std::vector<TOKEN>& tokens);
//
//
//class AST_id :public AST
//{
//public:
//	std::vector<TOKEN> value;
//	AST_id(std::vector<TOKEN>& tokens);
//	void show(std::string pre) override;
//};
//AST_id::AST_id(std::vector<TOKEN>& tokens)
//{
//	value.push_back(tokens[0]);
//	tokens.erase(tokens.begin());
//}
//void AST_id::show(std::string pre)
//{
//	std::cout << pre << " type:id" << std::endl;
//	token_echo(value, "    ");
//	std::cout << std::endl;
//}
//
//
//class AST_type :public AST
//{
//public:
//	std::vector<TOKEN> value;
//	AST_type(std::vector<TOKEN>& tokens);
//	void show(std::string pre) override;
//};
//AST_type::AST_type(std::vector<TOKEN>& tokens)
//{
//	value.push_back(tokens[0]);
//	tokens.erase(tokens.begin());
//	if (tokens[0].type != TOKEN_TYPE::string && (tokens[0].Value == "*" || tokens[0].Value == "&"))
//	{
//		value.push_back(tokens[0]);
//		tokens.erase(tokens.begin());
//	}
//}
//void AST_type::show(std::string pre)
//{
//	std::cout << pre << "type:type" << std::endl;
//	token_echo(value, "    ");
//	std::cout << std::endl;
//}
//
//class AST_var :public AST
//{
//public:
//	std::string value;
//	AST_var(std::string value) :value(value) {};
//	void show(std::string pre) override;
//};
//void AST_var::show(std::string pre)
//{
//	std::cout << pre << " type:var" << std::endl;
//	std::cout << pre << "value:" << value << std::endl;
//	std::cout << std::endl;
//}
//
//class AST_string :public AST
//{
//public:
//	std::vector<TOKEN> value;
//	AST_string(std::vector<TOKEN> value);
//	void show(std::string pre) override;
//};
//AST_string::AST_string(std::vector<TOKEN> tokens)
//{
//	value.push_back(tokens[0]);
//	tokens.erase(tokens.begin());
//}
//void AST_string::show(std::string pre)
//{
//	std::cout << pre << " type:string" << std::endl;
//	std::cout << pre << "value:" << value[0].Value << std::endl;
//	std::cout << std::endl;
//}
//
//class AST_call :public AST
//{
//public:
//	AST_id* name;
//	std::vector<AST*> args;
//	AST_call(std::vector<TOKEN>& tokens);
//	void show(std::string pre) override;
//};
//AST_call::AST_call(std::vector<TOKEN>& tokens)
//{
//	name = new AST_id(tokens);
//
//	if (tokens[0].Value == "(")
//		tokens.erase(tokens.begin());
//	else
//		ErrorExit("函数调用解析错误", tokens);
//
//	while (tokens[0].Value != ")")
//	{
//		if (tokens[0].type == TOKEN_TYPE::string)
//			args.push_back(new AST_string(tokens));
//		else
//			args.push_back(new AST_id(tokens));
//		tokens.erase(tokens.begin());
//
//		if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ",")
//		{
//			tokens.erase(tokens.begin());
//			continue;
//		}
//	}
//
//	if (tokens[0].Value == ")")
//		tokens.erase(tokens.begin());
//	else
//		ErrorExit("函数调用解析错误", tokens);
//
//	if (tokens[0].Value == ";")
//		tokens.erase(tokens.begin());
//}
//void AST_call::show(std::string pre)
//{
//	std::cout << pre << "type:call" << std::endl;
//	std::cout << pre << "name:"  << std::endl;
//	name->show(pre+"    ");
//	std::cout << pre << "args:" << std::endl;
//	for (auto a : args)
//		a->show("    ");
//	std::cout << std::endl;
//}
//
//class AST_function :public AST
//{
//	AST_type* ret;
//	AST_id* name;
//	std::vector<AST_type*> args_type;
//	std::vector<AST*> args_name;
//	bool isVarArg = false;
//	std::vector<AST*> body;
//public:
//	AST_function(std::vector<TOKEN>& tokens);
//	void show(std::string pre) override;
//};
//AST_function::AST_function(std::vector<TOKEN>& tokens)
//{
//	//函数返回参数
//	ret = new AST_type(tokens);
//	//函数名
//	name = new AST_id(tokens);
//
//	if (tokens[0].Value == "(")
//		tokens.erase(tokens.begin());
//	else
//		ErrorExit("函数定义参数部分解析错误", tokens);
//	
//	//解析参数
//	while (!tokens.empty())
//		if (tokens[0].Value == ")")
//		{
//			break;
//		}
//		else if (tokens[0].Value == "...")
//		{
//			isVarArg = true;
//			tokens.erase(tokens.begin());
//			break;
//		}
//		else if (tokens[0].Value == ",") //下一个参数
//		{
//			tokens.erase(tokens.begin());
//			continue;
//		}
//		else
//		{
//			args_type.push_back(new AST_type(tokens));
//			//读取参数名称
//			if (tokens[0].Value != ")" && tokens[0].Value != ",")
//			{
//				args_name.push_back(new AST_id(tokens));
//			}
//			else
//				ErrorExit("函数定义参数部分解析错误", tokens);
//		}
//	//移除)
//	if (tokens[0].Value == ")")
//		tokens.erase(tokens.begin());
//	else
//		ErrorExit("函数定义结束部分错误", tokens);
//	//判断后续是否存在函数体
//	if (tokens[0].Value == ";")
//		tokens.erase(tokens.begin());
//	else if (tokens[0].Value == "{")
//	{
//		tokens.erase(tokens.begin());
//		body = ast(tokens);
//	}
//	else
//		ErrorExit("函数定义解析错误", tokens);
//
//}
//void AST_function::show(std::string pre)
//{
//	std::cout << pre << "type:function" << std::endl;
//	std::cout << pre << " ret:"  << std::endl;
//	ret->show(pre + "    ");
//	std::cout << pre << "name:" << std::endl;
//	name->show(pre + "    ");
//	std::cout << pre << "args" << std::endl;
//	for (int i = 0; i < args_type.size(); i++)
//	{
//		args_type[i]->show(pre+"    ");
//		args_name[i]->show(pre+"    ");
//	}
//	for (auto a : body)
//		a->show(pre + "    ");
//	std::cout << std::endl;
//}
//
//class AST_noncode:public AST
//{
//	TOKEN value;
//public:
//	AST_noncode(TOKEN value) :value(value) {}
//	void show(std::string pre) override;
//};
//void AST_noncode::show(std::string pre)
//{
//	std::cout << pre << " type:noncode" << std::endl;
//	std::cout << pre << "value:" << value.Value << std::endl;
//	std::cout << std::endl;
//}
//
////语法分析，主要区分代码层次、语句用途
//std::vector<AST*> ast(std::vector<TOKEN>& tokens)
//{
//	std::vector<AST*> ast_list;
//	while (!tokens.empty())//	{
//		//代码区结束,直接返回
//		if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == "}")
//		{
//			tokens.erase(tokens.begin());
//			break;
//		}
//
//		//非代码，直接输出
//		if (tokens[0].type == TOKEN_TYPE::noncode)
//		{
//			AST_noncode* a = new AST_noncode(tokens[0]);
//			ast_list.push_back(a);
//			tokens.erase(tokens.begin());
//			continue;
//		}
//
//		//Function
//		//	Ex: int function_name(int arg1)
//		//		 |		|              + args
//		//       |      + 函数名称
//		//       +  返回值类型
//		//判断依据：当前为字符，下一个token是一个字符，再下一个是(
//		if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type == TOKEN_TYPE::block && tokens[2].type == TOKEN_TYPE::paren_open)
//		{
//			ast_list.push_back(new AST_function(tokens));
//			continue;
//		}
//
//		//函数调用
//		//Ex:	printf("abc");
//		if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type == TOKEN_TYPE::paren_open)
//		{
//			ast_list.push_back(new AST_call(tokens));
//			continue;
//		}
//		////变量定义
//		////Ex:	int a;
//		////		int b=2;
//		//if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type == TOKEN_TYPE::block)
//		//{
//		//	if (tokens[2].Value == ";") //变量定义
//		//	{
//
//		//	}
//		//}
//		ErrorExit("未识别标识", tokens);
//		break;
//	}
//	return ast_list;
//}


//	THE END