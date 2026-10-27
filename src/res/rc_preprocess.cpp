// ============================================================
// rc_preprocess.cpp — 在 .rc 之上复用现有 Preprocessor 出 token 流
//
// 策略（不触碰共享 preproc 层）：把 .rc 文本经「空白化尖括号系统头」
//   预处理后，塞进一个 CompileOptions 副本（source_path=rc、source_text=
//   改写文本、preload_colang_h=false、include_dirs 透传），交
//   Preprocessor(opts,errs).run(tokens) 展开 #define/#if/#include "resource.h"。
//   · 引号头（resource.h）照常解析（找不到 → Preprocessor 报致命，符合负例要求）。
//   · 尖括号系统头（<windows.h>/<winresrc.h>）v1 内置关键字已覆盖其常量，
//     故整行空白化（保留换行以维持行号），避免「file not found」致命。
// ============================================================

#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "colang/driver.h"
#include "colang/lexer.h"
#include "colang/preproc.h"

namespace colang
{

namespace
{

// 读整文件为字符串（剥 UTF-8 BOM）。失败返回 false。
bool read_text(const std::string &path, std::string &out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
		static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
		out.erase(0, 3);
	return true;
}

// 把 `#include <...>` 整行（含反斜杠续行）空白化为等价空格，保留换行/行号。
// 引号形式 `#include "..."` 不动（交 Preprocessor 正常解析/报错）。
std::string blank_angle_includes(const std::string &text)
{
	std::string out = text;
	std::size_t i = 0;
	bool in_cont = false; // 上一行是被空白化的角头且以反斜杠续行
	while (i <= out.size())
	{
		std::size_t nl = out.find('\n', i);
		const bool last = (nl == std::string::npos);
		const std::size_t line_end = last ? out.size() : nl;

		std::size_t s = i;
		while (s < line_end && (out[s] == ' ' || out[s] == '\t' || out[s] == '\r'))
			++s;
		bool blank = in_cont;
		if (!blank && s < line_end && out[s] == '#')
		{
			std::size_t p = s + 1;
			while (p < line_end && (out[p] == ' ' || out[p] == '\t'))
				++p;
			const std::size_t idb = p;
			while (p < line_end && (std::isalnum(static_cast<unsigned char>(out[p])) || out[p] == '_'))
				++p;
			const std::string dir = out.substr(idb, p - idb);
			if (dir == "include")
			{
				while (p < line_end && (out[p] == ' ' || out[p] == '\t'))
					++p;
				if (p < line_end && out[p] == '<')
					blank = true;
			}
		}

		bool cont = false;
		if (blank)
		{
			// 判续行（忽略行尾 \r\n）——在覆写空格前查看原始行尾
			std::size_t t = line_end;
			while (t > i && (out[t - 1] == '\r' || out[t - 1] == '\n'))
				--t;
			cont = (t > i && out[t - 1] == '\\');
			for (std::size_t k = i; k < line_end; ++k)
				if (out[k] != '\n' && out[k] != '\r')
					out[k] = ' ';
		}
		in_cont = blank && cont;

		if (last)
			break;
		i = nl + 1;
	}
	return out;
}

}  // namespace

// 内部入口（rc_parser.cpp 前向声明调用，不入 winres.h）。
bool rc_preprocess(const std::string &rc_path, const CompileOptions &base,
                   std::vector<Token> &out, std::vector<Error> &errors)
{
	std::string raw;
	if (!read_text(rc_path, raw))
	{
		errors.push_back(Error{rc_path, Pos{1, 1}, "cannot open rc file: " + rc_path});
		return false;
	}

	CompileOptions ropts;
	ropts.source_path = rc_path;
	ropts.source_text = blank_angle_includes(raw);
	ropts.include_dirs = base.include_dirs;
	ropts.target = base.target;
	ropts.target_triple = base.target_triple;
	ropts.preload_colang_h = false; // .rc 不吃 C 标准头
	ropts.rc_dialect = true;        // .rc 方言：保留同行并列的多个字符串 token，不做阶段6 拼接

	Preprocessor pp(ropts, errors);
	return pp.run(out);
}

}  // namespace colang
