//
//	AST语法分析
//
#pragma once

#include <vector>
#include "lexer.h"

enum AST_TYPE
{
	echo, //直接输出
	function,	//函数定义
	function_ret,	//函数返回值类型
	function_name,	//函数名称
	function_args,	//函数参数列表
	function_body,	//函数体
	call,		//函数调用		
	call_args,	//函数调用参数
	codeblock,	//独立代码块，例如{}括起来的代码
};
struct AST_STRUCT
{
	AST_TYPE type;
	std::vector<AST_STRUCT> child;
	std::vector<TOKEN> tokens;
};
//语法分析，主要区分代码层次、语句用途
void ast(std::vector<TOKEN>& tokens, std::vector<AST_STRUCT>& ast_list)
{
	while (!tokens.empty())
	{
		if (tokens[0].type == TOKEN_TYPE::noncode)
		{
			AST_STRUCT a;
			a.type = AST_TYPE::echo;
			a.tokens.push_back(tokens[0]);
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
			AST_STRUCT func;
			func.type = AST_TYPE::function;
			//返回值
			AST_STRUCT ret;
			ret.type = AST_TYPE::function_ret;
			ret.tokens.push_back(tokens[0]);
			func.child.push_back(ret);
			tokens.erase(tokens.begin());
			//名称
			AST_STRUCT name;
			name.type = AST_TYPE::function_name;
			name.tokens.push_back(tokens[0]);
			func.child.push_back(name);
			tokens.erase(tokens.begin());
			//参数开始
			if (tokens[0].type == TOKEN_TYPE::paren_open)
				tokens.erase(tokens.begin());
			else
				ErrorExit("函数定义解析错误", tokens[0]);
			//参数列表
			while (tokens[0].type != TOKEN_TYPE::paren_close) //只要不是)节点，则一直读取
			{
				AST_STRUCT arg;
				arg.type = AST_TYPE::function_args;
				while (tokens[0].type != TOKEN_TYPE::paren_close && tokens[0].type != TOKEN_TYPE::comma)
				{
					arg.tokens.push_back(tokens[0]);
					tokens.erase(tokens.begin());
				}
				func.child.push_back(arg);
				//移除已处理的逗号
				if (tokens[0].type == TOKEN_TYPE::comma)
					tokens.erase(tokens.begin());
			}
			//移除)
			if (tokens[0].Value == ")")
				tokens.erase(tokens.begin());
			else
				ErrorExit("函数定义结束部分错误", tokens[0]);
			//判断后续是否存在函数体
			if (tokens[0].Value == ";")
				tokens.erase(tokens.begin());
			else if (tokens[0].Value == "{")
			{
				AST_STRUCT body;
				body.type = AST_TYPE::function_body;
				ast(tokens, body.child);
				func.child.push_back(body);
			}

			ast_list.push_back(func);
			continue;
		}
		//函数调用
		//Ex:	printf("abc");
		if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type == TOKEN_TYPE::paren_open)
		{
			AST_STRUCT call;
			call.type = AST_TYPE::call;
			//名称
			AST_STRUCT name;
			name.type = AST_TYPE::function_name;
			name.tokens.push_back(tokens[0]);
			call.child.push_back(name);
			tokens.erase(tokens.begin());
			//参数开始
			if (tokens[0].type == TOKEN_TYPE::paren_open)
				tokens.erase(tokens.begin());
			else
				ErrorExit("函数调用解析错误", tokens[0]);
			//参数列表
			while (tokens[0].type != TOKEN_TYPE::paren_close) //只要不是)节点，则一直读取
			{
				AST_STRUCT arg;
				arg.type = AST_TYPE::function_args;
				while (tokens[0].type != TOKEN_TYPE::paren_close && tokens[0].type != TOKEN_TYPE::comma)
				{
					arg.tokens.push_back(tokens[0]);
					tokens.erase(tokens.begin());
				}
				call.child.push_back(arg);
				//移除已处理的逗号
				if (tokens[0].type == TOKEN_TYPE::comma)
					tokens.erase(tokens.begin());
			}
			//移除)
			if (tokens[0].Value == ")")
				tokens.erase(tokens.begin());
			else
				ErrorExit("函数定义结束部分错误", tokens[0]);
			//判断后续是否存在函数体
			if (tokens[0].Value == ";")
				tokens.erase(tokens.begin());

			ast_list.push_back(call);
			continue;
		}
		ErrorExit("未识别标识", tokens[0]);
		break;
	}
}

void token_echo(std::vector<TOKEN> tokens, std::string pre)
{
	for (TOKEN token : tokens)
	{
		printf(pre.c_str());
		printf("[r:%3d,c:%3d]%2d : %s\n", token.row_index, token.col_index, token.type, token.Value.c_str());
	}
}
void ast_echo(std::vector<AST_STRUCT>& ast_list, std::string pre)
{
	for (AST_STRUCT a : ast_list)
	{
		printf(pre.c_str());
		printf(" type: %d\n", a.type);
		if (!a.child.empty())
		{
			printf(pre.c_str());
			printf("child:\n");
			ast_echo(a.child, pre + "    ");
		}
		if (!a.tokens.empty())
		{
			token_echo(a.tokens, pre + "    ");
		}
	}
}

//	THE END