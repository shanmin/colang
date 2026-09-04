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
	// FIX（2026-09-02，P0#1）：tokens.erase(name) 后空，tokens[0] UB
	if (tokens.empty()) ErrorExit("if: missing '('", name);
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("if: condition parse error", tokens);

	//解析参数
	expr1 = ast_parse_expr(tokens);

	//判断后续是否存在函数体
	// FIX（2026-09-02，P0#1）：ast_parse_expr 消费到 tokens 空时先判空
	if (tokens.empty()) return;
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
	//FIX（2026-09-01）：LLVM 23 的 getTerminator() 在块无 terminator 时返回 &InstList.back()（非空！），
	//   判空/判 terminator 必须用 getTerminatorOrNull()；否则空块还会触发 UB。
	last_instruction = ir_builder->GetInsertBlock()->getTerminatorOrNull();
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
	last_instruction = ir_builder->GetInsertBlock()->getTerminatorOrNull();
	if (last_instruction != NULL && last_instruction->getOpcode() == llvm::Instruction::TermOps::Ret)
	{
	}
	else
		ir_builder->CreateBr(enddbb); //这个结束跳转没有意义了

	ir_builder->SetInsertPoint(enddbb);



	return nullptr;
}
