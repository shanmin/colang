//
//	IR
//
#pragma once

#include <vector>
#include <map>
#include <memory>
#include <llvm/Bitcode/BitcodeWriter.h>
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include "ast.h"
#include "lexer.h"



struct VAR_INFO
{
	llvm::Type* type;
	llvm::AllocaInst* value;
};
struct VAR_LIST
{
	std::string zone; //global,codeblock,function
	std::map<std::string, VAR_INFO> info; //name,type
};




//void ErrorExit(const char* str, std::vector<AST>& ast_list)
//{
//	printf("\n---------- Error ----------\n%s\n", str);
//	ast_echo(ast_list,"",true);
//	exit(1);
//}


//void ir_proc(std::vector<AST>& ast_list, bool ismain);


void ir(std::vector<AST*>& ast_list, const char* filename);

llvm::Type* ir_type(std::vector<TOKEN>& tokens);


//
//llvm::Value* ir_expr(std::vector<AST>& ast_list, IRINFO& irinfo);
//
//struct IR_EXPR
//{
//	AST op;		//操作符节点
//	int op_pri; //操作符优先级
//	AST right;
//	llvm::Value* right_value=NULL;
//};


//
////语法分析，并生成IR代码
//void ir_proc(std::vector<AST>& ast_list,IRINFO& irinfo,bool ismain)
//{
//
//	while (!ast_list.empty())
//	{
//		//非代码，直接输出
//		if (ast_list[0].type==AST_TYPE::ast_noncode)
//		{

//			continue;
//		}
//
//		//Function
//		//	Ex: int function_name(int arg1)
//		//		 |		|              + args
//		//       |      + 函数名称
//		//       +  返回值类型
//		//判断依据：当前为字符，下一个token是一个字符，再下一个是(
//		if (ast_list[0].type==AST_TYPE::ast_function)
//		{

//		}
//
//		if (ast_list[0].type ==AST_TYPE::ast_codeblock)
//		{

//		}
//
//		//变量定义
//		//Ex:	int a;
//		//		int b=2;
//		if (ast_list[0].type==AST_TYPE::ast_var)
//		{
//			//VAR_LIST var_list = irinfo.varlist.rbegin();

//		}
//
//		//变量赋值
//		if (ast_list[0].type ==AST_TYPE::ast_expr)
//		{
//			ir_expr(ast_list[0].body["body"], irinfo);
//			////VAR_LIST var_list = irinfo.varlist.back();
//			//VAR_INFO var_info = irinfo.varlist[irinfo.varlist.size() - 1].info[ast_list[0].value["name"][0].Value];
//			//irinfo.builder->CreateStore(ir_expr(ast_list[0].value["value"],irinfo), var_info.value);
//			ast_list.erase(ast_list.begin());
//			continue;
//		}
//
//		ErrorExit("未识别标识", ast_list);
//		break;
//	}
//	//return ast_list;
//}
//

//	THE END