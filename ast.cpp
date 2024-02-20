//
//	ast.cpp
//

#include <llvm/IR/Value.h>
#include "colang.h"


llvm::LLVMContext ir_context;
llvm::Module* ir_module;
std::unique_ptr<llvm::IRBuilder<>> ir_builder;
std::vector<VAR_LIST> ir_varlist; //局部变量范围


void ir(std::vector<AST*>& ast_list, const char* filename)
{
	ir_module = new llvm::Module(filename, ir_context);
	ir_builder = std::make_unique<llvm::IRBuilder<>>((ir_context));

	//设置当前变量作用域
	VAR_LIST varlist;
	varlist.zone = "global";
	ir_varlist.push_back(varlist);

	llvm::FunctionType* mainType = llvm::FunctionType::get(ir_builder->getInt32Ty(), false);
	llvm::Function* main = llvm::Function::Create(mainType, llvm::GlobalValue::ExternalLinkage, "main", ir_module);
	llvm::BasicBlock* entryMain = llvm::BasicBlock::Create(ir_context, "entry_main", main);
	ir_builder->SetInsertPoint(entryMain);

	//ir_proc(tokens, true);
	for (auto a : ast_list)
		a->codegen();

	llvm::ConstantInt* ret = ir_builder->getInt32(0);
	ir_builder->CreateRet(ret);
	llvm::verifyFunction(*main);

	//验证模块是否存在问题
	std::string mstr;
	llvm::raw_string_ostream merr(mstr);
	bool result = llvm::verifyModule(*ir_module, &merr);
	if (result)
	{
		printf("模块存在错误！%s", mstr.c_str());
		exit(2);
	}

	ir_module->print(llvm::outs(), nullptr);

	//输出ll格式文件
	std::string col = std::string(filename) + "l";
	std::error_code ec;
	llvm::raw_fd_ostream fout(col, ec);
	ir_module->print(fout, nullptr);
	fout.close();
	printf("\n---------- write ll ----------\n");
	printf("save=%x\n", ec.value());

	std::string cob = std::string(filename) + "b";
	llvm::raw_fd_ostream fout_cob(cob, ec);
	llvm::WriteBitcodeToFile(*ir_module, fout_cob);
	fout_cob.close();
	printf("\n---------- write bc ----------\n");
	printf("save=%x\n", ec.value());
}


llvm::Type* ir_type(std::vector<TOKEN>& tokens)
{
	std::string name = tokens[0].Value;
	if (tokens.size() > 1 && tokens[1].Value == "*")
	{
		name += "*";
	}

	llvm::Type* type = NULL;
	if (name == "void") type = llvm::Type::getVoidTy(ir_context);
	if (name == "int8" || name == "byte" || name == "char") type = llvm::Type::getInt8Ty(ir_context);
	if (name == "int8*" || name == "byte*" || name == "char*") type = llvm::Type::getInt8PtrTy(ir_context);
	if (name == "int16" || name == "short") type = llvm::Type::getInt16Ty(ir_context);
	if (name == "int16*" || name == "short*") type = llvm::Type::getInt16PtrTy(ir_context);
	if (name == "int32" || name == "int") type = llvm::Type::getInt32Ty(ir_context);
	if (name == "int32*" || name == "int*") type = llvm::Type::getInt32PtrTy(ir_context);
	if (name == "int64" || name == "long") type = llvm::Type::getInt64Ty(ir_context);
	if (name == "int64*" || name == "long*") type = llvm::Type::getInt64PtrTy(ir_context);
	if (name == "float") type = llvm::Type::getFloatTy(ir_context);
	if (name == "float*") type = llvm::Type::getFloatPtrTy(ir_context);
	if (name == "double") type = llvm::Type::getDoubleTy(ir_context);
	if (name == "double*") type = llvm::Type::getDoublePtrTy(ir_context);

	if (type == NULL)
		ErrorExit("数据类型定义错误", tokens);

	tokens.erase(tokens.begin());
	if (tokens.size() > 0 && tokens[0].Value == "*")
	{
		tokens.erase(tokens.begin());
	}
	return type;
}

