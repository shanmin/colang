//
//	lexer	词法分析
//
#include "colang.h"

//加载源代码文件
//  语义：成功返回填充后的 SRCINFO；任何失败（打开/读/内存/空文件占位）一律走 ErrorExit 直接终止，绝不把 {nullptr src / empty filename} 的残缺 SRCINFO 丢给上游
//  （上游 lexer/lexer_prepare/co2m/diag 都不会有空指针路径，从而消除 colang.exe 0xC0000005 崩溃）
SRCINFO loadsrc(const char* filename)
{
	SRCINFO srcinfo;
	if (!filename || !*filename) {
		fprintf(stdout, "\n---------- Error ----------\nERROR: empty input filename\n");
		exit(2);
	}
	//读取源文件内容
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL)
	{
		// 这里 filename 往往来自 argv 或 #include 的字符串 token，没有行号信息 → 用最简非 token 错误（与 ErrorExit(vector&) 空容器分支同格式）
		fprintf(stdout, "\n---------- Error ----------\nERROR: cannot open input file: %s\n", filename);
		exit(2);
	}
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	if (size < 0) {
		fclose(fp);
		fprintf(stdout, "\n---------- Error ----------\nERROR: failed to query input file size: %s\n", filename);
		exit(2);
	}
	srcinfo.filename = filename;
	srcinfo.src = (char*)malloc((size_t)size + 1);
	if (srcinfo.src == NULL)
	{
		fclose(fp);
		fprintf(stdout, "\n---------- Error ----------\nERROR: out of memory loading %s (size=%ld)\n", filename, size);
		exit(2);
	}
	fseek(fp, 0, SEEK_SET);
	size_t nread = fread(srcinfo.src, 1, (size_t)size, fp);
	if (nread != (size_t)size) {
		fclose(fp);
		free(srcinfo.src);
		srcinfo.src = NULL;
		fprintf(stdout, "\n---------- Error ----------\nERROR: short read for %s (%zu/%ld bytes)\n", filename, nread, size);
		exit(2);
	}
	srcinfo.src[size] = 0;
	fclose(fp);

	//跳过 UTF-8 BOM（EF BB BF），避免 BOM 字节混入首个模板文本（noncode）输出到页面头部
	if (size >= 3
		&& (unsigned char)srcinfo.src[0] == 0xEF
		&& (unsigned char)srcinfo.src[1] == 0xBB
		&& (unsigned char)srcinfo.src[2] == 0xBF)
	{
		//整体前移 3 字节覆盖 BOM（含结尾 0），后续处理与无 BOM 文件完全一致
		memmove(srcinfo.src, srcinfo.src + 3, size - 3 + 1);
	}

	return srcinfo;
}


//判断字符是否为 UTF-8 续字节（0x80-0xBF）
//	多字节字符只由首字节贡献列宽，保证列号按字符计数而非字节
bool is_utf8_continue(char c)
{
	return ((unsigned char)c & 0xC0) == 0x80;
}


//判断字符是否为单字符操作符
bool is_opcode1(char c)
{
	return  c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
			c == ',' || c == ';' || c == '.' || c == '~'; // ~ 位取反（一元前缀）
}


//判断字符是否为多字符操作符
bool is_opcode2(char c)
{
	return  c == '<' || c == '>' || c == '=' || c == ':' ||
		c == '+' || c == '-' || c == '*' || c == '/' || c == '|' || c == '&' || c == '!' || c == '%';
}


