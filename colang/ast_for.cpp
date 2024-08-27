
////////////////////////////////////////////////////////////////////////////////
//
// AST_for
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_for::AST_for(std::vector<TOKEN>& tokens)
{
	//名称
	name = tokens[0];
	tokens.erase(tokens.begin());
	//参数
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("for定义参数部分解析错误", tokens);

	//解析参数
	if (tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::code && tokens[1].Value != "(")
	{
		new AST_var(tokens);
	}
	expr1 = ast_parse_expr(tokens);
	//移除;
	if (tokens[0].Value == ";")
		tokens.erase(tokens.begin());
	else
		ErrorExit("for定义部分1错误", tokens);

	expr2 = ast_parse_expr(tokens);
	//移除;
	if (tokens[0].Value == ";")
		tokens.erase(tokens.begin());
	else
		ErrorExit("for定义部分2错误", tokens);

	expr3 = ast_parse_expr(tokens);
	//）在上面表达式读取时已经移除
	////移除)
	//if (tokens[0].Value == ")")
	//	tokens.erase(tokens.begin());
	//else
	//	ErrorExit("for定义部分3错误", tokens);

	//判断后续是否存在函数体
	if (tokens[0].Value == ";")
	{
		tokens.erase(tokens.begin());
		return;
	}
	body = ast1(tokens);

	if (tokens[0].Value == ";")
	{
		tokens.erase(tokens.begin());
	}
}
void AST_for::show(std::string pre)
{
	std::cout << pre << "#TYPE:for" << std::endl;
	std::cout << pre << " expr1:" << std::endl;
	expr1->show(pre + "      ");
	std::cout << pre << " expr2:" << std::endl;
	expr2->show(pre + "      ");
	std::cout << pre << " expr3:" << std::endl;
	expr3->show(pre + "      ");
	if (body)
	{
		std::cout << pre << " body:" << std::endl;
		body->show(pre + "      ");
	}
	std::cout << std::endl;
}
llvm::Value* AST_for::codegen()
{
	//for (for1; for2; for3)
	//	for4;
	//
	//for1
	//if(for2) for4 else for3
	//for4
	//for3
	//goto for2;

	//llvm::BasicBlock* bb = ir_builder->GetInsertBlock();

	llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	llvm::BasicBlock* bbexpr = llvm::BasicBlock::Create(ir_context, "", func);
	llvm::BasicBlock* bbbody = llvm::BasicBlock::Create(ir_context, "", func);
	llvm::BasicBlock* bbover = llvm::BasicBlock::Create(ir_context, "", func);

	expr1->codegen();
	ir_builder->CreateBr(bbexpr); //必须有一个跳转，好让前面的BasicBlock结束

	ir_builder->SetInsertPoint(bbexpr);
	llvm::Value* expr2v = ir_type_conver(expr2->codegen(), llvm::Type::getInt1Ty(ir_context));
	ir_builder->CreateCondBr(expr2v, bbbody, bbover);

	ir_builder->SetInsertPoint(bbbody);
	if (body)
		body->codegen();
	expr3->codegen();
	ir_builder->CreateBr(bbexpr);
	//ir_builder->CreateBr(forend);

	ir_builder->SetInsertPoint(bbover);
	//ir_builder->SetInsertPoint(bb);

	return nullptr;
}