//查找变量
VAR_INFO ir_var(std::string name, std::vector<VAR_LIST> var_list, TOKEN token)
{
	//从下往上查找变量定义
	while (!var_list.empty())
	{
		VAR_LIST vlist = var_list.back();
		var_list.pop_back();

		if (vlist.info.find(name) == vlist.info.end())
		{
			std::vector<TOKEN> tmp;
			tmp.push_back(token);
			ErrorExit("ERROR: 变量不存在", tmp);
		}
		VAR_INFO vinfo = vlist.info[name];
		if (vinfo.value)
		{
			return vinfo;
		}
	}
}
//读取变量值
llvm::Value* ir_var_load(VAR_INFO& var_info)
{
	return ir_builder->CreateLoad(var_info.type, var_info.value);
}

std::map<std::string,int> IR_EXPR_PRI =
{
	{"*",30},
	{"/",30},
	{"+",20},
	{"-",20},
	{"=",10}
};


//读取一个表达式
AST* ast_parse_expr1(std::vector<TOKEN>& tokens);
//解析表达式
AST* ast_parse_expr(std::vector<TOKEN>& tokens, int left_pri = 0, AST* left = NULL)
{
	if (left_pri == 0) //首次读取
	{
		//if (tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "(")
		//{
		//	tokens.erase(tokens.begin());
		//	left = ast_parse_expr(tokens);
		//}
		//else
		//	left = new AST_value(tokens);
		left = ast_parse_expr1(tokens);
	}

	while (!tokens.empty())
	{
		int right_pri = 0;
		TOKEN op;
		//读取下一个操作符，计算操作优先级
		if (tokens[0].type == TOKEN_TYPE::opcode)
		{
			if (tokens[0].Value == ")")
			{
				tokens.erase(tokens.begin());
				return left;
			}
			if(tokens[0].Value==";" || tokens[0].Value==",")
				return left;
			if (IR_EXPR_PRI.find(tokens[0].Value) == IR_EXPR_PRI.end())
				ErrorExit("未识别的运算符", tokens);
			right_pri = IR_EXPR_PRI[tokens[0].Value];
			op = tokens[0];
			tokens.erase(tokens.begin());
		}
		if (right_pri < left_pri)
			return left;

		AST* right = ast_parse_expr1(tokens);

		int next_pri = 0;
		if (tokens[0].type == TOKEN_TYPE::opcode)
		{
			if (IR_EXPR_PRI.find(tokens[0].Value) != IR_EXPR_PRI.end())
				next_pri = IR_EXPR_PRI[tokens[0].Value];
		}

		if(right_pri<next_pri)
		{
			right = ast_parse_expr(tokens, right_pri, right);
			if (!right)
				return nullptr;
		}
		left = new AST_expr(left, op, right);


		//AST* right = ast_parse_expr1(tokens);
		//if (right_pri > left_pri)
		//{
		//	right = ast_parse_expr(tokens, right_pri, right);
		//	if (!right)
		//		return nullptr;
		//}
		//left = new AST_expr(left, op, right);
		//left_pri = right_pri;
	}

	return left;
}
//读取一个表达式
AST* ast_parse_expr1(std::vector<TOKEN>& tokens)
{
	if (tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "(")
	{
		return new AST_call(tokens);
	}
	if (tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "(")
	{
		tokens.erase(tokens.begin());
		return ast_parse_expr(tokens);
	}
	return new AST_value(tokens);
}


////////////////////////////////////////////////////////////////////////////////
//
// AST_call
//
////////////////////////////////////////////////////////////////////////////////


