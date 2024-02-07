//
// CmLang
//

#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include "colang.h"
//#include "lexer.h"
#include "ast.h"

static std::unique_ptr<llvm::LLVMContext> TheContext;
static std::unique_ptr<llvm::Module> TheModule;
static std::unique_ptr<llvm::IRBuilder<>> Builder;
static std::map<std::string, llvm::Value*> NamedValues;



//std::vector<ExprAST> ExprList;
//std::vector<FunctionAST> FunctionList;

class ExprAST
{
public:
	virtual ~ExprAST() = default;
};

//int32
class ExprASTint32 :ExprAST
{
	int value;
	bool un;
public:
	ExprASTint32(int value,bool un=false):value(value),un(un){}
};


class ExprASTdouble :ExprAST
{
	double value;
public:
	ExprASTdouble(double value) :value(value) {}
	llvm::Value* codegen()
	{
		return llvm::ConstantFP::get(*TheContext, llvm::APFloat(value));
	}
};


//字符串
class ExprASTstring :ExprAST
{
	std::string value;
public:
	ExprASTstring(std::string value) :value(value) {}
};

//变量
class ExprASTvar :ExprAST
{
	std::string name;
public:
	ExprASTvar(std::string name) :name(name) {}
};

//class ExprASTbinary :ExprAST
//{
//	std::string op;
//	std::unique_ptr<ExprAST> lhs;
//	std::unique_ptr<ExprAST> rhs;
//public:
//	ExprASTbinary(std::string op, std::unique_ptr<ExprAST> lhs, std::unique_ptr<ExprAST> rhs) :op(op), lhs(lhs), rhs(rhs) {}
//};

class ExprASTcall :ExprAST
{
	std::string callee;
	std::vector<ExprAST> args;
public:
	ExprASTcall(std::string callee, std::vector<ExprAST> args) :callee(callee), args(args) {}
};

//非代码，直接输出
class ExprASTnoncode :ExprAST
{
	std::string value;
public:
	ExprASTnoncode(std::string value) :value(value) {}
	llvm::Value* codegen()
	{
		llvm::Function* func = TheModule->getFunction("printf");
		std::vector<llvm::Value*> args;
	}
};


//函数定义
class FunctionAST
{
	std::string ret_type;
	std::string name;
	std::vector<std::string> args_type;
	std::vector<std::string> args_name;
	std::vector<ExprAST> body;
public:
	FunctionAST(std::string ret_type, std::string Name,std::vector<std::string> args_type,
		std::vector<std::string> args_name,std::vector<ExprAST> body)
		:ret_type(ret_type), name(name), args_type(args_type),args_name(args_name),body(body) {}
	//const std::string& getName() const { return name; }
};




llvm::Type* ast_getType(std::string name)
{
	if (name == "void") return llvm::Type::getVoidTy(*TheContext);
	if (name == "int8" || name == "byte" || name=="char") return llvm::Type::getInt8Ty(*TheContext);
	if (name == "int8*" || name == "byte*" || name=="char*") return llvm::Type::getInt8PtrTy(*TheContext);
	if (name == "int16" || name == "short") return llvm::Type::getInt16Ty(*TheContext);
	if (name == "int16*" || name == "short*") return llvm::Type::getInt16PtrTy(*TheContext);
	if (name == "int32" || name == "int") return llvm::Type::getInt32Ty(*TheContext);
	if (name == "int32*" || name == "int*") return llvm::Type::getInt32PtrTy(*TheContext);
	if (name == "int64" || name == "long") return llvm::Type::getInt64Ty(*TheContext);
	if (name == "int64*" || name == "long*") return llvm::Type::getInt64PtrTy(*TheContext);
	if (name == "float") return llvm::Type::getFloatTy(*TheContext);
	if (name == "float*") return llvm::Type::getFloatPtrTy(*TheContext);
	if (name == "double") return llvm::Type::getDoubleTy(*TheContext);
	if (name == "double*") return llvm::Type::getDoublePtrTy(*TheContext);
	return NULL;
}

