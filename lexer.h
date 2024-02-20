//
//	Lexer
//
#pragma once

#include <vector>
#include <string>
#include <iostream>

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

void token_echo(TOKEN token, std::string pre);
void token_echo(std::vector<TOKEN> tokens, std::string pre);

//判断字符是否为单字符操作符
bool is_opcode1(char c);
//判断字符是否为操作符字符
bool is_opcode2(char c);

//词法分析器
//	把源代码拆分为最小的token单元
void lexer(std::vector<TOKEN>& tokens, SRCINFO& srcinfo);

//	THE END