AST_call::AST_call(std::vector<TOKEN>& tokens)
{
	//名称
	name = tokens[0];
	tokens.erase(tokens.begin());

	//参数
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("函数调用解析错误", tokens);
	//解析参数
	while (!tokens.empty())
	{
		if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == ")") break;
		//args.push_back(new AST_expr(tokens));
		////args.push_back(tokens[0]);
		args.push_back(ast_parse_expr(tokens));
		if (tokens[0].Value == ",")
			tokens.erase(tokens.begin());
		else if (tokens[0].Value == ")")
		{
			tokens.erase(tokens.begin());
			return;
		}
		else if (tokens[0].Value == ";")
			return;
		else
			ErrorExit("函数调用参数解析错误(1)", tokens);
	}
	if (tokens[0].Value == ")")
		tokens.erase(tokens.begin());
	else
		ErrorExit("函数调用参数解析错误", tokens);
}
void AST_call::show(std::string pre)
{
	std::cout << pre << "#TYPE:call" << std::endl;
	std::cout << pre << " name:";
	token_echo(name,"");
	std::cout << pre << " args:" << std::endl;
	for (auto a : args)
		a->show(pre + "      ");
	std::cout << std::endl;
}
llvm::Value* AST_call::codegen()
{
	//函数调用
	//Ex:	printf("abc");
	std::vector<llvm::Value*> fargs;
	for (auto a : args)
		fargs.push_back(a->codegen());
	llvm::Function* function = ir_module->getFunction(name.Value);
	if (function)
	{
		return ir_builder->CreateCall(function, fargs);
	}
	else
		ErrorExit("未找到函数定义",name);
	return NULL;
}


////////////////////////////////////////////////////////////////////////////////
//
// AST_codeblock
//
////////////////////////////////////////////////////////////////////////////////

AST_codeblock::AST_codeblock(std::vector<TOKEN>& tokens)
{
	if (tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "{")
		tokens.erase(tokens.begin());
	body = ast(tokens);
}
void AST_codeblock::show(std::string pre)
{
	std::cout << pre << "#TYPE:codeblock" << std::endl;
	for (AST* a : body)
	{
		a->show(pre + "      ");
	}
	std::cout << std::endl;
}
llvm::Value* AST_codeblock::codegen()
{
	//设置当前变量作用域
	VAR_LIST varlist;
	varlist.zone = "codeblock";
	ir_varlist.push_back(varlist);

	//llvm::Function* TheFunction = ir_builder->GetInsertBlock()->getParent();
	//llvm::BasicBlock* bb = llvm::BasicBlock::Create(ir_context,"codeblock",TheFunction);
	//ir_builder->SetInsertPoint(bb);
	llvm::Value* bb = nullptr;

	for (auto a : body)
		a->codegen();
			
	ir_varlist.pop_back();
	return bb;
}


////////////////////////////////////////////////////////////////////////////////
//
// AST_expr
//
////////////////////////////////////////////////////////////////////////////////


//AST_expr::AST_expr(std::vector<TOKEN>& tokens)
//{
//	while (true)
//	{
//		AST_BINARYOP bop;
//		if (tokens[0].type == TOKEN_TYPE::opcode)
//		{
//			if (tokens[0].Value == ";" || tokens[0].Value == ",")
//				break;
//			if (tokens[0].Value == ")")
//			{
//				tokens.erase(tokens.begin());
//				break;
//			}
//			bop.op = tokens[0];
//			bop.op_pri = IR_EXPR_PRI[bop.op.Value];
//			tokens.erase(tokens.begin());
//		}
//		if (tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "(")
//		{
//			tokens.erase(tokens.begin());
//			bop.right = new AST_expr(tokens);
//		}
//		else
//			bop.right = new AST_value(tokens);
//		blist.push_back(bop);
//	}
//}
//AST_expr::AST_expr(AST* left, TOKEN op, AST* right)
//{
//	this->left = left;
//	this->op = op;
//	this->right = right;
//	//填充运算符优先级
//	if (IR_EXPR_PRI.find(op.Value) == IR_EXPR_PRI.end())
//		ErrorExit("未识别的运算符", op);
//	op_pri = IR_EXPR_PRI[op.Value];
//}
void AST_expr::show(std::string pre)
{
	std::cout<<pre << "#TYPE:expr" << std::endl;
	std::cout << pre << " left:" << std::endl;
	left->show(pre+"      ");
	std::cout << pre << "   op:";
	token_echo(op, "           ");
	std::cout << pre << "right:" << std::endl;
	right->show(pre+"      ");
	std::cout << std::endl;
}
llvm::Value* AST_expr::codegen()
{
	//	////转换
	//	//ir_value(*current,irinfo);
	//	//只有一个值，不需计算的处理
	//	if (right == NULL)
	//	{
	//		value=current.right_value;
	//		list.erase(list.begin()+index-1);
	//		continue;
	//	}

	//判断赋值操作
	if (op.Value == "=")
	{
		VAR_INFO var_info = ir_var(((AST_value*)left)->value.Value, ir_varlist, ((AST_value*)left)->value);
		return ir_builder->CreateStore(right->codegen(), var_info.value);
	}

	llvm::Value* l = left->codegen();
	llvm::Value* r = right->codegen();
	if (l->getType() != r->getType())
		ErrorExit("表达式两边类型不一致", op);
	if (op.Value == "+")
	{
		return ir_builder->CreateAdd(l, r);
	}
	if (op.Value == "-")
	{
		return ir_builder->CreateSub(l, r);
	}
	if (op.Value == "*")
	{
		return ir_builder->CreateMul(l, r);
	}
	if (op.Value == "/")
	{
		return ir_builder->CreateSDiv(l, r);
	}
	ErrorExit("不支持的运算符", op);
}



