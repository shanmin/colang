//
//	lexer.cpp
//

#include <vector>
#include "lexer.h"

void ErrorExit(const char* str, std::vector<TOKEN>& tokens)
{
	//printf(str);
	if (tokens.size() == 0)
		printf("\n---------- Error ----------\n%s\n", str);
	else
		printf("\n---------- Error ----------\n%s\n\t  row: %d\n\t  col: %d\n\t type: %d\n\ttoken: %s\n",
			str, tokens[0].row_index+1, tokens[0].col_index, tokens[0].type, tokens[0].Value.c_str());
	exit(1);
}
void ErrorExit(const char* str, TOKEN token)
{
	printf("\n---------- Error ----------\n%s\n\t  row: %d\n\t  col: %d\n\t type: %d\n\ttoken: %s\n",
			str, token.row_index+1, token.col_index, token.type, token.Value.c_str());
	exit(1);
}

void token_echo(TOKEN token, std::string pre)
{
	std::vector<std::string> TOKEN_TYPE_STRING =
	{
		"noncode",
		"   code",
		" opcode",
		" string",
		" number",
	};

	if (token.filename == "")
	{
		std::cout << std::endl;
		return;
	}
	printf(pre.c_str());
	printf("[r:%3d,c:%3d](%d:%s) : %s\n", token.row_index + 1, token.col_index, token.type, TOKEN_TYPE_STRING[token.type].c_str(), token.Value.c_str());
}

void token_echo(std::vector<TOKEN> tokens, std::string pre)
{
	bool first = true;
	for (TOKEN token : tokens)
		if (first)
		{
			token_echo(token, "");
			first = false;
		}
		else
			token_echo(token, pre);
}

//判断字符是否为单字符操作符
bool is_opcode1(char c)
{
	return  c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == ';';
}

//判断字符是否为操作符字符
bool is_opcode2(char c)
{
	return  c == '<' || c == '>' || c == '=' || c == ',' || c==':' ||
		c == '+' || c == '-' || c == '*' || c == '/' || c == '|' || c == '&' || c == '!';
}

//词法分析器
//	把源代码拆分为最小的token单元
void lexer(std::vector<TOKEN>& tokens, SRCINFO& srcinfo)
{
	
	char* src = srcinfo.src;
	std::string current;
	//读取源代码数据进行解析
	bool iscode = false; //标识是否进入代码区
	int begin_row_index = 0;//当前处理开始的行号
	int begin_col_index = 0;//当前处理开始的列号
	int row_index = 0; //当前处理的代码行号
	int col_index = 0; //当前处理的代码列号

	while (src[0] != 0)
	{
		if (iscode)
		{
			//跳过空白字符
			while (isspace(src[0]))
			{
				if (src[0] == '\n')
				{
					row_index++;
					col_index = 0;
				}
				else
					col_index++;
				src++;
			}

			//判断代码结束区域
			if (src[0] == '?' && src[1] == '>')
			{
				iscode = false;
				src += 2;
				col_index += 2;
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
				continue;
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
				token.filename = srcinfo.filename;
				token.type = TOKEN_TYPE::string;
				token.Value = current;
				token.row_index = begin_row_index;
				token.col_index = begin_col_index;
				tokens.push_back(token);

				current = "";
				continue;
			}

			//运算操作符
			if (current.empty())
			{
				while (is_opcode1(src[0]))
				{
					current += src[0];
					src++;
					col_index++;
					break;
				}
				if (current.empty())
					while (is_opcode2(src[0]))
					{
						current += src[0];
						src++;
						col_index++;
					}
				//运算符
				if (!current.empty())
				{
					TOKEN token;
					token.filename = srcinfo.filename;
					token.row_index = row_index;
					token.col_index = col_index;
					token.type = TOKEN_TYPE::opcode;
					token.Value = current;
					tokens.push_back(token);

					current = "";
					continue;
				}
			}

			//代码
			while (src[0])
			{
				if (is_opcode1(src[0]) || is_opcode2(src[0]) ||
					src[0] == ' ' ||
					src[0] == '|' || src[0] == '&' || src[0] == '%' || src[0] == '?' ||
					src[0] == ',' || src[0] == ':' || src[0] == 0 ||
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
			}
			if (!current.empty())
			{
				TOKEN token;
				token.filename = srcinfo.filename;
				token.row_index = begin_row_index;
				token.col_index = begin_col_index;
				if (isdigit(current[0]))
					token.type = TOKEN_TYPE::number;
				else
					token.type = TOKEN_TYPE::code;
				token.Value = current;
				tokens.push_back(token);

				current = "";
			}
		}
		else
		{
			//读取非代码区内容
			while (src[0] != 0)
			{
				//判断是否进入代码区
				if (src[0] == '<' && src[1] == '?')
				{
					src += 2;
					col_index += 2;
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
				token.filename = srcinfo.filename;
				token.row_index = begin_row_index;
				token.col_index = begin_col_index;
				token.type = TOKEN_TYPE::noncode;
				token.Value = current;
				tokens.push_back(token);
			}
			current = "";
			iscode = true;	//进入代码区
		}
	}//while (src[0] != 0)
}

//	THE END