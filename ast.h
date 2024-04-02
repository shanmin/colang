//
//	AST语法分析
//
#pragma once

#include <iostream>
#include <llvm/IR/Value.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
//#include "colang.h"


class AST
{
public:
	virtual llvm::Value* codegen() = 0;
	virtual void show(std::string pre)=0;
};


class AST_expr :public AST
{
	TOKEN op;
	AST* left;
	AST* right;
public:
	//int op_pri;
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	//AST_expr();
	//AST_expr(std::vector<TOKEN>& tokens);
	AST_expr(AST* left, TOKEN op, AST* right) :left(left), op(op), right(right) {};
};


std::vector<AST*> ast(std::vector<TOKEN>& tokens);
AST* ast1(std::vector<TOKEN>& tokens);


class AST_noncode :public AST
{
	TOKEN value;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_noncode(std::vector<TOKEN>& tokens);
};



class AST_codeblock :public AST
{
	std::vector<AST*> body;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_codeblock(std::vector<TOKEN>& tokens);
};


class AST_call :public AST
{
	TOKEN name;
	std::vector<AST*> args;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_call(std::vector<TOKEN>& tokens);
};


class AST_for :public AST
{
	TOKEN name;
	AST* expr1 = NULL;
	AST* expr2 = NULL;
	AST* expr3 = NULL;
	AST* body = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_for(std::vector<TOKEN>& tokens);
};


class AST_function :public AST
{
	std::vector<TOKEN> rett;
	TOKEN name;
	std::vector<TOKEN> args;
	std::vector<AST*> body;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_function(std::vector<TOKEN>& tokens);
};


class AST_if :public AST
{
	TOKEN name;
	AST* expr1=NULL;
	AST* thenbody=NULL;
	AST* elsebody=NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_if(std::vector<TOKEN>& tokens);
};


class AST_label :public AST
{
	TOKEN name;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_label(std::vector<TOKEN>& tokens);
};


class AST_value :public AST
{
public:
	TOKEN value;
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_value(TOKEN token) :value(token) {}
	AST_value(std::vector<TOKEN>& tokens);
};


class AST_var :public AST
{
	std::vector<TOKEN> type;
	TOKEN name;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_var(std::vector<TOKEN>& tokens);
};


void ast_echo(std::vector<AST*> ast_list, std::string pre);



//enum AST_TYPE
//{
//	ast_noncode,
//	ast_codeblock,
//	ast_function,
//	ast_call,
//	ast_var,
//	ast_expr,
//};
//std::vector<std::string> AST_TYPE_STRING = {
//	"noncode",
//	"codeblock",
//	"function",
//	"call",
//	"var",
//	"expr",
//};
//
//struct AST
//{
//	AST_TYPE type;
//	std::map<std::string, std::vector<TOKEN>> value;
//	//std::vector<AST> body;
//	std::map<std::string, std::vector<AST>> body;
//};
//
//
//std::vector<AST> ast_parse_expr(std::vector<TOKEN>& tokens)
//{
//	std::vector<AST> ast_list;
//	while (!tokens.empty())
//	{
//
//		//代码区结束,直接返回
//		if (tokens[0].Value == "}")
//			break;
//		else if (tokens[0].Value == ")" || tokens[0].Value==";") // 表达式结束
//		{
//			tokens.erase(tokens.begin());
//			break;
//		}
//		if(tokens[0].Value==",")
//		{
//			tokens.erase(tokens.begin());
//			continue;
//		}
//
//		//表达式
//		AST a;
//		a.type =AST_TYPE::ast_expr;
//		a.value["value"].push_back(tokens[0]);
//		tokens.erase(tokens.begin());
//		ast_list.push_back(a);
//	}
//	return ast_list;
//}
//
////语法分析，主要区分代码层次、语句用途
//std::vector<AST> ast(std::vector<TOKEN>& tokens)
//{
//	std::vector<AST> ast_list;
//	while (!tokens.empty())
//	{
//		//代码区结束,直接返回
//		if (tokens[0].type != TOKEN_TYPE::string)
//		{
//			if (tokens[0].Value == "{") //新作用域
//			{
//				tokens.erase(tokens.begin());
//				AST a;
//				a.type = AST_TYPE::ast_codeblock;
//				a.body["body"] = ast(tokens);
//				ast_list.push_back(a);
//				continue;
//			}
//			if (tokens[0].Value == ")") // 表达式结束
//			{
//				tokens.erase(tokens.begin());
//				break;
//			}
//		}
//
//		//表达式处理
//		AST a;
//		a.type = AST_TYPE::ast_expr;
//		a.body["body"] = ast_parse_expr(tokens);
//		ast_list.push_back(a);
//
//
//		////变量赋值
//		//if (tokens[1].type != TOKEN_TYPE::string && tokens[1].Value == "=")
//		//{
//		//	AST a;
//		//	a.type =AST_TYPE::ast_expr;
//		//	a.value["name"].push_back(tokens[0]);
//		//	tokens.erase(tokens.begin());
//		//	tokens.erase(tokens.begin());
//		//	while (!tokens.empty() && tokens[0].type != TOKEN_TYPE::string && tokens[0].Value != ";")
//		//	{
//		//		a.value["value"].push_back(tokens[0]);
//		//		tokens.erase(tokens.begin());
//		//	}
//		//	if(!tokens.empty())
//		//		tokens.erase(tokens.begin());
//		//	ast_list.push_back(a);
//		//	continue;
//		//}
//
//		////表达式
//		//{
//		//	AST a;
//		//	a.type = ast_expr;
//		//	a.value["value"].push_back(tokens[0]);
//		//	tokens.erase(tokens.begin());
//		//	ast_list.push_back(a);
//		//	continue;
//		//}
//
//		//ErrorExit("未识别标识", tokens);
//		//break;
//	}
//	return ast_list;
//}
//


//	THE END