llvm::Type* ast_type(std::vector<TOKEN>& tokens)
{
	std::string name = tokens[0].Value;
	if (tokens[1].Value == "*")
		name += "*";

	llvm::Type* type=NULL;
	if (name == "void") type = llvm::Type::getVoidTy(*TheContext);
	if (name == "int8" || name == "byte" || name == "char") type = llvm::Type::getInt8Ty(*TheContext);
	if (name == "int8*" || name == "byte*" || name == "char*") type = llvm::Type::getInt8PtrTy(*TheContext);
	if (name == "int16" || name == "short") type = llvm::Type::getInt16Ty(*TheContext);
	if (name == "int16*" || name == "short*") type = llvm::Type::getInt16PtrTy(*TheContext);
	if (name == "int32" || name == "int") type = llvm::Type::getInt32Ty(*TheContext);
	if (name == "int32*" || name == "int*") type = llvm::Type::getInt32PtrTy(*TheContext);
	if (name == "int64" || name == "long") type = llvm::Type::getInt64Ty(*TheContext);
	if (name == "int64*" || name == "long*") type = llvm::Type::getInt64PtrTy(*TheContext);
	if (name == "float") type = llvm::Type::getFloatTy(*TheContext);
	if (name == "float*") type = llvm::Type::getFloatPtrTy(*TheContext);
	if (name == "double") type = llvm::Type::getDoubleTy(*TheContext);
	if (name == "double*") type = llvm::Type::getDoublePtrTy(*TheContext);
	
	if (type == NULL)
	{
		printf("数据类型定义错误");
		exit(4);
	}
	tokens.erase(tokens.begin());
	if(tokens[0].Value=="*")
		tokens.erase(tokens.begin());
	return type;
}

std::vector<ExprAST> parse_statement(std::vector<TOKEN>& tokens)
{
	std::vector<ExprAST> ast;

	if (tokens[0].type == TOKEN_TYPE::semicolon)
		return ast;

}

//函数定义的处理
FunctionAST parse_function(std::vector<TOKEN>& tokens)
{
	//函数返回类型
	std::string ret_type = tokens[0].Value;
	tokens.erase(tokens.begin());
	if (tokens[1].Value == "*")
	{
		ret_type += "*";
		tokens.erase(tokens.begin());
	}

	//函数名称
	std::string name = tokens[0].Value;
	tokens.erase(tokens.begin());
	
	//读取函数定义的参数
	std::vector<std::string> args_type;
	std::vector<std::string> args_name;
	while (tokens[0].type != TOKEN_TYPE::paren_close) //判断是否等于右括号，如果是则退出判断
	{
		if (tokens[0].type == TOKEN_TYPE::paren_open || tokens[0].type == TOKEN_TYPE::comma)
			tokens.erase(tokens.begin());	//跳过左括号或逗号
		else
			ErrorExit("函数定义中参数部分错误", tokens[0]);

		std::string arg_type;
		std::string arg_name;
		// int* a
		if (tokens[0].type == TOKEN_TYPE::block && tokens[1].Value == "*" && tokens[2].type == TOKEN_TYPE::block)
		{
			arg_type = tokens[0].Value + "*";
			arg_name = tokens[2].Value;
			tokens.erase(tokens.begin());
			tokens.erase(tokens.begin());
			tokens.erase(tokens.begin());
		}
		// int a
		else if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type == TOKEN_TYPE::block && tokens[2].type != TOKEN_TYPE::block)
		{
			arg_type = tokens[0].Value;
			arg_name = tokens[1].Value;
			tokens.erase(tokens.begin());
			tokens.erase(tokens.begin());
		}
		// a 未设置类型，则系统默认为object类型
		else if (tokens[0].type == TOKEN_TYPE::block && tokens[1].type != TOKEN_TYPE::block)
		{
			arg_type = "object";
			arg_name = tokens[1].Value;
			tokens.erase(tokens.begin());
		}
		else
			ErrorExit("函数定义参数识别错误", tokens[0]);

		args_type.push_back(arg_type);
		args_name.push_back(arg_name);
	}

	//读取函数体
	std::vector<ExprAST> body;
	//	判断下一个标识是否为{，如果是{则认为是函数体数据
	if (tokens[0].type == TOKEN_TYPE::bracket_open)
	{
		body = parse_statement(tokens);
	}
	
	FunctionAST func(ret_type, name, args_type, args_name,body);
	return func;
}

