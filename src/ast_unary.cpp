////////////////////////////////////////////////////////////////////////////////
//
// AST_unary — 一元前缀运算符
//
//   当前实现：~ 位取反（整型 CreateNot，等价 x ^ all-ones）
//              - 负号（整型 CreateNeg = 0-x，浮点 CreateFNeg）
//   将来扩展：! 逻辑非（icmp eq 0）
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

void AST_unary::show(std::string pre)
{
	std::cout << pre << "#TYPE:unary" << std::endl;
	std::cout << pre << "   op:";
	token_echo(op, pre + "      ");
	std::cout << pre << "operand:" << std::endl;
	operand->show(pre + "      ");
	std::cout << std::endl;
}

llvm::Value* AST_unary::codegen()
{
	llvm::Value* v = operand->codegen();
	if (!v) ErrorExit("unary operator: invalid operand", op);

	//i1 整型提升（模拟 C 的 _Bool 在一元运算中提升到 int 的语义）：
	//	~true 在 i1 上是 ~1 = 0（i1），但 C 的 _Bool 提升后 ~true = ~1 = -2（int）
	//	-true 在 i1 上是 0-1 = 1（i1，因为 -1 ≡ 1 mod 2），但 C 提升后 -true = -1（int）
	//	与 ast_expr.cpp 的二元运算 i1 提升保持一致；浮点不受影响（i1 不可能是浮点）
	if (v->getType()->isIntegerTy(1))
		v = ir_builder->CreateZExt(v, llvm::Type::getInt32Ty(ir_context));

	if (op.Value == "~")
	{
		// ~ 位取反：仅整型支持。CreateNot 内部生成 XOR(v, allones)
		if (!v->getType()->isIntegerTy())
			ErrorExit("operator '~' only supports integer types (float not supported)", op);
		return ir_builder->CreateNot(v);
	}

	if (op.Value == "-")
	{
		// - 一元负号：算术类型支持。整型 CreateNeg（0 - x），浮点 CreateFNeg
		if (v->getType()->isFloatingPointTy())
			return ir_builder->CreateFNeg(v);
		if (v->getType()->isIntegerTy())
			return ir_builder->CreateNeg(v);
		ErrorExit("unary '-' only supports arithmetic types (integer/float)", op);
	}

	ErrorExit("unsupported unary operator", op);
}
