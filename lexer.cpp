//
//	lexer	词法分析
//

#include <iostream>
#include "colang.h"

void ErrorExit(const char* str, TOKEN token)
{
	printf("\n---------- Error ----------\n%s\n\t  row: %d\n\t  col: %d\n\t type: %d\n\ttoken: %s\n",
		str, token.row_index + 1, token.col_index, token.type, token.Value.c_str());
	exit(1);
}
void ErrorExit(const char* str, std::vector<TOKEN>& tokens)
{
	//printf(str);
	if (tokens.size() == 0)
		printf("\n---------- Error ----------\n%s\n", str);
	else
	{
		//printf("\n---------- Error ----------\n%s\n\t  row: %d\n\t  col: %d\n\t type: %d\n\ttoken: %s\n",
		//	str, tokens[0].row_index + 1, tokens[0].col_index, tokens[0].type, tokens[0].Value.c_str());
		////printf("CODE:\n");
		////if(tokens[0].row_index>0)
		////	printf(tokens[0].)
		ErrorExit(str, tokens[0]);
	}
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
	std::string showstr;
	for (char c : token.Value)
	{
		//对特殊字符进行转换，好显示
		switch (c)
		{
		case '\r':showstr += "\\r"; break;
		case '\n':showstr += "\\n"; break;
		case '\t':showstr += "\\t"; break;
		default:showstr += c; break;
		}
	}
	printf("\033[1m[%s,r:%3d,c:%3d](%d:%s) :\033[0m %s\n",token.filename.c_str(), token.row_index + 1, token.col_index, token.type, TOKEN_TYPE_STRING[token.type].c_str(), showstr.c_str());
}
//输出指定TOKEN信息
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
	return  c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c==',' || c == ';';
}

//判断字符是否为多字符操作符
bool is_opcode2(char c)
{
	return  c == '<' || c == '>' || c == '=' || c == ':' ||
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
	int current_row_index = 0; //当前处理的代码行号
	int current_col_index = 0; //当前处理的代码列号

	while (src[0] != 0)
	{
		if (iscode)
		{
			//跳过空白字符
			while (isspace(src[0]))
			{
				if (src[0] == '\n')
				{
					current_row_index++;
					current_col_index = 0;
				}
				else
					current_col_index++;
				src++;
			}

			//判断代码结束区域
			if (src[0] == '?' && src[1] == '>')
			{
				iscode = false;
				src += 2;
				current_col_index += 2;
				continue;
			}

			//跳过单行注释
			if (src[0] == '/' && src[1] == '/')
			{
				src += 2;
				current_col_index += 2;
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
					current_col_index++;
				}
				continue;
			}

			//跳过注释块
			if (src[0] == '/' && src[1] == '*')
			{
				src += 2;
				current_col_index += 2;
				while (src[0])
				{
					if (src[0] == '*' && src[1] == '/')
					{
						src += 2;
						current_col_index += 2;
						break;
					}
					else if (src[0] == '\n')
					{
						src++;
						current_row_index++;
						current_col_index = 0;
					}
					else
					{
						src++;
						current_col_index++;
					}
				}
				continue;
			}

			//判断字符串读取
			if (src[0] == '"')
			{
				begin_row_index = current_row_index;
				begin_col_index = current_col_index;
				src++;
				current_col_index++;
				while (src[0] != 0)
					if (src[0] == '\\')
					{
						if (src[1] == '"')		current += "\"";
						else if (src[1] == '\\')	current += "\\";
						else if (src[1] == 'b')	current += "\b";	//backspace
						else if (src[1] == 'f')	current += "\f";	//formfeed
						else if (src[1] == 'n')	current += "\n";	//linefeed
						else if (src[1] == 'r')	current += "\r";	//carriage return
						else if (src[1] == 't')	current += "\t";	//horizontal tab
						else
						{
							current += src[0];
							current += src[1];
						}
						src += 2;
						current_col_index += 2;
						continue;
					}
					else if (src[0] == '"') //字符串结束
					{
						src++;
						current_col_index++;
						break;
					}
					else
					{
						current += src[0];
						src++;
						if (src[0] == '\n')
						{
							current_row_index++;
							current_col_index = 0;
						}
						else
							current_col_index++;
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
					current_col_index++;
					break;
				}
				if (current.empty())
					while (is_opcode2(src[0]))
					{
						current += src[0];
						src++;
						current_col_index++;
					}
				//运算符
				if (!current.empty())
				{
					TOKEN token;
					token.filename = srcinfo.filename;
					token.row_index = current_row_index;
					token.col_index = current_col_index;
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
						begin_row_index = current_row_index;
						begin_col_index = current_col_index;
					}
					current += src[0];
					src++;
					current_col_index++;
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
					current_col_index += 2;
					break;
				}
				else
				{
					if (current.empty())
					{
						begin_row_index = current_row_index;
						begin_col_index = current_col_index;
					}
					current += src[0];
					if (src[0] == '\n')
					{
						current_row_index++;
						current_col_index = 0;
					}
					else
						current_col_index++;
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

//词法分析预处理
bool lexer_prepare(std::vector<TOKEN>& tokens)
{
	for (int i = 0; i < tokens.size() - 1; i++)
	{
		if (tokens[i].type == TOKEN_TYPE::code && tokens[i].Value == "#include" && tokens[i + 1].type == TOKEN_TYPE::string)
		{
			//读取include文件
			std::vector<TOKEN> include_tokens;
			SRCINFO srcinfo = loadfile(tokens[i+1].Value.c_str());
			if (srcinfo.filename.empty())
			{
				ErrorExit("#include文件加载错误", tokens[i+1]);
			}
			lexer(include_tokens, srcinfo);
			//重组TOKENs
			std::vector<TOKEN> new_tokens;
			for (int j = 0; j < i; j++)
				new_tokens.push_back(tokens[j]);
			for(int j=0;j<include_tokens.size();j++)
				new_tokens.push_back(include_tokens[j]);
			for(int j=i+2;j<tokens.size();j++)
				new_tokens.push_back(tokens[j]);
			tokens = new_tokens;
			return true;
		}
	}
	return false;
}


//	THE END