void ast_proc(std::vector<TOKEN>& tokens, int token_index)
{
	switch (tokens[token_index].type)
	{
	case TOKEN_TYPE::noncode:
		break;
	case TOKEN_TYPE::block:
		//判断是否为函数定义
		//判断依据：下一个token是一个字符，再下一个是(
		if (tokens[token_index + 1].type == TOKEN_TYPE::block && tokens[token_index + 2].type == TOKEN_TYPE::paren_open)
		{
			parse_function(tokens);
		}
		break;
	}
}

//语法分析
//void ast(std::vector<TOKEN>& tokens)
//{
//	TheContext = std::make_unique<llvm::LLVMContext>();
//	TheModule = std::make_unique<llvm::Module>("", *TheContext);
//	Builder = std::make_unique<llvm::IRBuilder<>>(*TheContext);
//
//	ast_proc(tokens, 0);
//
//	return;
//}


struct IR_INFO
{
	llvm::LLVMContext context;
	llvm::Module* module;
	std::unique_ptr<llvm::IRBuilder<>> builder;
};

llvm::Type* ir_type(std::vector<TOKEN>& tokens,IR_INFO& ir_info)
{
	std::string name = tokens[0].Value;
	if (tokens.size()>1 && tokens[1].Value == "*")
		name += "*";

	llvm::Type* type = NULL;
	if (name == "void") type = llvm::Type::getVoidTy(ir_info.context);
	if (name == "int8" || name == "byte" || name == "char") type = llvm::Type::getInt8Ty(ir_info.context);
	if (name == "int8*" || name == "byte*" || name == "char*") type = llvm::Type::getInt8PtrTy(ir_info.context);
	if (name == "int16" || name == "short") type = llvm::Type::getInt16Ty(ir_info.context);
	if (name == "int16*" || name == "short*") type = llvm::Type::getInt16PtrTy(ir_info.context);
	if (name == "int32" || name == "int") type = llvm::Type::getInt32Ty(ir_info.context);
	if (name == "int32*" || name == "int*") type = llvm::Type::getInt32PtrTy(ir_info.context);
	if (name == "int64" || name == "long") type = llvm::Type::getInt64Ty(ir_info.context);
	if (name == "int64*" || name == "long*") type = llvm::Type::getInt64PtrTy(ir_info.context);
	if (name == "float") type = llvm::Type::getFloatTy(ir_info.context);
	if (name == "float*") type = llvm::Type::getFloatPtrTy(ir_info.context);
	if (name == "double") type = llvm::Type::getDoubleTy(ir_info.context);
	if (name == "double*") type = llvm::Type::getDoublePtrTy(ir_info.context);

	if (type == NULL)
		ErrorExit("数据类型定义错误",tokens[0]);
	return type;
}


