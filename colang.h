// cmlang.h

#pragma once

#include <iostream>

struct SRCINFO
{
	char* src;	//源代码内容
	unsigned int size = 0;	//当前待处理文件长度
};

#include "lexer.h"

void ErrorExit(const char* str,std::vector<TOKEN>& tokens)
{
	//printf(str);
	if(tokens.size()==0)
		printf("\n---------- Error ----------\n%s\n", str);
	else
		printf("\n---------- Error ----------\n%s\n\t  row: %d\n\t  col: %d\n\t type: %d\n\ttoken: %s\n",
			str, tokens[0].row_index, tokens[0].col_index, tokens[0].type, tokens[0].Value.c_str());
	exit(1);
}

//	THE END