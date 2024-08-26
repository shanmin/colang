////////////////////////////////////////////////////////////////////////////////
//
//	AST_function
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"

AST_function::AST_function(std::vector<TOKEN>& tokens)
{
	//����ֵ
	rett.push_back(tokens[0]);
	tokens.erase(tokens.begin());
	if (tokens[0].type != TOKEN_TYPE::string && (tokens[0].Value == "*" || tokens[0].Value == "&"))
	{
		rett.push_back(tokens[0]);
		tokens.erase(tokens.begin());
	}
	//������
	name = tokens[0];
	tokens.erase(tokens.begin());
	//����
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("��������������ֽ�������", tokens);

	//��������
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
	//�Ƴ�)
	if (tokens[0].Value == ")")
		tokens.erase(tokens.begin());
	else
		ErrorExit("��������������ִ���", tokens);
	//�жϺ����Ƿ���ں�����
	if (tokens[0].Value == ";")
		tokens.erase(tokens.begin());
	else if (tokens[0].Value == "{")
	{
		tokens.erase(tokens.begin());
		body = ast(tokens);
		//ErrorExit("�����岿�ֻ�δʵ��", tokens);
	}
	else
		ErrorExit("���������������", tokens);
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
		//���õ�ǰ����������
		VAR_LIST varlist;
		varlist.zone = "function";
		llvm::Function::arg_iterator args = function->arg_begin();
		for (int i = 0; i < fatype.size(); i++)
		{
			VAR_INFO vi;
			vi.type = fatype[i];
			vi.value = args;// ir_builder->CreateAlloca(function->getArg(i)->getType(), function->getArg(i));
			varlist.info[faname[i]] = vi;
			args++;
		}
		ir_varlist.push_back(varlist);
		//��ǰ��ǩ��
		LABEL_LIST lablelist;
		ir_labellist.push_back(lablelist);

		llvm::BasicBlock* old = ir_builder->GetInsertBlock();
		llvm::BasicBlock* entry = llvm::BasicBlock::Create(ir_context, "", function);
		ir_builder->SetInsertPoint(entry);

		

		for (auto& a : body)
			a->codegen();

		//return
		//ir_builder->CreateRetVoid();

		//������������ǩ������
		ir_varlist.pop_back();
		ir_labellist.pop_back();
		ir_builder->SetInsertPoint(old);

		//ir_builder-
	}
	return function;
}


//	THE END