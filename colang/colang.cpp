//
// colang
//
#include "colang.h"

#pragma comment(lib,"ws2_32.lib")


void ErrorExit(const char* str, TOKEN token)
{
	printf("\n---------- Error ----------\n%s\n\t  row: %d\n\t  col: %d\n\t type: %d\n\ttoken: %s\n",
		str, token.row_index + 1, token.col_index, token.type, token.Value.c_str());
	exit(1);
}
void ErrorExit(const char* str, std::vector<TOKEN>& tokens)
{
	if (tokens.size() == 0)
		printf("\n---------- Error ----------\n%s\n", str);
	else
	{
		ErrorExit(str, tokens[0]);
	}
	exit(1);
}


//解析一个指定的co文件到bc格式
void co2bc(const char* filename)
{
	SRCINFO srcinfo = loadsrc(filename);

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