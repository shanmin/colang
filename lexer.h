//
//	lexer.h
//
#pragma once

#include <string>
#include <vector>

struct SRCINFO
{
	std::string filename;
	char* src;	//源代码内容
};

enum TOKEN_TYPE
{
	noncode,	//非代码
	code,		//代码
	opcode,		//操作符
	string,		//字符串
	number,		//数字
};

struct TOKEN
{
	std::string filename;
	int row_index;	//所在行
	int col_index;	//所在列
	TOKEN_TYPE type;
	std::string Value;
};

void ErrorExit(const char* str, std::vector<TOKEN>& tokens);
void ErrorExit(const char* str, TOKEN token);

void lexer(std::vector<TOKEN>& tokens, SRCINFO& srcinfo);
bool lexer_prepare(std::vector<TOKEN>& tokens);
void token_echo(TOKEN token, std::string pre);
void token_echo(std::vector<TOKEN> tokens, std::string pre);

//THE END