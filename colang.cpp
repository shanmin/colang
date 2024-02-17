//
// CmLang
//

#include <cstdio>
#include <vector>
#include "lexer.h"
#include "ast.h"
#include "ir.h"

//解析一个指定的co文件到bc格式
void co2bc(const char* filename)
{
	SRCINFO srcinfo;
	//读取源文件内容
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL)
	{
		printf("ERROR: input file open fail.");
		return;
	}
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	srcinfo.filename = filename;
	srcinfo.src = (char*)malloc(size + 1);
	if (srcinfo.src == NULL)
	{
		printf("ERROR: out of memory");
		return;
	}
	fseek(fp, 0, SEEK_SET);
	fread(srcinfo.src, 1, size, fp);
	srcinfo.src[size] = 0;
	fclose(fp);

	//词法分析
	std::vector<TOKEN> tokens;
	lexer(tokens, srcinfo);
	printf("---------- Lexer ----------\n");
    token_echo(tokens);

	//语法分析
	printf("\n---------- AST ----------\n");
	std::vector<AST> ast_list = ast(tokens);
	ast_echo(ast_list, "");


	//IR
	printf("\n---------- IR ----------\n");
	ir(ast_list,filename);
	////IR_INFO ir_info;
	//////ir_info.context = llvm::LLVMContext();
	////ir_info.module =new llvm::Module(cmfilename, ir_info.context);
	////ir_info.builder = std::make_unique<llvm::IRBuilder<>>((ir_info.context));
	//////ir(ast_list, ir_info);
	//////验证模块是否存在问题
	////std::string mstr;
	////llvm::raw_string_ostream merr(mstr);
	////bool result=llvm::verifyModule(*ir_info.module,&merr);
	////if (result)
	////{
	////	printf("模块存在错误！%s",mstr.c_str());
	////	exit(2);
	////}
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main(int argc, char** argv)
{
    //参数处理
	if (argc == 1)
	{
		printf("ERROR: no input file\n");
		return 1;
	}
 
    co2bc(argv[1]);

    //// Prime the first token.
    //fprintf(stderr, "ready> ");
    //getNextToken();

    //// Make the module, which holds all the code.
    //InitializeModule();

    //// Run the main "interpreter loop" now.
    //MainLoop();

    //// Print out all of the generated code.
    //TheModule->print(errs(), nullptr);

    return 0;
}



