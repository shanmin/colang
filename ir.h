//
//	IR
//

#include <vector>
#include <llvm/Bitcode/BitcodeWriter.h>
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "lexer.h"
#include "ast.h"

void ErrorExit(const char* str, std::vector<AST>& ast_list)
{
	printf("\n---------- Error ----------\n%s\n", str);
	ast_echo(ast_list,"",true);
	exit(1);
}

struct VAR_INFO
{
	llvm::Type* type;
	llvm::Value* value;
};
struct VAR_LIST
{
	std::string zone; //global,codeblock,function
	std::map<std::string, VAR_INFO> info; //name,type
};

struct IRINFO
{
	llvm::LLVMContext context;
	llvm::Module* module;
	std::unique_ptr<llvm::IRBuilder<>> builder;

	std::vector<VAR_LIST> varlist; //局部变量范围
};


void ir_proc(std::vector<AST>& ast_list, IRINFO& irinfo,bool ismain);


void ir(std::vector<AST>& tokens,const char* filename)
{
	IRINFO irinfo;
	irinfo.module =new llvm::Module(filename, irinfo.context);
	irinfo.builder = std::make_unique<llvm::IRBuilder<>>((irinfo.context));

	//设置当前变量作用域
	VAR_LIST varlist;
	varlist.zone = "global";
	irinfo.varlist.push_back(varlist);

	llvm::FunctionType* mainType = llvm::FunctionType::get(irinfo.builder->getInt32Ty(), false);
	llvm::Function* main = llvm::Function::Create(mainType, llvm::GlobalValue::ExternalLinkage, "main", irinfo.module);
	llvm::BasicBlock* entryMain = llvm::BasicBlock::Create(irinfo.context, "entry_main", main);
	irinfo.builder->SetInsertPoint(entryMain);

	ir_proc(tokens, irinfo,true);

	llvm::ConstantInt* ret =irinfo.builder->getInt32(0);
	irinfo.builder->CreateRet(ret);
	llvm::verifyFunction(*main);

	//验证模块是否存在问题
	std::string mstr;
	llvm::raw_string_ostream merr(mstr);
	bool result=llvm::verifyModule(*irinfo.module,&merr);
	if (result)
	{
		printf("模块存在错误！%s",mstr.c_str());
		exit(2);
	}

	irinfo.module->print(llvm::outs(), nullptr);

	//输出ll格式文件
	std::string col = std::string(filename) + "l";
	std::error_code ec;
	llvm::raw_fd_ostream fout(col, ec);
	irinfo.module->print(fout, nullptr);
	fout.close();
	printf("\n---------- write ll ----------\n");
	printf("save=%x\n", ec.value());

	std::string cob = std::string(filename) + "b";
	llvm::raw_fd_ostream fout_cob(cob, ec);
	llvm::WriteBitcodeToFile(*irinfo.module, fout_cob);
	fout_cob.close();
	printf("\n---------- write bc ----------\n");
	printf("save=%x\n", ec.value());
}


llvm::Type* irtype(std::vector<TOKEN>& tokens, IRINFO& irinfo)
{
	std::string name = tokens[0].Value;
	if (tokens.size() > 1 && tokens[1].Value == "*")
	{
		name += "*";
	}

	llvm::Type* type = NULL;
	if (name == "void") type = llvm::Type::getVoidTy(irinfo.context);
	if (name == "int8" || name == "byte" || name == "char") type = llvm::Type::getInt8Ty(irinfo.context);
	if (name == "int8*" || name == "byte*" || name == "char*") type = llvm::Type::getInt8PtrTy(irinfo.context);
	if (name == "int16" || name == "short") type = llvm::Type::getInt16Ty(irinfo.context);
	if (name == "int16*" || name == "short*") type = llvm::Type::getInt16PtrTy(irinfo.context);
	if (name == "int32" || name == "int") type = llvm::Type::getInt32Ty(irinfo.context);
	if (name == "int32*" || name == "int*") type = llvm::Type::getInt32PtrTy(irinfo.context);
	if (name == "int64" || name == "long") type = llvm::Type::getInt64Ty(irinfo.context);
	if (name == "int64*" || name == "long*") type = llvm::Type::getInt64PtrTy(irinfo.context);
	if (name == "float") type = llvm::Type::getFloatTy(irinfo.context);
	if (name == "float*") type = llvm::Type::getFloatPtrTy(irinfo.context);
	if (name == "double") type = llvm::Type::getDoubleTy(irinfo.context);
	if (name == "double*") type = llvm::Type::getDoublePtrTy(irinfo.context);
	
	if (type == NULL)
		ErrorExit("数据类型定义错误", tokens);

	tokens.erase(tokens.begin());
	if (tokens.size() > 0 && tokens[0].Value == "*")
	{
		tokens.erase(tokens.begin());
	}
	return type;
}
llvm::Value* irvalue(std::vector<TOKEN>& tokens, IRINFO& irinfo)
{
	llvm::Value* ret;
	for (TOKEN t : tokens)
		if (t.type == TOKEN_TYPE::string)
		{
			ret = irinfo.builder->CreateGlobalStringPtr(t.Value);
			tokens.erase(tokens.begin());
			break;
		}
		else
			ErrorExit("未识别的值", tokens);
	return ret;
}

