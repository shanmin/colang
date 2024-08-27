////////////////////////////////////////////////////////////////////////////////
//
// AST_expr
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

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
	std::cout << pre << "#TYPE:expr" << std::endl;
	std::cout << pre << " left:" << std::endl;
	left->show(pre + "      ");
	std::cout << pre << "   op:";
	token_echo(op, "           ");
	if (right)
	{
		std::cout << pre << "right:" << std::endl;
		right->show(pre + "      ");
	}
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
		VARINFO var_info =scope::get(((AST_value*)left)->value);
		llvm::Value* ret = right->codegen();
		ir_builder->CreateStore(ret, var_info.value);
		return ret;
	}

	llvm::Value* l = left->codegen();
	llvm::Value* r = right->codegen();

	if (l->getType() != r->getType())
		ErrorExit("表达式两边类型不一致", op);
	if (op.Value == "+") return ir_builder->CreateAdd(l, r);
	else if (op.Value == "-") return ir_builder->CreateSub(l, r);
	else if (op.Value == "*") return ir_builder->CreateMul(l, r);
	else if (op.Value == "/") return ir_builder->CreateSDiv(l, r);
	else if (op.Value == ">") return ir_builder->CreateICmpSGT(l, r);
	else if (op.Value == "<") return ir_builder->CreateICmpSLT(l, r);
	else if (op.Value == ">=") return ir_builder->CreateICmpSGE(l, r);
	else if (op.Value == "<=") return ir_builder->CreateICmpSLE(l, r);
	else if (op.Value == "==") return ir_builder->CreateICmpEQ(l, r);
	else if (op.Value == "!=") return ir_builder->CreateICmpNE(l, r);
	else if (op.Value == "&" or op.Value == "&&") return ir_builder->CreateAnd(l, r);
	else if (op.Value == "|" or op.Value == "||") return ir_builder->CreateOr(l, r);

	ErrorExit("不支持的运算符", op);
}