//#include "colang.h"
////#include "lexer.h"
//#include "ast.h"
//#include "ir.h"
//
////static std::map<std::string, llvm::Value*> NamedValues;
//
////std::vector<ExprAST> ExprList;
////std::vector<FunctionAST> FunctionList;
//
////class ExprASTbinary :ExprAST
////{
////	std::string op;
////	std::unique_ptr<ExprAST> lhs;
////	std::unique_ptr<ExprAST> rhs;
////public:
////	ExprASTbinary(std::string op, std::unique_ptr<ExprAST> lhs, std::unique_ptr<ExprAST> rhs) :op(op), lhs(lhs), rhs(rhs) {}
////};
//
////class ExprASTcall :ExprAST
////{
////	std::string callee;
////	std::vector<ExprAST> args;
////public:
////	ExprASTcall(std::string callee, std::vector<ExprAST> args) :callee(callee), args(args) {}
////};
////
//////非代码，直接输出
////class ExprASTnoncode :ExprAST
////{
////	std::string value;
////public:
////	ExprASTnoncode(std::string value) :value(value) {}
////	llvm::Value* codegen()
////	{
////		llvm::Function* func = TheModule->getFunction("printf");
////		std::vector<llvm::Value*> args;
////	}
////};
//
////
////llvm::Type* ast_getType(std::string name)
////{
////	if (name == "void") return llvm::Type::getVoidTy(*TheContext);
////	if (name == "int8" || name == "byte" || name=="char") return llvm::Type::getInt8Ty(*TheContext);
////	if (name == "int8*" || name == "byte*" || name=="char*") return llvm::Type::getInt8PtrTy(*TheContext);
////	if (name == "int16" || name == "short") return llvm::Type::getInt16Ty(*TheContext);
////	if (name == "int16*" || name == "short*") return llvm::Type::getInt16PtrTy(*TheContext);
////	if (name == "int32" || name == "int") return llvm::Type::getInt32Ty(*TheContext);
////	if (name == "int32*" || name == "int*") return llvm::Type::getInt32PtrTy(*TheContext);
////	if (name == "int64" || name == "long") return llvm::Type::getInt64Ty(*TheContext);
////	if (name == "int64*" || name == "long*") return llvm::Type::getInt64PtrTy(*TheContext);
////	if (name == "float") return llvm::Type::getFloatTy(*TheContext);
////	if (name == "float*") return llvm::Type::getFloatPtrTy(*TheContext);
////	if (name == "double") return llvm::Type::getDoubleTy(*TheContext);
////	if (name == "double*") return llvm::Type::getDoublePtrTy(*TheContext);
////	return NULL;
////}
////
////llvm::Type* ast_type(std::vector<TOKEN>& tokens)
////{
////	std::string name = tokens[0].Value;
////	if (tokens[1].Value == "*")
////		name += "*";
////
////	llvm::Type* type=NULL;
////	if (name == "void") type = llvm::Type::getVoidTy(*TheContext);
////	if (name == "int8" || name == "byte" || name == "char") type = llvm::Type::getInt8Ty(*TheContext);
////	if (name == "int8*" || name == "byte*" || name == "char*") type = llvm::Type::getInt8PtrTy(*TheContext);
////	if (name == "int16" || name == "short") type = llvm::Type::getInt16Ty(*TheContext);
////	if (name == "int16*" || name == "short*") type = llvm::Type::getInt16PtrTy(*TheContext);
////	if (name == "int32" || name == "int") type = llvm::Type::getInt32Ty(*TheContext);
////	if (name == "int32*" || name == "int*") type = llvm::Type::getInt32PtrTy(*TheContext);
////	if (name == "int64" || name == "long") type = llvm::Type::getInt64Ty(*TheContext);
////	if (name == "int64*" || name == "long*") type = llvm::Type::getInt64PtrTy(*TheContext);
////	if (name == "float") type = llvm::Type::getFloatTy(*TheContext);
////	if (name == "float*") type = llvm::Type::getFloatPtrTy(*TheContext);
////	if (name == "double") type = llvm::Type::getDoubleTy(*TheContext);
////	if (name == "double*") type = llvm::Type::getDoublePtrTy(*TheContext);
////	
////	if (type == NULL)
////	{
////		printf("数据类型定义错误");
////		exit(4);
////	}
////	tokens.erase(tokens.begin());
////	if(tokens[0].Value=="*")
////		tokens.erase(tokens.begin());
////	return type;
////}
////
//////std::vector<ExprAST> parse_statement(std::vector<TOKEN>& tokens)
//////{
//////	std::vector<ExprAST> ast;
//////
//////	if (tokens[0].type == TOKEN_TYPE::semicolon)
//////		return ast;
//////
//////}
////
//////函数定义的处理
////FunctionAST parse_function(std::vector<TOKEN>& tokens)
////{
////	//函数返回类型
////	std::string ret_type = tokens[0].Value;
////	tokens.erase(tokens.begin());
////	if (tokens[1].Value == "*")
////	{
////		ret_type += "*";
////		tokens.erase(tokens.begin());
////	}
////
////	//函数名称
////	std::string name = tokens[0].Value;
////	tokens.erase(tokens.begin());
////	
////	//读取函数定义的参数
////	std::vector<std::string> args_type;
////	std::vector<std::string> args_name;
////	while (tokens[0].type != TOKEN_TYPE::paren_close) //判断是否等于右括号，如果是则退出判断
////	{
////		if (tokens[0].type == TOKEN_TYPE::paren_open || tokens[0].type == TOKEN_TYPE::comma)
////			tokens.erase(tokens.begin());	//跳过左括号或逗号
////		else
////			ErrorExit("函数定义中参数部分错误", tokens);
////
////		std::string arg_type;
////		std::string arg_name;
////		// int* a
////		if (tokens[0].type == TOKEN_TYPE::block && tokens[1].Value == "*" && tokens[2].type == TOKEN_TYPE::block)
////		{
////			arg_type = tokens[0].Value + "*";
////			arg_name = tokens[2].Value;
////			tokens.erase(tokens.begin());
////			tokens.erase(tokens.begin());
////			tokens.erase(tokens.begin());
////		}
////		// int a
////		else if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type == TOKEN_TYPE::block && tokens[2].type != TOKEN_TYPE::block)
////		{
////			arg_type = tokens[0].Value;
////			arg_name = tokens[1].Value;
////			tokens.erase(tokens.begin());
////			tokens.erase(tokens.begin());
////		}
////		// a 未设置类型，则系统默认为object类型
////		else if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type != TOKEN_TYPE::block)
////		{
////			arg_type = "object";
////			arg_name = tokens[1].Value;
////			tokens.erase(tokens.begin());
////		}
////		else
////			ErrorExit("函数定义参数识别错误", tokens);
////
////		args_type.push_back(arg_type);
////		args_name.push_back(arg_name);
////	}
////
////	//读取函数体
////	std::vector<ExprAST> body;
////	//	判断下一个标识是否为{，如果是{则认为是函数体数据
////	if (tokens[0].type == TOKEN_TYPE::bracket_open)
////	{
////		body = parse_statement(tokens);
////	}
////	
////	FunctionAST func(ret_type, name, args_type, args_name,body);
////	return func;
////}
////
////void ast_proc(std::vector<TOKEN>& tokens, int token_index)
////{
////	switch (tokens[token_index].type)
////	{
////	case TOKEN_TYPE::noncode:
////		break;
////	case TOKEN_TYPE::block:
////		//判断是否为函数定义
////		//判断依据：下一个token是一个字符，再下一个是(
////		if (tokens[token_index + 1].type == TOKEN_TYPE::block && tokens[token_index + 2].type == TOKEN_TYPE::paren_open)
////		{
////			parse_function(tokens);
////		}
////		break;
////	}
////}
//
//
//struct IR_INFO
//{
//	llvm::LLVMContext context;
//	llvm::Module* module;
//	std::unique_ptr<llvm::IRBuilder<>> builder;
//};
//
//llvm::Type* ir_type(std::vector<TOKEN>& tokens,IR_INFO& ir_info)
//{
//	std::string name = tokens[0].Value;
//	if (tokens.size()>1 && tokens[1].Value == "*")
//		name += "*";
//
//	llvm::Type* type = NULL;
//	if (name == "void") type = llvm::Type::getVoidTy(ir_info.context);
//	if (name == "int8" || name == "byte" || name == "char") type = llvm::Type::getInt8Ty(ir_info.context);
//	if (name == "int8*" || name == "byte*" || name == "char*") type = llvm::Type::getInt8PtrTy(ir_info.context);
//	if (name == "int16" || name == "short") type = llvm::Type::getInt16Ty(ir_info.context);
//	if (name == "int16*" || name == "short*") type = llvm::Type::getInt16PtrTy(ir_info.context);
//	if (name == "int32" || name == "int") type = llvm::Type::getInt32Ty(ir_info.context);
//	if (name == "int32*" || name == "int*") type = llvm::Type::getInt32PtrTy(ir_info.context);
//	if (name == "int64" || name == "long") type = llvm::Type::getInt64Ty(ir_info.context);
//	if (name == "int64*" || name == "long*") type = llvm::Type::getInt64PtrTy(ir_info.context);
//	if (name == "float") type = llvm::Type::getFloatTy(ir_info.context);
//	if (name == "float*") type = llvm::Type::getFloatPtrTy(ir_info.context);
//	if (name == "double") type = llvm::Type::getDoubleTy(ir_info.context);
//	if (name == "double*") type = llvm::Type::getDoublePtrTy(ir_info.context);
//
//	if (type == NULL)
//		ErrorExit("数据类型定义错误",tokens);
//	return type;
//}
//
//llvm::Value* ir_value(std::vector<TOKEN>& tokens, IR_INFO& ir_info)
//{
//	llvm::Value* ret;
//	for (TOKEN t : tokens)
//		if (t.type == TOKEN_TYPE::string)
//			ret = ir_info.builder->CreateGlobalStringPtr(t.Value);
//		else
//			ErrorExit("未识别的值", tokens);
//	return ret;
//}
//
////void ir(std::vector<AST> ast_list,IR_INFO& ir_info)
////{
////	llvm::FunctionType* mainType = llvm::FunctionType::get(ir_info.builder->getInt32Ty(), false);
////	llvm::Function* main = llvm::Function::Create(mainType, llvm::GlobalValue::ExternalLinkage, "main", ir_info.module);
////	llvm::BasicBlock* entryMain = llvm::BasicBlock::Create(ir_info.context, "entry_main", main);
////	ir_info.builder->SetInsertPoint(entryMain);
////
////	for (AST a : ast_list)
////	{
////		if (a.type == AST_TYPE::echo)
////		{
////		}
////		if (a.type == AST_TYPE::function)
////		{
//
////		}
////		// Function Call
////		if (a.type == AST_TYPE::call)
////		{
//
////		}
////		//ErrorExit("未处理的代码", a.tokens[0]);
////		printf("\n---------- ERROR ----------\n");
////		printf("未处理的代码\n");
////		std::vector<AST_STRUCT> tmp;
////		tmp.push_back(a);
////		ast_echo(tmp, "");
////		exit(3);
////	}
////
////	llvm::ConstantInt* ret =ir_info.builder->getInt32(0);
////	ir_info.builder->CreateRet(ret);
////	llvm::verifyFunction(*main);
////}
//

//

//	THE END