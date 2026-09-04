
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
	// FIX（2026-09-02，P0#1）：判空再取 "("
	if (tokens.empty()) ErrorExit("for: missing '('", name);
	if (tokens[0].Value == "(")
		tokens.erase(tokens.begin());
	else
		ErrorExit("for: condition parse error", tokens);

	//解析参数
	// FIX（2026-09-02，P0#1）：tokens[1] 访问前判 size>=2
	if (tokens.size() >= 2 && tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::code && tokens[1].Value != "(")
	{
		var=new AST_var(tokens);
	}
	expr1 = ast_parse_expr(tokens);
	//移除;
	// FIX（2026-09-02，P0#1）：ast_parse_expr 可能消费 tokens 直到空
	if (tokens.empty()) ErrorExit("for: missing 1st ';'", name);
	if (tokens[0].Value == ";")
		tokens.erase(tokens.begin());
	else
		ErrorExit("for: part 1 parse error", tokens);

	expr2 = ast_parse_expr(tokens);
	//移除;
	if (tokens.empty()) ErrorExit("for: missing 2nd ';'", name);
	if (tokens[0].Value == ";")
		tokens.erase(tokens.begin());
	else
		ErrorExit("for: part 2 parse error", tokens);

	expr3 = ast_parse_expr(tokens);
	//）在上面表达式读取时已经移除

	//判断后续是否存在函数体
	if (!tokens.empty() && tokens[0].Value == ";")
	{
		tokens.erase(tokens.begin());
		return;
	}
	body = ast1(tokens);

	if (!tokens.empty() && tokens[0].Value == ";")
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
	// layout:
	//   for1
	//   br bbexpr
	// bbexpr:
	//   for2 -> condbr bbbody, bbover
	// bbbody:
	//   body               <- break N -> bbover 最内层 (stack.back)
	//   br bbcontinue
	// bbcontinue:
	//   for3               <- continue N -> bbcontinue 最内层
	//   br bbexpr
	// bbover:
	//   (NOP)

	//因为在for里面会新定义变量，所以这里单独作用域 20240827 shanmin
	scope::push("for");

	llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	llvm::BasicBlock* bbexpr = llvm::BasicBlock::Create(ir_context, "for_expr", func);
	llvm::BasicBlock* bbbody = llvm::BasicBlock::Create(ir_context, "for_body", func);
	llvm::BasicBlock* bbcontinue = llvm::BasicBlock::Create(ir_context, "for_cont", func);
	llvm::BasicBlock* bbover = llvm::BasicBlock::Create(ir_context, "for_over", func);

	scope::push_loop_bb(bbover, bbcontinue);

	if (var)
		var->codegen();
	if(expr1)
		expr1->codegen();
	ir_builder->CreateBr(bbexpr); //必须有一个跳转，好让前面的BasicBlock结束

	ir_builder->SetInsertPoint(bbexpr);
	llvm::Value* expr2v = ir_type_conver(expr2->codegen(), llvm::Type::getInt1Ty(ir_context));
	ir_builder->CreateCondBr(expr2v, bbbody, bbover);

	ir_builder->SetInsertPoint(bbbody);
	if (body)
		body->codegen();
	ir_builder->CreateBr(bbcontinue);

	ir_builder->SetInsertPoint(bbcontinue);
	expr3->codegen();
	ir_builder->CreateBr(bbexpr);

	ir_builder->SetInsertPoint(bbover);

	scope::pop_loop_bb();
	scope::pop();

	return nullptr;
}