void ir(std::vector<AST_STRUCT> ast_list,IR_INFO& ir_info)
{
	llvm::FunctionType* mainType = llvm::FunctionType::get(ir_info.builder->getInt32Ty(), false);
	llvm::Function* main = llvm::Function::Create(mainType, llvm::GlobalValue::ExternalLinkage, "main", ir_info.module);
	llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(ir_info.context, "entry_main", main);
	ir_info.builder->SetInsertPoint(entryBlock);

	for (AST_STRUCT a : ast_list)
	{
		if (a.type == AST_TYPE::echo)
		{
			llvm::Function* function = ir_info.module->getFunction("printf");
			if (function)
			{
				//std::vector<llvm::Value*> args;
				//llvm::Value* v = ir_info.builder->CreateGlobalStringPtr(a.tokens[0].Value);
				//args.push_back(v);

				llvm::Value* args = ir_info.builder->CreateGlobalStringPtr(a.tokens[0].Value);
				ir_info.builder->CreateCall(function, args);
			}
			else
				ErrorExit("未找到函数定义：printf", a.tokens[0]);
			continue;
		}
		if (a.type == AST_TYPE::function)
		{
			//args
			llvm::Type* frtype;
			std::string fname;
			std::vector<llvm::Type*> fatype;
			std::vector<std::string> faname;
			bool isVarArg=false;
			for (AST_STRUCT aa : a.child)
				if (aa.type == AST_TYPE::function_ret)
					frtype = ir_type(aa.tokens, ir_info);
				else if (aa.type == AST_TYPE::function_name)
					fname = aa.tokens[0].Value;
				else if (aa.type == AST_TYPE::function_args)
					if (aa.tokens[0].Value == "...")
						isVarArg = true;
					else
					{
						fatype.push_back(ir_type(aa.tokens, ir_info));
						faname.push_back(aa.tokens[aa.tokens.size() - 1].Value);
					}
			llvm::FunctionType* functionType = llvm::FunctionType::get(frtype, fatype, isVarArg);
			llvm::Function* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,fname,ir_info.module);
			//arg name
			unsigned i = 0;
			for (auto& a : function->args())
			{
				a.setName(faname[i]);
			}
			continue;
		}
		continue;

	}
}

//解析一个指定的cm文件到bc格式
int cm2bc(const char* cmfilename)
{
	SRCINFO srcinfo;

	//读取源文件内容
	FILE* fp = fopen(cmfilename, "rb");
	if (fp == NULL)
	{
		printf("ERROR: input file open fail.");
		return 2;
	}
	fseek(fp, 0, SEEK_END);
	srcinfo.size = ftell(fp);
	srcinfo.src = (char*)malloc(srcinfo.size + 1);
	if (srcinfo.src == NULL)
	{
		printf("ERROR: out of memory");
		return 3;
	}
	fseek(fp, 0, SEEK_SET);
	fread(srcinfo.src, 1, srcinfo.size, fp);
	srcinfo.src[srcinfo.size] = 0;
	//srcinfo.src_current = srcinfo.src;
	fclose(fp);

	//词法分析
	std::vector<TOKEN> tokens;
	lexer(tokens, srcinfo);
	printf("---------- Lexer ----------\n");
	for (TOKEN token : tokens)
	{
		printf("[r:%3d,c:%3d]%2d : %s\n",token.row_index,token.col_index, token.type, token.Value.c_str());
	}

	//语法分析
	std::vector<AST_STRUCT> ast_list;
	ast(tokens,ast_list);
	printf("\n---------- AST ----------\n");
	ast_echo(ast_list, "");

	//IR
	IR_INFO ir_info;
	//ir_info.context = llvm::LLVMContext();
	ir_info.module =new llvm::Module(cmfilename, ir_info.context);
	ir_info.builder = std::make_unique<llvm::IRBuilder<>>((ir_info.context));
	ir(ast_list, ir_info);
	printf("\n---------- IR ----------\n");
	ir_info.module->print(llvm::outs(), nullptr);


	std::string col = std::string(cmfilename) + "l";
	std::error_code ec;
	llvm::raw_fd_ostream fout(col,ec);
	ir_info.module->print(fout, nullptr);
	fout.close();

	//std::string cob = std::string(cmfilename) + "b";
	


}

int main(int argc, char** argv)
{
	//参数处理
	if (argc == 1)
	{
		printf("ERROR: no input file\n");
		return 1;
	}

	cm2bc(argv[1]);
	//const char* filename = "test/test1.cm";
	//cm2bc(filename);

	//Builder = new llvm::IRBuilder(TheContext);
	//TheModule = new llvm::Module("", TheContext);

	return 0;
}

//	THE END