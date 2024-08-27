////////////////////////////////////////////////////////////////////////////////
//
// AST_return
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_return::AST_return(std::vector<TOKEN>& tokens)
{
	this->token = tokens[0];
	tokens.erase(tokens.begin());
	if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value != ";")
		value = ast_parse_expr(tokens);
}


void AST_return::show(std::string pre)
{
	std::cout << pre << "#TYPE:return" << std::endl;
	if (value)
	{
		std::cout << pre << "value:";
		value->show(pre + "      ");
	}
	std::cout << std::endl;
}


llvm::Value* AST_return::codegen()
{
	if (value)
		return ir_builder->CreateRet(value->codegen());
	else
		return ir_builder->CreateRetVoid();

	////假设代码：
	////	int ff()
	////	{
	////		if(1)
	////			return 1;
	////		return 0;
	////	}
	////	按照现在生成的方式，生成的bc码如下：
	////	define i32 @ff() {
	////		br i1 true, label %then, label %else
	////	then:                                             ; preds = %0
	////		ret i32 1
	////		br label %endd
	////	else:                                             ; preds = %0
	////		br label %endd
	////	endd:                                             ; preds = %else, %then
	////		ret i32 0
	////	}
	////	这段代码回引发LVMM验证错误：
	////		Terminator found in the middle of a basic block!
	////		label %then
	////	因为未找到简单的解决方式，所以想把return的生成代码改为一个函数只有一个，当生成return时设置一个返回变量，然后goto到返回代码。最终结果类似：
	////	define i32 @ff() {
	////		br i1 true, label %then, label %else
	////	then:                                             ; preds = %0
	////		%RETURN_VAL=1;
	////		br label %RETURN
	////	else:                                             ; preds = %0
	////		br label %endd
	////	endd:                                             ; preds = %else, %then
	////		RETURN_VAL=0;
	////		br label %RETURN
	////	RETURN:
	////		ret i32 %RETURN_VAL
	////	}
	//llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	////查找RETURN标签
	////llvm::BasicBlock* bb_return=NULL;
	//for (llvm::BasicBlock &bb : *func)
	//	if (bb.getName() == "__co__RETURN")
	//	{
	//		//bb_return = &bb;
	//		if (value) //判断是否需要返回值
	//		{
	//			VAR_INFO vinfo = ir_var("__co__RETURN_VAL", ir_varlist, this->token);
	//			//vinfo.value = value->codegen();
	//			ir_builder->CreateStore(value->codegen(), vinfo.value);
	//		}
	//		ir_builder->CreateBr(&bb);
	//		return NULL;
	//	}

	//ErrorExit("ERROR: 未找到 RETURN 标签", this->token);
			

	//llvm::BasicBlock* bbexpr = llvm::BasicBlock::Create(ir_context, "return over", func);
	//ir_builder->SetInsertPoint(bbexpr);

	return NULL;
}

//	THE END