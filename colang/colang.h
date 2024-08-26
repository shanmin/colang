//
// colang.h
//
#pragma once

#include <iostream>
#include <map>
#include <vector>

#include "lexer.h"
#include "ast.h"


void ErrorExit(const char* str, TOKEN token);
void ErrorExit(const char* str, std::vector<TOKEN>& tokens);

//	THE END