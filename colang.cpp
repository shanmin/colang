//
// colang
//

#include "colang.h"

#pragma comment(lib,"ws2_32.lib")

SRCINFO loadfile(const char* filename)
{
	SRCINFO srcinfo;
	//读取源文件内容
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL)
	{
		printf("ERROR: input file open fail.");
		return srcinfo;
	}
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	srcinfo.filename = filename;
	srcinfo.src = (char*)malloc(size + 1);
	if (srcinfo.src == NULL)
	{
		printf("ERROR: out of memory");
		return srcinfo;
	}
	fseek(fp, 0, SEEK_SET);
	fread(srcinfo.src, 1, size, fp);
	srcinfo.src[size] = 0;
	fclose(fp);

	return srcinfo;
}

//解析一个指定的co文件到bc格式
void co2bc(const char* filename)
{
	SRCINFO srcinfo=loadfile(filename);

	//词法分析
	std::vector<TOKEN> tokens;
	lexer(tokens, srcinfo);

	//预处理
	lexer_prepare(tokens);

	printf("---------- Lexer ----------\n");
	token_echo(tokens, "");

	//语法分析
	printf("\n---------- AST ----------\n");
	std::vector<AST*> ast_list = ast(tokens);
	ast_echo(ast_list, "");

	//IR
	printf("\n---------- IR ----------\n");
	ir(ast_list, filename);
}

int main(int argc, char** argv)
{
	//参数处理
	if (argc == 1)
	{
		printf("ERROR: no input file\n");
		return 1;
	}

	co2bc(argv[1]);
	return 0;
}

//	THE END