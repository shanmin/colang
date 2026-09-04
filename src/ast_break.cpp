////////////////////////////////////////////////////////////////////////////////
//
// AST_break  +  AST_continue
//   支持:  break ;         // 等价于 break 1
//          break N ;       // N = 十进制正整数字面量（无符号）
//          continue ;
//          continue N ;
//   仅 for / do / while 会在其 codegen 中把 bbover / bbcontinue 压入 scope 两栈；
//   不在循环内 / N > 嵌套深度 / N==0  → ErrorExit 带诊断源码行
//
////////////////////////////////////////////////////////////////////////////////
#include "colang.h"

// 解析可选的正整数字面量后缀，返回 1-based 层数；返回 false 表示 token 合法但不是数字（保持默认 N=1）。
// 非法情形（十六进制/浮点字面量、负号、超范围）会用关键字 token 定位 ErrorExit。
static unsigned parse_levels_suffix(std::vector<TOKEN>& tokens, const TOKEN& kw)
{
	// N 省略 → 默认 N = 1
	if (tokens.empty()) return 1;
	if (tokens[0].type != TOKEN_TYPE::number) return 1;

	const std::string& s = tokens[0].Value;
	if (s.empty()) {
		ErrorExit("break/continue: missing N suffix", kw);
	}
	// 仅允许十进制正整数：首字符 [1-9] 后接 [0-9]* 或 单个 0？ → 我们禁止 0（=0 由 scope 报错更直接），但仍允许字面量 "0" 被解析为 0 交给 scope 报"必须>=1"
	for (char c : s) {
		if (c < '0' || c > '9') {
			// 含 0x / . / e / f → 不允许
			char msg[256];
			snprintf(msg, sizeof(msg), "break/continue N must be a decimal integer literal; unexpected '%c'", c);
			ErrorExit(msg, tokens[0]);
		}
	}
	// 用 64-bit 安全转，再截到 unsigned（65535 层循环足够）
	unsigned long long v = 0;
	for (char c : s) {
		unsigned d = (unsigned)(c - '0');
		if (v > (unsigned long long)UINT_MAX / 10 || (v == (unsigned long long)UINT_MAX / 10 && d > UINT_MAX % 10)) {
			ErrorExit("break/continue N out of representable range", tokens[0]);
		}
		v = v * 10 + d;
	}
	tokens.erase(tokens.begin());
	return (unsigned)v;
}

static void consume_semicolon(std::vector<TOKEN>& tokens)
{
	if (!tokens.empty() && tokens[0].Value == ";")
		tokens.erase(tokens.begin());
}

AST_break::AST_break(std::vector<TOKEN>& tokens)
	: levels(1)
{
	kw = tokens[0];
	tokens.erase(tokens.begin());
	levels = parse_levels_suffix(tokens, kw);
	consume_semicolon(tokens);
}
void AST_break::show(std::string pre)
{
	std::cout << pre << "#TYPE:break  levels=" << levels << std::endl;
}
llvm::Value* AST_break::codegen()
{
	llvm::BasicBlock* target = scope::get_break_bb(levels, kw);
	ir_builder->CreateBr(target);
	// 创建一个临时"死"BB 作为后续插入点，避免同一 BB 里多 terminator（body 里 break 后续代码被 CFG 自然当死代码忽略）
	llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	llvm::BasicBlock* dead = llvm::BasicBlock::Create(ir_context, "after_break", func);
	ir_builder->SetInsertPoint(dead);
	return nullptr;
}

AST_continue::AST_continue(std::vector<TOKEN>& tokens)
	: levels(1)
{
	kw = tokens[0];
	tokens.erase(tokens.begin());
	levels = parse_levels_suffix(tokens, kw);
	consume_semicolon(tokens);
}
void AST_continue::show(std::string pre)
{
	std::cout << pre << "#TYPE:continue  levels=" << levels << std::endl;
}
llvm::Value* AST_continue::codegen()
{
	llvm::BasicBlock* target = scope::get_continue_bb(levels, kw);
	ir_builder->CreateBr(target);
	llvm::Function* func = ir_builder->GetInsertBlock()->getParent();
	llvm::BasicBlock* dead = llvm::BasicBlock::Create(ir_context, "after_continue", func);
	ir_builder->SetInsertPoint(dead);
	return nullptr;
}
