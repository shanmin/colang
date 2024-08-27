////////////////////////////////////////////////////////////////////////////////
//
//	AST_function
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"

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
		body = ast(tokens);
		//ErrorExit("函数体部分还未实现", tokens);
	}
	else
		ErrorExit("函数定义解析错误", tokens);
}


void AST_function::show(std::string pre)
{
	std::cout << pre << "\033[1m#TYPE:function\033[0m" << std::endl;
	std::cout << pre << " rett:";
	token_echo(rett, pre + "      ");
	std::cout << pre << " name:";
	token_echo(name, pre);
	std::cout << pre << " args:";
	token_echo(args, pre + "      ");
	if (!body.empty())
	{
		std::cout << pre << " body:" << std::endl;
		for (auto a : body)
			//token_echo(body, pre + "      ");
			a->show(pre + "    ");
	}
	std::cout << std::endl;
}


llvm::Value* AST_function::codegen()
{
	//args
	llvm::Type* frtype = ir_type(rett);
	std::string fname = name.Value;
	std::vector<llvm::Type*> fatype;
	std::vector<std::string> faname;
	std::vector<bool> faun;
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
			if (args[0].Value.substr(0, 1) == "u")
				faun.push_back(true);
			else
				faun.push_back(false);
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
		scope::push("function");
		llvm::Function::arg_iterator args = function->arg_begin();
		for (int i = 0; i < fatype.size(); i++)
		{
			VARINFO vi;
			vi.name = faname[i];
			vi.type = fatype[i];
			vi.value = args;// ir_builder->CreateAlloca(function->getArg(i)->getType(), function->getArg(i));
			scope::set(vi);
			args++;
		}
		//当前标签域
		LABEL_LIST lablelist;
		ir_labellist.push_back(lablelist);

		llvm::BasicBlock* old = ir_builder->GetInsertBlock();
		//创建进入标签
		llvm::BasicBlock* entry = llvm::BasicBlock::Create(ir_context, "", function);
		ir_builder->SetInsertPoint(entry);

		////创建 RETURN 返回值变量
		//VAR_INFO return_val;
		//if (!frtype->isVoidTy())
		//{
		//	return_val.type = frtype;
		//	return_val.value = ir_builder->CreateAlloca(function->getReturnType(), NULL, NULL, "__co__RETURN_VAL");
		//	varlist.info["__co__RETURN_VAL"] = return_val;
		//}
		//ir_varlist.push_back(varlist);

		////创建 RETURN 标签
		//llvm::BasicBlock* bb_return = llvm::BasicBlock::Create(ir_context, "__co__RETURN", function);
		//ir_builder->SetInsertPoint(bb_return);
		////提前插入返回指令
		//if (frtype->isVoidTy())
		//	ir_builder->CreateRetVoid();
		//else
		//{
		//	llvm::Value* v=ir_var_load(return_val);
		//	ir_builder->CreateRet(v);
		//}

		ir_builder->SetInsertPoint(entry);

		for (auto& a : body)
			a->codegen();

		//清理变量、标签作用域
		scope::pop();
		ir_labellist.pop_back();
		ir_builder->SetInsertPoint(old);
	}
	return function;
}


//	THE END