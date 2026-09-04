////////////////////////////////////////////////////////////////////////////////
//
// AST_label
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_label::AST_label(std::vector<TOKEN>& tokens)
{
	//名称
	name = tokens[0];
	tokens.erase(tokens.begin());
	//参数
	// FIX（2026-09-02，P0#1）：tokens.erase(name) 后空，tokens[0] UB
	if (tokens.empty()) ErrorExit("label: missing ':'", name);
	if (tokens[0].Value == ":")
		tokens.erase(tokens.begin());
	else
		ErrorExit("label: parse error", tokens);
}
void AST_label::show(std::string pre)
{
	std::cout << pre << "#TYPE:label    " << name.Value << std::endl << std::endl << std::endl;
}
llvm::Value* AST_label::codegen()
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
	ir_builder->SetInsertPoint(bbstart);

	return nullptr;
}
