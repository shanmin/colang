////////////////////////////////////////////////////////////////////////////////
//
// AST_if
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_if::AST_if(std::vector<TOKEN>& tokens)
{
	//名称
	name = tokens[0];
	tokens.erase(tokens.begin());
	//参数
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("if定义参数部分解析错误", tokens);

	//std::vector<TOKEN> e = get_tokens(tokens, ")");
	//expr1 = ast_parse_expr(e);

	//解析参数
	expr1 = ast_parse_expr(tokens);
	//while (!tokens.empty())
	//	if (tokens[0].Value == ")")
	//	{
	//		break;
	//	}
	//	else
	//	{
	//		expr1.push_back(tokens[0]);
	//		tokens.erase(tokens.begin());
	//	}
	// 
	////移除)
	//if (tokens[0].Value == ")")
	//	tokens.erase(tokens.begin());
	//else
	//	ErrorExit("if定义结束部分错误", tokens);

	//判断后续是否存在函数体
	if (tokens[0].Value == ";")
	{
		tokens.erase(tokens.begin());
		return;
	}
	thenbody = ast1(tokens);
	
	if (tokens.empty())
		return;

	if (tokens[0].Value == ";")
	{
		tokens.erase(tokens.begin());
	}
	if (!tokens.empty() && tokens[0].Value == "else")
	{
		tokens.erase(tokens.begin());
		elsebody = ast1(tokens);
	}

}


void AST_if::show(std::string pre)
{
	std::cout << pre << "#TYPE:if" << std::endl;
	std::cout << pre << " expr:" << std::endl;
	expr1->show(pre + "      ");
	if (thenbody)
	{
		std::cout << pre << " then:" << std::endl;
		thenbody->show(pre + "      ");
	}
	if (elsebody)
	{
		std::cout << pre << " else:" << std::endl;
		elsebody->show(pre + "      ");
	}
	std::cout << std::endl;
}


llvm::Value* AST_if::codegen()
{
	//llvm::BasicBlock* bb = ir_builder->GetInsertBlock();

	llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	llvm::BasicBlock* thenbb = llvm::BasicBlock::Create(ir_context, "then", func);
	llvm::BasicBlock* elsebb = llvm::BasicBlock::Create(ir_context, "else", func);
	llvm::BasicBlock* enddbb = llvm::BasicBlock::Create(ir_context, "endd", func);

	llvm::Value* expr1v = ir_type_conver(expr1->codegen(), llvm::Type::getInt1Ty(ir_context));
	ir_builder->CreateCondBr(expr1v, thenbb, elsebb);

	ir_builder->SetInsertPoint(thenbb);
	if (thenbody)
	{
		llvm::Value* cv = thenbody->codegen();
		////llvm::Type* ct = cv->getType();
		////if (typeid(cv) == typeid(llvm::ReturnInst))
		//if(cv!=NULL)
		//	if(llvm::ReturnInst::classof(cv))
		//		printf("\n---------- class name : %d ----------\n", llvm::ReturnInst::classof(cv));

	}
	llvm::Instruction* last_instruction;
	//获取当前代码块最后一条指令，如果指令是return则不创建后面的br指令
	last_instruction = ir_builder->GetInsertBlock()->getTerminator();
	if(last_instruction!=NULL && last_instruction->getOpcode()== llvm::Instruction::TermOps::Ret)
	{ }
	else
		ir_builder->CreateBr(enddbb);

	ir_builder->SetInsertPoint(elsebb);
	if (elsebody)
	{
		elsebody->codegen();
	}
	//获取当前代码块最后一条指令，如果指令是return则不创建后面的br指令
	last_instruction = ir_builder->GetInsertBlock()->getTerminator();
	if (last_instruction != NULL && last_instruction->getOpcode() == llvm::Instruction::TermOps::Ret)
	{
	}
	else
		ir_builder->CreateBr(enddbb); //这个结束跳转没有意义了

	ir_builder->SetInsertPoint(enddbb);



	return nullptr;
}
