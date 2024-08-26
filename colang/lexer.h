//
//	lexer	词法分析
//
#pragma once

//源代码文件信息
struct SRCINFO
{
	std::string filename;
	char* src = NULL;	//源代码内容
};

enum TOKEN_TYPE
{
	noncode,	//非代码
	code,		//代码
	opcode,		//操作符，例如+-()等操作符号
	string,		//字符串
	number,		//数字
};

struct TOKEN
{
	std::string filename;	//所属文件
	int row_index;			//所在行
	int col_index;			//所在列
	TOKEN_TYPE type;		//类型
	std::string Value;		//内容
};

SRCINFO loadsrc(const char* filename);
void lexer(std::vector<TOKEN>& tokens, SRCINFO& srcinfo);
bool lexer_prepare(std::vector<TOKEN>& tokens);
void token_echo(TOKEN token, std::string pre);
void token_echo(std::vector<TOKEN> tokens, std::string pre);

//	THE END