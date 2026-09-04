////////////////////////////////////////////////////////////////////////////////
//
// AST_delete
//   delete 指针变量; → 先调析构 ClassName.__dtor（若定义），再 free(指针)
//   常见：Counter *p = new Counter(1); ...; delete p;
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

AST_delete::AST_delete(std::vector<TOKEN>& tokens)
{
	// 吃掉 "delete"
	if (!tokens.empty() && tokens[0].Value == "delete")
		tokens.erase(tokens.begin());
	// 若紧接着是一个 code token（变量名），保存下来：codegen 阶段用来查 VARINFO.pointee_st 反推类名
	if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::code)
		var_name_tok = tokens[0];
	// 解析表达式（ast_parse_expr 会自己处理到语句结尾 ; 前）
	expr = ast_parse_expr(tokens);
}

void AST_delete::show(std::string pre)
{
	std::cout << pre << "#TYPE:delete" << std::endl;
	if (expr) {
		std::cout << pre << "  expr:";
		expr->show(pre + "      ");
	}
	std::cout << std::endl;
}

llvm::Value* AST_delete::codegen()
{
	if (!expr) return nullptr;
	llvm::Value* ptr = expr->codegen();
	if (!ptr) return nullptr;
	if (!ir_module) return nullptr;

	// --- Step 1：查析构函数（需要反推出类名）---
	//   变量名 → scope::get(var) → VARINFO.pointee_st → StructType::getName() = ClassName
	llvm::StructType* pointee_st = nullptr;
	if (!var_name_tok.Value.empty() && scope::has_var_any_scope(var_name_tok.Value)) {
		VARINFO vi = scope::get(var_name_tok); // 注意：若变量是占位（value=null）在 get 内部会继续向上层搜，可能 return 另一个同名变量；delete 通常在当前函数内，少见占位。
		pointee_st = vi.pointee_st;
	}

	if (pointee_st) {
		std::string class_name = pointee_st->getName().str();
		if (!class_name.empty()) {
			std::string dtor_name = class_name + ".__dtor";
			llvm::Function* dtor = ir_module->getFunction(dtor_name);
			// 析构无参，只取 this（1 个形参）
			if (dtor && dtor->arg_size() == 1) {
				std::vector<llvm::Value*> call_args = { ptr };
				(void)ir_builder->CreateCall(dtor, call_args);
			}
		}
	}

	// --- Step 2：free(ptr) ---
	llvm::Function* free_fn = ir_module->getFunction("free");
	if (!free_fn) {
		// 兜底：声明 extern "void free(void*)"
		llvm::Type* void_ty = llvm::Type::getVoidTy(ir_context);
		llvm::Type* i8p = ir_builder->getPtrTy();
		auto* FT = llvm::FunctionType::get(void_ty, { i8p }, false);
		free_fn = llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage, "free", ir_module);
	}
	if (free_fn) {
		std::vector<llvm::Value*> free_args = { ptr };
		(void)ir_builder->CreateCall(free_fn, free_args);
	}

	return nullptr;
}

//	THE END
