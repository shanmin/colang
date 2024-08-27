//
//	ast.h
//
#pragma once

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include "llvm/IR/Verifier.h"


struct VAR_INFO
{
	llvm::Type* type; //变量类型
	//llvm::AllocaInst* value;	//变量实例
	llvm::Value* value;
	bool un; //是否为unsigned类型
};
struct VAR_LIST
{
	//变量范围
	//	global		全局
	//	function	函数
	//	codeblock	局部
	std::string zone;
	//变量列表
	std::map<std::string, VAR_INFO> info; //name,type
};

struct LABEL_LIST
{
	std::map<std::string, llvm::BasicBlock*> info;
};


extern std::unique_ptr<llvm::IRBuilder<>> ir_builder;
extern llvm::Module* ir_module;
extern llvm::LLVMContext ir_context;

extern std::vector<VAR_LIST> ir_varlist; //局部变量范围
extern std::vector<LABEL_LIST> ir_labellist; //局部标签


////////////////////////////////////////////////////////////////////////////////
//
//	AST
//
////////////////////////////////////////////////////////////////////////////////

class AST
{
public:
	virtual llvm::Value* codegen() = 0;			//生成代码
	virtual void show(std::string pre) = 0;		//显示代码
};

class AST_call :public AST
{
	TOKEN name;
	std::vector<AST*> args;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_call(std::vector<TOKEN>& tokens);
};

class AST_codeblock :public AST
{
	std::vector<AST*> body;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_codeblock(std::vector<TOKEN>& tokens);
};

class AST_do :public AST
{
	TOKEN name;
	AST* expr = NULL;
	AST* body = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_do(std::vector<TOKEN>& tokens);
};

class AST_expr :public AST
{
	TOKEN op;
	AST* left;
	AST* right;
public:
	//int op_pri;
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	//AST_expr();
	//AST_expr(std::vector<TOKEN>& tokens);
	AST_expr(AST* left, TOKEN op, AST* right) :left(left), op(op), right(right) {};
};

class AST_if :public AST
{
	TOKEN name;
	AST* expr1 = NULL;
	AST* thenbody = NULL;
	AST* elsebody = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_if(std::vector<TOKEN>& tokens);
};

class AST_label :public AST
{
	TOKEN name;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_label(std::vector<TOKEN>& tokens);
};

class AST_for :public AST
{
	TOKEN name;
	AST* expr1 = NULL;
	AST* expr2 = NULL;
	AST* expr3 = NULL;
	AST* body = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_for(std::vector<TOKEN>& tokens);
};

class AST_function :public AST
{
	std::vector<TOKEN> rett;
	TOKEN name;
	std::vector<TOKEN> args;
	std::vector<AST*> body;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_function(std::vector<TOKEN>& tokens);
};

class AST_goto :public AST
{
	TOKEN name;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_goto(std::vector<TOKEN>& tokens);
};

class AST_noncode :public AST
{
	TOKEN value;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_noncode(std::vector<TOKEN>& tokens);
};

class AST_return :public AST
{
	TOKEN token;
	AST* value = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_return(std::vector<TOKEN>& tokens);
};

class AST_value :public AST
{
	llvm::Value* _value; //解析后的常量
public:
	TOKEN value;
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_value(TOKEN token) :value(token) {}
	AST_value(std::vector<TOKEN>& tokens);
};

class AST_var :public AST
{
	std::vector<TOKEN> type;
	TOKEN name;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_var(std::vector<TOKEN>& tokens);
};

class AST_while :public AST
{
	TOKEN name;
	AST* expr = NULL;
	AST* body = NULL;
public:
	llvm::Value* codegen() override;
	void show(std::string pre) override;
	AST_while(std::vector<TOKEN>& tokens);
};


////////////////////////////////////////////////////////////////////////////////
//
//	OTHER
//
////////////////////////////////////////////////////////////////////////////////

std::vector<AST*> ast(std::vector<TOKEN>& tokens);
AST* ast1(std::vector<TOKEN>& tokens);
AST* ast_parse_expr(std::vector<TOKEN>& tokens, int left_pri = 0, AST* left = NULL);
AST* ast_parse_expr1(std::vector<TOKEN>& tokens);
AST* ast_parse_expr_add1(AST* old, std::vector<TOKEN>& tokens);

void ast_echo(std::vector<AST*> ast_list, std::string pre);

llvm::Type* ir_type(std::vector<TOKEN>& tokens);
llvm::Value* ir_type_conver(llvm::Value* value, llvm::Type* to);

void ir(std::vector<AST*>& ast_list, const char* filename);
VAR_INFO ir_var(std::string name, std::vector<VAR_LIST> var_list, TOKEN token);
llvm::Value* ir_var_load(VAR_INFO& var_info);



//	THE END