struct IR_EXPR
{
	std::string op;
	llvm::Value* value;
};
//表达式处理
llvm::Value* ir_expr(std::vector<TOKEN>& tokens, IRINFO& irinfo)
{
	llvm::Value* value;
	for (TOKEN t : tokens)
		if (t.type == TOKEN_TYPE::string)
		{
			value = irinfo.builder->CreateGlobalStringPtr(t.Value);
			tokens.erase(tokens.begin());
		}
		else if (t.type == TOKEN_TYPE::number)
		{
			int v = atoi(t.Value.c_str());
			value = irinfo.builder->getInt32(v);
		}
		else
			ErrorExit("未识别的值", tokens);
	return value;
}

//语法分析，并生成IR代码
void ir_proc(std::vector<AST>& ast_list,IRINFO& irinfo,bool ismain)
{

	while (!ast_list.empty())
	{
		//非代码，直接输出
		if (ast_list[0].type==AST_TYPE::ast_noncode)
		{
			llvm::Function* function = irinfo.module->getFunction("printf");
			if (function)
			{
				llvm::Value* args = irinfo.builder->CreateGlobalStringPtr(ast_list[0].value["value"][0].Value);
				irinfo.builder->CreateCall(function, args);
				ast_list.erase(ast_list.begin());
			}
			else
				ErrorExit("未找到函数定义：printf", ast_list[0].value["value"]);
			continue;
		}

		//Function
		//	Ex: int function_name(int arg1)
		//		 |		|              + args
		//       |      + 函数名称
		//       +  返回值类型
		//判断依据：当前为字符，下一个token是一个字符，再下一个是(
		if (ast_list[0].type==AST_TYPE::ast_function)
		{
			//args
			llvm::Type* frtype = irtype(ast_list[0].value["ret"], irinfo);
			std::string fname=ast_list[0].value["name"][0].Value;
			std::vector<llvm::Type*> fatype;
			std::vector<std::string> faname;
			bool isVarArg=false;
			std::vector<TOKEN> args = ast_list[0].value["args"];
			while(!args.empty())
				if (args[0].Value == "...")
				{
					isVarArg = true;
					break;
				}
				else if (args[0].Value == ",")
				{
					args.erase(args.begin());
				}
				else
				{
					fatype.push_back(irtype(args, irinfo));
					if (!args.empty())
					{
						faname.push_back(args[0].Value);
						args.erase(args.begin());
					}
					else
						faname.push_back("");
				}
			llvm::FunctionType* functionType = llvm::FunctionType::get(frtype, fatype, isVarArg);
			llvm::Function* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,fname,irinfo.module);
			//arg name
			unsigned i = 0;
			for (auto& a : function->args())
			{
				if(faname[i]!="")
					a.setName(faname[i]);
			}
			if (!ast_list[0].body.empty())
			{
				//设置当前变量作用域
				VAR_LIST varlist;
				varlist.zone = "function";
				irinfo.varlist.push_back(varlist);

				ir_proc(ast_list[0].body, irinfo, false);

				irinfo.varlist.pop_back();
			}
			ast_list.erase(ast_list.begin());
			continue;
		}

		//函数调用
		//Ex:	printf("abc");
		if (ast_list[0].type==AST_TYPE::ast_call)
		{
			std::string fname=ast_list[0].value["name"][0].Value;
			std::vector<llvm::Value*> fargs;
			std::vector<TOKEN> args = ast_list[0].value["args"];
			while (!args.empty())
				if (args[0].Value == ",")
				{
					args.erase(args.begin());
				}
				else
				{
					fargs.push_back(irvalue(args, irinfo));
				}
			llvm::Function* function = irinfo.module->getFunction(fname);
			if (function)
			{
				irinfo.builder->CreateCall(function, fargs);
			}
			else
				ErrorExit( "未找到函数定义", ast_list[0].value["name"]);
			ast_list.erase(ast_list.begin());
			continue;
		}

		if (ast_list[0].type ==AST_TYPE::ast_codeblock)
		{
			//设置当前变量作用域
			VAR_LIST varlist;
			varlist.zone = "function";
			irinfo.varlist.push_back(varlist);

			ir_proc(ast_list[0].body,irinfo,false);
			
			irinfo.varlist.pop_back();

			ast_list.erase(ast_list.begin());
			continue;
		}

		//变量定义
		//Ex:	int a;
		//		int b=2;
		if (ast_list[0].type==AST_TYPE::ast_var)
		{
			//VAR_LIST var_list = irinfo.varlist.rbegin();
			VAR_INFO var_info;
			var_info.type= irtype(ast_list[0].value["type"], irinfo);
			var_info.value = irinfo.builder->CreateAlloca(var_info.type);
			irinfo.varlist[irinfo.varlist.size()-1].info[ast_list[0].value["name"][0].Value] = var_info;
			ast_list.erase(ast_list.begin());
			continue;
		}

		//变量赋值
		if (ast_list[0].type ==AST_TYPE::ast_expr)
		{
			//VAR_LIST var_list = irinfo.varlist.back();
			VAR_INFO var_info = irinfo.varlist[irinfo.varlist.size() - 1].info[ast_list[0].value["name"][0].Value];
			irinfo.builder->CreateStore(ir_expr(ast_list[0].value["value"],irinfo), var_info.value);
			ast_list.erase(ast_list.begin());
			continue;
		}

		ErrorExit("未识别标识", ast_list);
		break;
	}
	//return ast_list;
}


//	THE END