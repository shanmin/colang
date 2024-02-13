//
//	Lexer
//
#pragma once

#include <string>
#include <vector>
#include "colang.h"

enum TOKEN_TYPE
{
	noncode,	//非代码
	preproc,	//预处理代码，以#开头的指令
	block,	//关键字
	string,		//字符串
	number,		//数字
	paren_open,		//左括号(
	paren_close,	//右括号)
	bracket_open,	//左中括号[
	bracket_close,	//右中括号]
	curly_open,		//左大括号{
	curly_close,	//右大括号}
	semicolon,	//分号;
	comma,		//逗号,
	colon,		//冒号:
	equal,		//等号=
	define,		//常量预处理
};

struct TOKEN
{
	TOKEN_TYPE type;
	std::string filename;
	std::string Value;
	int row_index;	//所在行
	int col_index;	//所在列
};


void token_echo(std::vector<TOKEN> tokens, std::string pre)
{
	for (TOKEN token : tokens)
	{
		printf(pre.c_str());
		printf("[r:%3d,c:%3d]%2d : %s\n", token.row_index+1, token.col_index, token.type, token.Value.c_str());
	}
}
void token_echo(TOKEN token, std::string pre)
{
		printf(pre.c_str());
		printf("[r:%3d,c:%3d]%2d : %s\n", token.row_index+1, token.col_index, token.type, token.Value.c_str());
}

