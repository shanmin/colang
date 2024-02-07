// cmlang.h

#pragma once

#include <iostream>

struct SRCINFO
{
	char* src;	//源代码内容
	unsigned int size = 0;	//当前待处理文件长度
};

#include "lexer.h"

void ErrorExit(const char* str, TOKEN token)
{
	printf(str);
	printf("\n---------- Error ----------\n\t  row: %d\n\t  col: %d\n\t type: %d\n\ttoken: %s\n", token.row_index, token.col_index, token.type, token.Value.c_str());
	exit(1);
}

//	THE END