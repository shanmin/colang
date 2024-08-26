
////////////////////////////////////////////////////////////////////////////////
//
// AST_goto
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_goto::AST_goto(std::vector<TOKEN>& tokens)
{
	//返回值
	tokens.erase(tokens.begin());
	if (tokens[0].type == TOKEN_TYPE::code)
	{
		name = tokens[0];
		tokens.erase(tokens.begin());
	}
	else
		ErrorExit("goto部分解析错误", tokens);

	//if (tokens[0].Value == ";")
	//	tokens.erase(tokens.begin());
	//else
	//	ErrorExit("goto部分结束符错误", tokens);
}
void AST_goto::show(std::string pre)
{
	std::cout << pre << "#TYPE:goto" << std::endl;
	token_echo(name, pre + "");
	std::cout << std::endl;
}
llvm::Value* AST_goto::codegen()
{
	llvm::BasicBlock* bbstart;
	//如果goto在前，则前面已经创建这个标签了
	LABEL_LIST labellist = ir_labellist.back();
	if (labellist.info.find(name.Value) == labellist.info.end())
	{
		llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
		bbstart = llvm::BasicBlock::Create(ir_context, name.Value, func);
		ir_labellist[ir_labellist.size() - 1].info[name.Value] = bbstart;
	}
	else
	{
		bbstart = labellist.info[name.Value];
	}
	ir_builder->CreateBr(bbstart);

	//goto后面要新常见一个basic block，否则会出现“Terminator found in the middle of a basic block!”错误信息
	{
		llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
		llvm::BasicBlock* bbstart1 = llvm::BasicBlock::Create(ir_context, "", func);
		ir_builder->SetInsertPoint(bbstart1);
	}

	return nullptr;
}
