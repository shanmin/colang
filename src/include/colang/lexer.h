// ============================================================
// lexer.h — 词法器 / pp-token 分类（P1.1 + P1.2 落地，P1.3 起支持拉取模型）
//
// 方案 A（经典分离）：本层是「预处理扫描器」，实现翻译阶段 2–4：
//   - 行拼接（\\\n 续行）
//   - 注释替换为空白（// 与 /* */，phase 3）
//   - 单遍最长匹配切出 pp-token：标识符/关键字、pp-number、字符常数、
//     字符串字面量、运算符标点（含 digraph 等价）
// 词法器不做宏展开与指令处理，那是预处理器（P1.3+）的职责；
// 预处理器按「逻辑行」拉取本层产出的 pp-token（next() 提供拉取能力）。
// ============================================================
#pragma once

#include <string>
#include <vector>

#include "colang/common.h"

namespace colang
{

// pp-token 大类
enum class TokKind
{
	Identifier, // 非关键字标识符
	Keyword,    // C 关键字 / 保留标识符（_Bool、alignas 等）
	Number,     // pp-number（宽松的数值 token，语义留待下游定档）
	Char,       // 字符常数：'x' '\n' L'x' u8'x' 等（前缀并入 text）
	String,     // 字符串字面量："..." 与 u8""/u""/U""/L""（前缀并入 text）
	Punct,      // 运算符 / 标点
	EndOfFile,  // 流结束
};

// 单个 pp-token：精确源码拼写 + 源位置 + 行首标记
struct Token
{
	TokKind kind = TokKind::EndOfFile;
	std::string text; // 源码原文拼写（字符串/字符含引号与前缀，未解码）
	SourceLoc loc;    // token 起点（文件/行/列）
	bool bol = false;     // 前一 token 之后发生过换行（本 token 是逻辑行首）
	bool had_space = false; // 与前一 token 之间隔了空白/注释（非行首时有效）；为函数式宏与 #/## 邻接判定铺路
	// P338：发射点 `#pragma pack` 的当前对齐上限（cap；0=无 pack 作用域，走自然对齐）。
	//   仅预处理器在 run 的逐行发射尾部按 cur_pack_ 盖章，供 parser 在记录定义点读回；
	//   默认 0 使所有既有 token 构造点零改动，且对 -E/print_tokens 文本输出不可见。
	unsigned pack = 0;
};

// next() 三态返回
enum class LexResult
{
	Token, // 已填充一个真实 token
	Eof,   // 已到源末尾（不填充 out，或填 EndOfFile 由实现决定）
	Error, // 词法错误；err 有效
};

// 预处理扫描器：把一个源文件缓冲切分为 pp-token 流
class Lexer
{
public:
	// file/text 由 Lexer 值持有，可安全放入容器/unique_ptr
	Lexer(std::string file, std::string text);

	// 拉取下一个 pp-token。返回 Token 时 out 已填；返回 Eof 时 out.kind=EndOfFile；
	// 返回 Error 时 err 已填。多次调用在 Eof 之后仍返回 Eof（幂等）。
	LexResult next(Token &out, Error &err);

	// 便捷封装：一次切完整个缓冲（--dump-tokens 用）
	bool tokenize(std::vector<Token> &out, std::vector<Error> &errors);

	// 重设行号/文件名（#line 指令用）
	void set_line(std::size_t l) { line_ = l; col_ = 1; }
	void set_file(const std::string &f) { file_ = f; }

	// RC 方言字符串模式（PE 资源档③）：置真后，"..." 按 rc.exe 规则切分——
	// "" 双写视为嵌入引号不终止、单 " 终止、反斜杠不在词法层遮蔽后续字符。
	// 默认关：C 路径逐字节不变。语义解码留待 rc_decode_string 处理。
	void set_rc_string_mode(bool b) { rc_str_ = b; }

private:
	std::string file_;
	std::string text_;
	std::size_t pos_ = 0;
	std::size_t line_ = 1;
	std::size_t col_ = 1; // UTF-8 码点计列，续字节不增列
	bool pending_bol_ = true; // 逻辑行首标记的跨调用状态
	bool pending_space_ = false; // 当前 token 前是否隔了空白/注释（had_space 的跨调用来源）
	bool rc_str_ = false; // RC 方言字符串模式：scan_literal 走 rc.exe 切分（默认关，C 路径不变）

	bool eof() const { return pos_ >= text_.size(); }
	char peek(std::size_t off = 0) const;
	char take();                 // 前进一字节，维护 line_/col_
	bool at_line_splice() const; // 当前是否 反斜杠+换行
	void skip_line_splice();     // 消费一个续行拼接序列
	SourceLoc here() const;      // 当前位置

	// 各类 token 扫描（假定调用方已判定起始字符类别）
	void scan_number(Token &t);                                         // pp-number
	bool scan_literal(Token &t, char quote, const std::string &prefix); // 字符/字符串
	bool scan_punct(Token &t); // 运算符/标点最长匹配（含 digraph 归一）

	// 空白/换行/注释消费；出错时返回 false（错误写入 err）
	bool skip_blanks(Error &err);
};

// 标识符是否为 C 关键字 / 保留标识符（覆盖到 C23）。
// 本项不分版本档（无 -std 门控），此处只做识别；具体方言基线由 sema/parser 定。
bool is_keyword(const std::string &word);

}  // namespace colang