//词法分析器
//	把源代码拆分为最小的token单元
void lexer(std::vector<TOKEN>& tokens, SRCINFO& srcinfo)
{
	char* src = srcinfo.src;
	std::string current;
	//读取源代码数据进行解析
	bool iscode = false; //标识是否进入代码区
	int row_index = 0; //当前处理的代码行号
	int col_index = 0; //当前处理的代码列号
	int begin_row_index = 0;//当前处理开始的行号
	int begin_col_index = 0;//当前处理开始的列号

	while (src[0] != 0)
	{
		if (iscode)
		{
			//跳过空白字符
			while (isspace(src[0]))
			{
				src++;
				if (src[0] == '\n')
				{
					row_index++;
					col_index = 0;
				}
				else
					col_index++;
			}

			//判断代码结束区域
			if (src[0] == '?' && src[1] == '>')
			{
				iscode = false;
				src += 2;
				col_index+=2;
				continue;
			}

			//跳过单行注释
			if (src[0] == '/' && src[1] == '/')
			{
				src += 2;
				col_index += 2;
				while (src[0])
				{
					if (src[0] == '?' && src[1] == '>')
					{
						break;
					}
					else if (src[0] == '\n')
					{
						break;
					}
					src++;
					col_index++;
				}
				continue;
			}

			//跳过注释块
			if (src[0] == '/' && src[1] == '*')
			{
				src += 2;
				col_index += 2;
				while (src[0])
				{
					if (src[0] == '*' && src[1] == '/')
					{
						src += 2;
						col_index += 2;
						break;
					}
					else if (src[0] == '\n')
					{
						src++;
						row_index++;
						col_index = 0;
					}
					else
					{
						src++;
						col_index++;
					}
				}
			}

			//判断字符串读取
			if (src[0] == '"')
			{
				begin_row_index = row_index;
				begin_col_index = col_index;
				src++;
				col_index++;
				while (src[0] != 0)
					if (src[0] == '\\')
					{
						if (src[1] == '"')		current += "\"";
						else if (src[1] == 'a')	current += "\a";
						else if (src[1] == 'b')	current += "\b";
						else if (src[1] == 'f')	current += "\f";
						else if (src[1] == 't')	current += "\t";
						else if (src[1] == 'v')	current += "\v";
						else if (src[1] == 'r')	current += "\r";
						else if (src[1] == 'n')	current += "\n";
						else
						{
							current += src[0];
							current += src[1];
						}
						src += 2;
						col_index += 2;
						continue;
					}
					else if (src[0] == '"') //字符串结束
					{
						src++;
						col_index++;
						break;
					}
					else
					{
						current += src[0];
						src++;
						if (src[0] == '\n')
						{
							row_index++;
							col_index = 0;
						}
						else
							col_index++;
					}
				TOKEN token;
				token.filename = srcinfo.src;
				token.type = TOKEN_TYPE::string;
				token.Value = current;
				token.row_index = begin_row_index;
				token.col_index = begin_col_index;
				tokens.push_back(token);

				current = "";
				continue;
			}

			//运算符
			if (current.empty())
			{
				//双运算符
				if (src[0] == '=' && src[1] == '=') current = "==";
				else if (src[0] == '>' && src[1] == '=') current = ">=";
				else if (src[0] == '=' && src[1] == '>') current = ">=";
				else if (src[0] == '<' && src[1] == '=') current = "<=";
				else if (src[0] == '=' && src[1] == '<') current = "<=";
				else if (src[0] == '!' && src[1] == '=') current = "!=";
				else if (src[0] == '<' && src[1] == '>') current = "!=";
				else if (src[0] == '>' && src[1] == '<') current = "!=";
				else if (src[0] == '+' && src[1] == '=') current = "+=";
				else if (src[0] == '-' && src[1] == '=') current = "-=";
				else if (src[0] == '*' && src[1] == '=') current = "*=";
				else if (src[0] == '/' && src[1] == '=') current = "/=";
				else if (src[0] == '&' && src[1] == '=') current = "&=";
				else if (src[0] == '|' && src[1] == '=') current = "|=";
				else if (src[0] == '<' && src[1] == '<')
				{
					if (src[2] == '=')
						current = "<<=";
					else
						current = "<<";
				}
				else if (src[0] == '>' && src[1] == '>')
				{
					if (src[2] == '=')
						current = ">>=";
					else
						current = ">>";
				}
				if (!current.empty())
				{
					TOKEN token;
					token.filename = srcinfo.src;
					token.type = TOKEN_TYPE::block;
					token.Value = current;
					token.row_index = row_index;
					token.col_index = col_index;
					tokens.push_back(token);

					current = "";
					src += 2;
					col_index += 2;
					continue;
				}

				if (src[0] == '=' || src[0] == '!' ||
					src[0] == '+' || src[0] == '-' || src[0] == '*' || src[0] == '/' ||
					src[0] == '|' || src[0] == '&' || src[0] == '%' || src[0] == '?' ||
					src[0] == '<' || src[0] == '>' ||
					src[0] == '(' || src[0] == ')' ||
					src[0] == '[' || src[0] == ']' ||
					src[0] == '{' || src[0] == '}' ||
					src[0] == ',' || src[0] == ':' || src[0] == ';'
					)
				{
					current = src[0];

					TOKEN token;
					token.filename = srcinfo.src;
					if (src[0] == '(')
						token.type = TOKEN_TYPE::paren_open;
					else if (src[0] == ')')
						token.type = TOKEN_TYPE::paren_close;
					else if (src[0] == '[')
						token.type = TOKEN_TYPE::bracket_open;
					else if (src[0] == ']')
						token.type = TOKEN_TYPE::bracket_close;
					else if (src[0] == '{')
						token.type = TOKEN_TYPE::curly_open;
					else if (src[0] == '}')
						token.type = TOKEN_TYPE::curly_close;
					else if (src[0] == ';')
						token.type = TOKEN_TYPE::semicolon;
					else if (src[0] == ',')
						token.type = TOKEN_TYPE::comma;
					else if (src[0] == ':')
						token.type = TOKEN_TYPE::colon;
					else if (src[0] == '=')
						token.type = TOKEN_TYPE::equal;
					else
						token.type = TOKEN_TYPE::block;
					token.Value = current;
					token.row_index = row_index;
					token.col_index = col_index;
					tokens.push_back(token);

					current = "";
					src++;
					col_index++;
					continue;
				}
			}

			//关键字
			while (src[0])
				if (src[0] == ' ' || src[0] == '=' ||
					src[0] == '+' || src[0] == '-' || src[0] == '*' || src[0] == '/' ||
					src[0] == '|' || src[0] == '&' || src[0] == '%' || src[0] == '?' ||
					src[0] == '<' || src[0] == '>' ||
					src[0] == '(' || src[0] == ')' ||
					src[0] == '[' || src[0] == ']' ||
					src[0] == '{' || src[0] == '}' ||
					src[0] == ',' || src[0] == ':' || src[0] == ';' || src[0] == 0 ||
					src[0] == '\t' || src[0] == '\r' || src[0] == '\n' || src[0] == '\\'
					)
				{
					break;
				}
				else
				{
					if (current.empty())
					{
						begin_row_index = row_index;
						begin_col_index = col_index;
					}
					current += src[0];
					src++;
					col_index++;
				}
			if (!current.empty())
			{
				TOKEN token;
				token.filename = srcinfo.src;
				if (isdigit(current[0]))
					token.type = TOKEN_TYPE::number;
				else if (token.Value.starts_with("#"))
					token.type = TOKEN_TYPE::preproc;
				else
					token.type = TOKEN_TYPE::block;
				token.Value = current;
				token.row_index = begin_row_index;
				token.col_index = begin_col_index;
				tokens.push_back(token);

				current = "";
			}
		}
		else
		{
			while (src[0] != 0)
			{
				//判断是否进入代码区
				if (src[0] == '<' && src[1] == '?')
				{
					src += 2;
					col_index+=2;
					break;
				}
				else
				{
					if (current.empty())
					{
						begin_row_index = row_index;
						begin_col_index = col_index;
					}
					current += src[0];
					if (src[0] == '\n')
					{
						row_index++;
						col_index = 0;
					}
					else
						col_index++;
					src++;
				}
			}
			//如果非代码区为空，则不需进行处理
			if (!current.empty())
			{
				TOKEN token;
				token.filename = srcinfo.src;
				token.type = TOKEN_TYPE::noncode;
				token.Value = current;
				token.row_index = begin_row_index;
				token.col_index = begin_col_index;
				tokens.push_back(token);
			}
			current = "";
			iscode = true;	//进入代码区
		}
	}
}

//	THE END