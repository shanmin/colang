//
// CmLang
//

#include <cstdio>
#include <vector>
#include "colang.h"
#include <malloc.h>
#include "ast.h"
#include "ir.h"
#include "lexer.h"

#pragma comment(lib,"ws2_32.lib")

//解析一个指定的co文件到bc格式
void co2bc(const char* filename)
{
	SRCINFO srcinfo;
	//读取源文件内容
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL)
	{
		printf("ERROR: input file open fail.");
		return;
	}
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	srcinfo.filename = filename;
	srcinfo.src = (char*)malloc(size + 1);
	if (srcinfo.src == NULL)
	{
		printf("ERROR: out of memory");
		return;
	}
	fseek(fp, 0, SEEK_SET);
	fread(srcinfo.src, 1, size, fp);
	srcinfo.src[size] = 0;
	fclose(fp);

	//词法分析
	std::vector<TOKEN> tokens;
	lexer(tokens, srcinfo);
	printf("---------- Lexer ----------\n");
    token_echo(tokens,"");

	//语法分析
	printf("\n---------- AST ----------\n");
	std::vector<AST*> ast_list = ast(tokens);
	ast_echo(ast_list, "");

	//IR
	printf("\n---------- IR ----------\n");
	ir(ast_list,filename);
	////IR_INFO ir_info;
	//////ir_info.context = llvm::LLVMContext();
	////ir_info.module =new llvm::Module(cmfilename, ir_info.context);
	////ir_info.builder = std::make_unique<llvm::IRBuilder<>>((ir_info.context));
	//////ir(ast_list, ir_info);
	//////验证模块是否存在问题
	////std::string mstr;
	////llvm::raw_string_ostream merr(mstr);
	////bool result=llvm::verifyModule(*ir_info.module,&merr);
	////if (result)
	////{
	////	printf("模块存在错误！%s",mstr.c_str());
	////	exit(2);
	////}
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main(int argc, char** argv)
{
    //参数处理
	if (argc == 1)
	{
		printf("ERROR: no input file\n");
		return 1;
	}
 
    co2bc(argv[1]);

    //// Prime the first token.
    //fprintf(stderr, "ready> ");
    //getNextToken();

    //// Make the module, which holds all the code.
    //InitializeModule();

    //// Run the main "interpreter loop" now.
    //MainLoop();

    //// Print out all of the generated code.
    //TheModule->print(errs(), nullptr);

    return 0;
}


//	THE END