//词法分析器
//	把源代码拆分为最小的token单元
void lexer(std::vector<TOKEN>& tokens, SRCINFO& srcinfo)
{
	//如果文件未读出内容，则直接返回 20240814 shanmin
	if (srcinfo.src == NULL)
		return;

	char* src = srcinfo.src;

	bool iscode = false; //标识是否进入代码区
	bool has_code = false; //是否出现过代码区（用于区分“文件开头”与“?>之后”两种非代码区）
	std::string current;	//当前正在处理的标识
	int begin_row_index = 0;//当前处理开始的行号
	int begin_col_index = 0;//当前处理开始的列号
	int current_row_index = 0; //当前处理的代码行号
	int current_col_index = 0; //当前处理的代码列号

	//读取源代码数据进行解析
	while (src[0] != 0)
	{
		if (iscode)	//代码区处理
		{
			has_code = true;

			//跳过空白字符，这个判断包括：空格、\t、\n、\v、\f、\r字符
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
					//注意：注释内的 ?> 不是代码结束符，仍属于注释文本（与 C // 语义一致：直到 \n 才结束）。
					//	如果用户真要在该行结束代码区，?> 需单独写在非注释行上。
					//	之前曾错误地将注释中的 ?> 当结束符，导致 "// <?co / ?>" 之类的注释把
					//	后续注释尾部当 noncode 模板文本处理，其中 / 字符触发 opcode 识别并报错。
					if (src[0] == '\n') //退出当前注释，后面的由前面的空白字符处理代码进行处理
					{
						break;
					}
					if (!is_utf8_continue(src[0])) current_col_index++;
					src++;
				}
				continue;
			}

			//跳过注释块
			if (src[0] == '/' && src[1] == '*')
			{
				int start_row = current_row_index;
				int start_col = current_col_index;
				src += 2;
				current_col_index += 2;
				bool closed = false;
				while (src[0])
				{
					if (src[0] == '*' && src[1] == '/') //注释块结束
					{
						src += 2;
						current_col_index += 2;
						closed = true;
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
						if (!is_utf8_continue(src[0])) current_col_index++;
						src++;
					}
				}
				if (!closed)
				{
					TOKEN diag;
					diag.filename = srcinfo.filename;
					diag.row_index = start_row;
					diag.col_index = start_col;
					diag.type = TOKEN_TYPE::opcode;
					diag.Value = "/*";
					ErrorExit("unterminated block comment (missing '*/')", diag);
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
				bool closed = false;
				while (src[0] != 0)
					if (src[0] == '"') //字符串结束
					{
						src++;
						current_col_index++;
						closed = true;
						break;
					}
					else if (src[0] == '\\')
					{
						if (src[1] == 0)
						{
							TOKEN diag;
							diag.filename = srcinfo.filename;
							diag.row_index = current_row_index;
							diag.col_index = current_col_index;
							diag.type = TOKEN_TYPE::string;
							diag.Value = "\\";
							ErrorExit("unterminated string (end of file after '\\\\' escape)", diag);
						}
						if (src[1] == '"')		current += "\"";
						else if (src[1] == '\\')current += "\\";
						else if (src[1] == 'b')	current += "\b";	//backspace
						else if (src[1] == 'f')	current += "\f";	//formfeed
						else if (src[1] == 'n')	current += "\n";	//linefeed
						else if (src[1] == 'r')	current += "\r";	//carriage return
						else if (src[1] == 't')	current += "\t";	//horizontal tab
						else if (src[1] == 'v')	current += "\v";	//vertical tab
						else
						{
							current += src[0];
							current += src[1];
						}
						src += 2;
						current_col_index += 2;
						continue;
					}
					else
					{
						current += src[0];
						if (src[0] == '\n')
						{
							current_row_index++;
							current_col_index = 0;
						}
						else if (!is_utf8_continue(src[0]))
							current_col_index++;
						src++;
					}

				if (!closed)
				{
					TOKEN diag;
					diag.filename = srcinfo.filename;
					diag.row_index = begin_row_index;
					diag.col_index = begin_col_index;
					diag.type = TOKEN_TYPE::string;
					diag.Value = "\"";
					ErrorExit("unterminated string (missing closing '\"')", diag);
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
				// 可变参数省略号：三个连续 '.' 合并为单个 "..." token（函数变参原型，如 printf(char* fmt, ...)）
				//   必须先于浮点小数点判断（"..." 第二位是 '.' 不是数字，本不会触发小数点分支，放最前仅为清晰）
				if (src[0] == '.' && src[1] == '.' && src[2] == '.')
				{
					current += "...";
					src += 3;
					current_col_index += 3;
					break;
				}

				// 特殊：如果当前符号是 '.' 且紧跟数字字符 → 不是字段符，这是浮点数字面量的小数点（如 .25）
				//   跳过 opcode 分支 → 交给下面「读取标识符/数字」分支，isdigit 不成立，但后面拼接数字时会把 "." 并入 number 字符串。
				//   同时也要处理 "3.25" 的场景：数字分支先拼 "3" 遇到 "."，此处如果 current.empty()+下一位数字→仍归 number（但 current.empty() 场景 current="3" 已经在上面分支）。
				// 更稳妥处理：. is_opcode1 成立但下一位 isdigit → 当前不要把 . 当 opcode，跳出 while，交给通用"标识符/数字"分支时若 current 起始 '.' 后跟 digits 当数字。
				if (src[0] == '.' && isdigit((unsigned char)src[1]))
					goto NOT_OPCODE1;

				// 数组语法：识别 "[]" 作为数组类型标记
				if (src[0] == '[' && src[1] == ']')
					{
						current += src[0];
						src++;
						current_col_index++;
						current += src[0];
						src++;
						current_col_index++;
						break;
					}
					
					current += src[0];
					src++;
					current_col_index++;
					break;
				}
NOT_OPCODE1:
				if (current.empty() && is_opcode2(src[0]))
				{
					//只取 1 个 opcode2 字符，再判断下一个字符能否组成「合法的 2 字符运算符」。
					//旧实现 while(is_opcode2) 贪婪合并所有连续 opcode2 字符 →
					//  非法组合如 "*-"（2*-3）/ "-+" 会被合成单 token → AST 报"未识别的运算符"。
					//合法的 2 字符运算符白名单：
					//  比较 == != >= <=  逻辑 && ||  复合 += -= *= /= %=  自增自减 ++ --
					current += src[0];
					src++;
					current_col_index++;
					if (is_opcode2(src[0]))
					{
						char c2 = src[0];
						std::string two = current + std::string(1, c2);
						if (two == "==" || two == "!=" || two == ">=" || two == "<=" ||
							two == "&&" || two == "||" ||
							two == "+=" || two == "-=" || two == "*=" || two == "/=" || two == "%=" ||
							two == "++" || two == "--" ||
							two == "<<" || two == ">>")
						{
							current += c2;
							src++;
							current_col_index++;
						}
						//否则不合并（如 *- / -+ 等非法组合各自独立成 token）
					}
				}
				//运算符
				if (!current.empty())
				{
					TOKEN token;
					token.filename = srcinfo.filename;
					token.row_index = current_row_index;
					token.col_index = current_col_index;
					
					// 检查是否是数组语法 "[]"
					if (current == "[]")
					{
						token.type = TOKEN_TYPE::array;
					}
					else
					{
						token.type = TOKEN_TYPE::opcode;
					}
					
					token.Value = current;
					tokens.push_back(token);

					current = "";
					continue;
				}
			}

			//代码 / 数字
			//  读取规则：常规情况下遇到 opcode1/2 就停止；
			//  **特殊**：若当前 current 已有数字字符，且 src[0]=='.' 且下一位 src[1] 是数字 → 把 '.' 继续并入 current（组成浮点字面量），
			//  不会作为字段 opcode（字段 '.' 是标识符后紧跟 '.'，下一位不是数字）。
			while (src[0])
			{
				bool break_here = false;
				if (src[0] == '.' && !current.empty() && isdigit((unsigned char)current.back()) && isdigit((unsigned char)src[1]))
				{
					// 3.25 场景：current 已有数字数字，接下来 ".d" → 继续并入
					break_here = false;
				}
				else if (is_opcode1(src[0]) || is_opcode2(src[0]) ||
					src[0] == ' ' || src[0] == '%' || src[0] == '?' ||
					src[0] == ',' || src[0] == ':' || src[0] == 0 ||
					src[0] == '\t' || src[0] == '\r' || src[0] == '\n' || src[0] == '\\'
					)
				{
					break_here = true;
				}
				if (break_here) break;

				if (current.empty())
				{
					begin_row_index = current_row_index;
					begin_col_index = current_col_index;
				}
				current += src[0];
				if (!is_utf8_continue(src[0])) current_col_index++;
				src++;
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
			else //本轮没有消费任何字符：遇到了代码区无法识别的字符（如裸露的 ?、\），若不报错将死循环
			{
				TOKEN token;
				token.filename = srcinfo.filename;
				token.row_index = current_row_index;
				token.col_index = current_col_index;
				token.type = TOKEN_TYPE::opcode;
				token.Value = std::string(1, src[0]);
				ErrorExit("unrecognized character in code region", token);
			}
		}
		else //非代码区处理
		{
			//读取非代码区内容
			while (src[0] != 0)
			{
				//判断是否进入代码区，代码区开始标记为 <?co
				//co 后面不能是字母/数字/下划线，避免误识别 <?code、<?co2 之类的标签
				if (src[0] == '<' && src[1] == '?' && src[2] == 'c' && src[3] == 'o'
					&& !isalnum((unsigned char)src[4]) && src[4] != '_')
				{
					src += 4;
					current_col_index += 4;
					break;
				}
				else
				{
					if (current.empty()) //如果当前标识为空，则记录标识开始位置
					{
						begin_row_index = current_row_index;
						begin_col_index = current_col_index;
					}
					current += src[0];	//保存一个有效字符
					if (src[0] == '\n')
					{
						current_row_index++;
						current_col_index = 0;
					}
					else if (!is_utf8_continue(src[0]))
						current_col_index++;
					src++;
				}
			}
			//如果非代码区为空，则不需进行处理
			//	另外：?> 之后若直到文件结束全是空白（空格、换行等），也忽略，
			//	避免尾部空白生成多余的模板输出。文件开头的空白仍按模板文本输出
			if (!current.empty()
				&& !(has_code && src[0] == 0
					&& current.find_first_not_of(" \t\r\n\v\f") == std::string::npos))
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
//	主要处理#inclde命令
bool lexer_prepare(std::vector<TOKEN>& tokens)
{
	for (int i = 0; i < tokens.size() - 1; i++)
	{
		if (tokens[i].type == TOKEN_TYPE::code && tokens[i].Value == "#include" && tokens[i + 1].type == TOKEN_TYPE::string)
		{
			//读取include文件
			std::vector<TOKEN> include_tokens;
			SRCINFO srcinfo = loadsrc(tokens[i + 1].Value.c_str());
			if (srcinfo.filename.empty())
			{
				ErrorExit("#include file load error", tokens[i + 1]);
			}
			lexer(include_tokens, srcinfo);
			//重组TOKENs
			std::vector<TOKEN> new_tokens;
			for (int j = 0; j < i; j++)
				new_tokens.push_back(tokens[j]);
			for (int j = 0; j < include_tokens.size(); j++)
				new_tokens.push_back(include_tokens[j]);
			for (int j = i + 2; j < tokens.size(); j++)
				new_tokens.push_back(tokens[j]);
			tokens = new_tokens;
			return true;
		}
	}
	return false;
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
	//printf("\033[1m[%s,r:%3d,c:%3d](%d:%s) :\033[0m %s\n", token.filename.c_str(), token.row_index + 1, token.col_index, token.type, TOKEN_TYPE_STRING[token.type].c_str(), showstr.c_str());
	printf("\033[2m[%s,r:%3d,c:%3d](%d:%s) \033[32m:\033[0m %s\n", token.filename.c_str(), token.row_index + 1, token.col_index + 1, token.type, TOKEN_TYPE_STRING[token.type].c_str(), showstr.c_str());
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

//	THE END