////////////////////////////////////////////////////////////////////////////////
//
// AST_function
//
////////////////////////////////////////////////////////////////////////////////


AST_function::AST_function(std::vector<TOKEN>& tokens)
{
	//返回值
	rett.push_back(tokens[0]);
	tokens.erase(tokens.begin());
	if (tokens[0].type != TOKEN_TYPE::string && (tokens[0].Value == "*" || tokens[0].Value == "&"))
	{
		rett.push_back(tokens[0]);
		tokens.erase(tokens.begin());
	}
	//函数名
	name = tokens[0];
	tokens.erase(tokens.begin());
	//参数
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
			args.push_back(tokens[0]);
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
		//body = ast(tokens);
		ErrorExit("函数体部分还未实现", tokens);
	}
	else
		ErrorExit("函数定义解析错误", tokens);
}
void AST_function::show(std::string pre)
{
	std::cout << pre << "#TYPE:function" << std::endl;
	std::cout << pre << " rett:";
	token_echo(rett, pre + "      ");
	std::cout << pre << " name:";
	token_echo(name, pre);
	std::cout << pre << " args:";
	token_echo(args, pre + "      ");
	std::cout << std::endl;
}
llvm::Value* AST_function::codegen()
{
	//args
	llvm::Type* frtype = ir_type(rett);
	std::string fname = name.Value;
	std::vector<llvm::Type*> fatype;
	std::vector<std::string> faname;
	bool isVarArg = false;
	while (!args.empty())
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
			fatype.push_back(ir_type(args));
			if (!args.empty())
			{
				faname.push_back(args[0].Value);
				args.erase(args.begin());
			}
			else
				faname.push_back("");
		}
	llvm::FunctionType* functionType = llvm::FunctionType::get(frtype, fatype, isVarArg);
	llvm::Function* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage, fname, ir_module);
	////arg name
	//unsigned i = 0;
	//for (auto& a : function->args())
	//{
	//	if(faname[i]!="")
	//		a.setName(faname[i]);
	//}
	if (!body.empty())
	{
		//设置当前变量作用域
		VAR_LIST varlist;
		varlist.zone = "function";
		ir_varlist.push_back(varlist);

		for (auto& a : body)
			a.codegen();

		ir_varlist.pop_back();
	}
	return function;
}


////////////////////////////////////////////////////////////////////////////////
//
// AST_noncode
//
////////////////////////////////////////////////////////////////////////////////


AST_noncode::AST_noncode(std::vector<TOKEN>& tokens)
{
	value = tokens[0];
	tokens.erase(tokens.begin());
}
void AST_noncode::show(std::string pre)
{
	std::cout << pre << "#TYPE:noncode" << std::endl;
	std::cout << pre << "value:";
	token_echo(value, pre + "      ");
	std::cout << std::endl;
}
llvm::Value* AST_noncode::codegen()
{
	llvm::Function* function = ir_module->getFunction("printf");
	if (function)
	{
		llvm::Value* args = ir_builder->CreateGlobalStringPtr(value.Value);
		ir_builder->CreateCall(function, args);
	}
	else
		ErrorExit("未找到非代码输出函数",value);
	return NULL;
}


////////////////////////////////////////////////////////////////////////////////
//
// AST_value
//
////////////////////////////////////////////////////////////////////////////////


