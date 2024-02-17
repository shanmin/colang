//
//	AST语法分析
//
#pragma once

#include <map>
#include <iostream>


enum AST_TYPE
{
	ast_noncode,
	ast_codeblock,
	ast_function,
	ast_call,
	ast_var,
	ast_expr,
};
std::vector<std::string> AST_TYPE_STRING = {
	"noncode",
	"codeblock",
	"function",
	"call",
	"var",
	"expr",
};

struct AST
{
	AST_TYPE type;
	std::map<std::string, std::vector<TOKEN>> value;
	std::vector<AST> body;
};


//
std::vector<AST> ast_parse_expr(std::vector<TOKEN>& tokens)
{
	std::vector<AST> ast_list;
	while (!tokens.empty())
	{
		if (tokens[0].type == TOKEN_TYPE::string)
		{
			AST a;
			a.type = AST_TYPE::ast_expr;
			a.value["value"].push_back(tokens[0]);
			tokens.erase(tokens.begin());
			ast_list.push_back(a);
			continue;
		}
		else if (tokens[0].type == TOKEN_TYPE::noncode)//非代码，直接输出
		{
			tokens[0].type = TOKEN_TYPE::string;
			TOKEN token_name;
			token_name.col_index = tokens[0].col_index;
			token_name.filename = tokens[0].filename;
			token_name.row_index = tokens[0].row_index;
			token_name.type = TOKEN_TYPE::code;
			token_name.Value = "printf";

			AST ast_args;
			ast_args.type = AST_TYPE::ast_expr;
			ast_args.value["value"].push_back(tokens[0]);

			AST a;
			a.type = AST_TYPE::ast_call;
			a.value["name"].push_back(token_name);
			a.body.push_back(ast_args);
			ast_list.push_back(a);
			tokens.erase(tokens.begin());
			break;
		}

		//代码区结束,直接返回
		if (tokens[0].Value == "}")
			break;
		else if (tokens[0].Value == ")" || tokens[0].Value==";") // 表达式结束
		{
			tokens.erase(tokens.begin());
			break;
		}
		if(tokens[0].Value==",")
		{
			tokens.erase(tokens.begin());
			continue;
		}

		//函数调用
		//Ex:	printf("abc");
		if (tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "(")
		{
			AST a;
			a.type = AST_TYPE::ast_call;
			a.value["name"].push_back(tokens[0]);
			tokens.erase(tokens.begin());

			if (tokens[0].Value == "(")
				tokens.erase(tokens.begin());
			else
				ErrorExit("函数调用解析错误", tokens);

			a.body = ast_parse_expr(tokens);//解析参数

			ast_list.push_back(a);
			if (tokens[0].Value == ";") //分号结尾的退出当前作用域
			{
				tokens.erase(tokens.begin());
				break;
			}
			continue;
		}


		if (tokens[0].Value == "(")
		{
			tokens.erase(tokens.begin());
			AST a;
			a.type = AST_TYPE::ast_expr;
			a.body = ast_parse_expr(tokens);
			//tokens.erase(tokens.begin());
			ast_list.push_back(a);
			continue;
		}

		//表达式
		AST a;
		a.type =AST_TYPE::ast_expr;
		a.value["value"].push_back(tokens[0]);
		tokens.erase(tokens.begin());
		ast_list.push_back(a);
	}
	return ast_list;
}

//语法分析，主要区分代码层次、语句用途
std::vector<AST> ast(std::vector<TOKEN>& tokens)
{
	std::vector<AST> ast_list;
	while (!tokens.empty())
	{
		//代码区结束,直接返回
		if (tokens[0].type != TOKEN_TYPE::string)
		{
			if (tokens[0].Value == "{") //新作用域
			{
				tokens.erase(tokens.begin());
				AST a;
				a.type = AST_TYPE::ast_codeblock;
				a.body = ast(tokens);
				ast_list.push_back(a);
				continue;
			}
			else if (tokens[0].Value == "}")
			{
				tokens.erase(tokens.begin());
				break;
			}
			if (tokens[0].Value == ")") // 表达式结束
			{
				tokens.erase(tokens.begin());
				break;
			}
		}

		//Function
		//	Ex: int function_name(int arg1)
		//		 |		|              + args
		//       |      + 函数名称
		//       +  返回值类型
		//判断依据：当前为字符，下一个token是一个字符，再下一个是(
		if (tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::code && tokens[2].type == TOKEN_TYPE::opcode && tokens[2].Value=="(")
		{
			AST a;
			a.type =AST_TYPE::ast_function;
			//返回值
			a.value["rett"].push_back(tokens[0]);
			tokens.erase(tokens.begin());
			if (tokens[0].type != TOKEN_TYPE::string && (tokens[0].Value == "*" || tokens[0].Value == "&"))
			{
				a.value["rett"].push_back(tokens[0]);
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
		
		//变量定义
		//Ex:	int a;
		//		int b=2;
		if (tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::code && tokens[1].Value != "(")
		{
			if (tokens[2].Value == ";") //变量定义
			{
				AST a;
				a.type = AST_TYPE::ast_var;
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
				a.type = AST_TYPE::ast_var;
				a.value["type"].push_back(tokens[0]);
				tokens.erase(tokens.begin());
				a.value["name"].push_back(tokens[0]);
				ast_list.push_back(a);
				continue;
			}
		}

		//表达式处理
		AST a;
		a.type = AST_TYPE::ast_expr;
		a.body = ast_parse_expr(tokens);
		ast_list.push_back(a);


		////变量赋值
		//if (tokens[1].type != TOKEN_TYPE::string && tokens[1].Value == "=")
		//{
		//	AST a;
		//	a.type =AST_TYPE::ast_expr;
		//	a.value["name"].push_back(tokens[0]);
		//	tokens.erase(tokens.begin());
		//	tokens.erase(tokens.begin());
		//	while (!tokens.empty() && tokens[0].type != TOKEN_TYPE::string && tokens[0].Value != ";")
		//	{
		//		a.value["value"].push_back(tokens[0]);
		//		tokens.erase(tokens.begin());
		//	}
		//	if(!tokens.empty())
		//		tokens.erase(tokens.begin());
		//	ast_list.push_back(a);
		//	continue;
		//}

		////表达式
		//{
		//	AST a;
		//	a.type = ast_expr;
		//	a.value["value"].push_back(tokens[0]);
		//	tokens.erase(tokens.begin());
		//	ast_list.push_back(a);
		//	continue;
		//}

		//ErrorExit("未识别标识", tokens);
		//break;
	}
	return ast_list;
}

void ast_echo(std::vector<AST> ast_list, std::string pre,bool onlyone=false)
{
	for (AST a : ast_list)
	{
		std::cout << pre << "#TYPE: " << a.type<<"[" << AST_TYPE_STRING[a.type]<<"]" << std::endl;
		for (auto it : a.value)
		{
			std::cout << pre << it.first<<":"<<std::endl;
			for (auto t : it.second)
			{
				printf(pre.c_str());
				printf("     [r:%3d,c:%3d]%2d(%s) : %s\n", t.row_index, t.col_index, t.type,TOKEN_TYPE_STRING[t.type].c_str(), t.Value.c_str());
			}
		}
		if (a.body.empty())
			std::cout << std::endl;
		else
		{
			std::cout << pre << "BODY:"<<std::endl;
			ast_echo(a.body, pre + "     ");
			std::cout << std::endl;
		}
		if (onlyone)
			break;
	}
}

//	THE END