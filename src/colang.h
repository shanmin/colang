//
// colang.h
//

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "lexer.h"
#include "ast.h"

void ErrorExit(const char* str, TOKEN token);
void ErrorExit(const char* str, std::vector<TOKEN>& tokens);
//warning 输出（方案 B：字面量越界/失精，不改退出码，仅 stderr 警告）
void Warning(const char* str, TOKEN token);

//取输出文件基名：若以 .co 结尾则替换掉扩展名（test.co → test），否则原样返回
std::string co_base(const char* filename);

//	THE END