AST_value::AST_value(std::vector<TOKEN>& tokens)
{
	value = tokens[0];
	tokens.erase(tokens.begin());
}
void AST_value::show(std::string pre)
{
	std::cout<<pre << "#TYPE:value";
	token_echo(value,"");
	std::cout << std::endl;
}
llvm::Value* AST_value::codegen()
{
	if (value.type == TOKEN_TYPE::string)
		return ir_builder->CreateGlobalStringPtr(value.Value);

	if (value.type == TOKEN_TYPE::number)
	{
		int v = atoi(value.Value.c_str());
		return ir_builder->getInt32(v);
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


////////////////////////////////////////////////////////////////////////////////
//
// AST_var
//
////////////////////////////////////////////////////////////////////////////////


AST_var::AST_var(std::vector<TOKEN>& tokens)
{
	//类型
	type.push_back(tokens[0]);
	tokens.erase(tokens.begin());
	if (tokens[0].type != TOKEN_TYPE::string && (tokens[0].Value == "*" || tokens[0].Value == "&"))
	{
		type.push_back(tokens[0]);
		tokens.erase(tokens.begin());
	}
	//名称
	name = tokens[0];
	//如果结束符，吃掉当前变量名称
	if (tokens[1].Value == ";")
		tokens.erase(tokens.begin());
}
void AST_var::show(std::string pre)
{
	std::cout << pre << "#TYPE:var" << std::endl;
	std::cout << pre << " type:";
	token_echo(type, pre + "      ");
	std::cout << pre << " name:";
	token_echo(name, pre);
	std::cout << std::endl;
}
llvm::Value* AST_var::codegen()
{
	//创建声明变量的占位
	VAR_INFO var_info;
	var_info.type= ir_type(type);
	var_info.value = ir_builder->CreateAlloca(var_info.type);
	ir_varlist[ir_varlist.size()-1].info[name.Value] = var_info;
	return var_info.value;
}


////////////////////////////////////////////////////////////////////////////////
//
// ast
//
////////////////////////////////////////////////////////////////////////////////


std::vector<AST*> ast(std::vector<TOKEN>& tokens)
{
	std::vector<AST*> ast_list;
	while (!tokens.empty())
	{
		if (tokens[0].type == TOKEN_TYPE::noncode)//非代码，直接输出
		{
			ast_list.push_back(new AST_noncode(tokens));
			continue;
		}

		if (tokens[0].type == TOKEN_TYPE::string)
		{
			//ast_list.push_back(new AST_expr(tokens));
			ast_list.push_back(ast_parse_expr(tokens));
			continue;
		}

		if (tokens[0].type != TOKEN_TYPE::string)
		{
			if (tokens[0].Value == "{")
			{
				ast_list.push_back(new AST_codeblock(tokens));
				continue;
			}
			else if (tokens[0].Value == "}")
			{
				tokens.erase(tokens.begin());
				break;
			}
			else if (tokens[0].Value == ";")
			{
				tokens.erase(tokens.begin());
				continue;
			}
		}

		//Function
		//	Ex: int function_name(int arg1)
		//		 |		|              + args
		//       |      + 函数名称
		//       +  返回值类型
		//判断依据：当前为字符，下一个token是一个字符，再下一个是(
		if (tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::code && tokens[2].type == TOKEN_TYPE::opcode && tokens[2].Value == "(")
		{
			ast_list.push_back(new AST_function(tokens));
			continue;
		}

		//函数调用
		//Ex:	printf("abc");
		if (tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "(")
		{
			ast_list.push_back(new AST_call(tokens));
			continue;
		}

		//变量定义
		//Ex:	int a;
		//		int b=2;
		if (tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::code && tokens[1].Value != "(")
		{
			ast_list.push_back(new AST_var(tokens));
			continue;
		}

		//ast_list.push_back(new AST_expr(tokens));
		ast_list.push_back(ast_parse_expr(tokens));
		continue;

		std::cout << "未处理标识:" << tokens[0].Value << std::endl;
		tokens.erase(tokens.begin());
	}
	return ast_list;
}

void ast_echo(std::vector<AST*> ast_list, std::string pre)
{
	for (AST* a : ast_list)
	{
		a->show(pre);
	}
}

//	THE END