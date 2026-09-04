//
//	ast.cpp
//

#include "colang.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

std::unique_ptr<llvm::IRBuilder<>> ir_builder;
llvm::Module* ir_module;
llvm::LLVMContext ir_context;



//std::vector<VAR_LIST> ir_varlist; //局部变量范围
std::vector<LABEL_LIST> ir_labellist; //局部标签

std::map<std::string, int> IR_EXPR_PRI =
{
	//优先级数值用 10 的倍数分配，留间隙便于将来插入新运算符。
	//赋值类固定为 1：ast_parse_expr 用 right_pri==1 判断右结合与返回时机。
	{"*",80},
	{"/",80},
	{"%",80}, //取模：与 * / 同优先级，左结合；codegen 按 is_un 选 srem/urem
	{"+",70},
	{"-",70},
	{"<<",60}, //左移：整型 Shl，不区分符号性
	{">>",60}, //右移：无符号 LShr（逻辑），有符号 AShr（算术）
	{">",50},
	{"<",50},
	{">=",50},
	{"<=",50},
	{"==",50},
	{"!=",50},
	{"&",40}, //按位与：整型 And
	{"|",30}, //按位或：整型 Or（将来 ^ 可放 35）
	{"&&",20}, //逻辑与（当前按位语义，非短路）
	{"||",10}, //逻辑或（当前按位语义，非短路）
	{"=",1},
	{"+=",1}, //复合赋值与 = 同优先级，右结合，codegen 里等价展开为 A = A op B
	{"-=",1},
	{"*=",1},
	{"/=",1},
	// "." 字段访问："优先级最高"。注意：在 ast_parse_expr while 主循环里，
	//  当 right_pri >= left_pri 时会 consume 下一个 operand，再按 priority 回卷；
	//  "." 需要比 +- */ 都高（例如 a.b+1 → (a.b)+1），所以放 160 足够；
	//  同时 "." 右操作数不是"表达式"而是"字段 code token"（即 . 不支持右递归穿透到一般表达式），
	//  我们会在 ast_parse_expr 遇到 op=="." 时特殊处理：直接读下一个 code token 当字段名，
	//  构造 right = AST_value(TOKEN_type=code, value=fieldname)，不走 ast_parse_expr1 递归。
	{".",160}
};

//判断是否为复合赋值运算符（+= -= *= /=）
bool is_compound_assign(const std::string& op)
{
	return op == "+=" || op == "-=" || op == "*=" || op == "/=";
}

////////////////////////////////////////////////////////////////////////////////
//
// ast
//
////////////////////////////////////////////////////////////////////////////////

//解析AST
std::vector<AST*> ast(std::vector<TOKEN>& tokens)
{
	std::vector<AST*> ast_list;
	while (!tokens.empty())
	{
		if (tokens[0].type != TOKEN_TYPE::string && tokens[0].Value == "}")
		{
			tokens.erase(tokens.begin());
			break;
		}
		AST* a = ast1(tokens);
		if (a)
			ast_list.push_back(a);
	}
	return ast_list;
}


//AST解析
//	oneblock 只解析一段，直到“}”或“;”结尾为止。
AST* ast1(std::vector<TOKEN>& tokens)
{
	while (!tokens.empty())
	{
		if (tokens[0].type == TOKEN_TYPE::noncode)//非代码，直接输出
		{
			return new AST_noncode(tokens);
		}

		if (tokens[0].type != TOKEN_TYPE::string)
		{
			if (tokens[0].Value == "{")
			{
				return new AST_codeblock(tokens);
			}
			else if (tokens[0].Value == "}")
			{
				return NULL;
			}
			else if (tokens[0].Value == ";")
			{
				tokens.erase(tokens.begin());
				return NULL;
			}

			//import "filename";  或  import filename;
			//— 必须放最前，避免被 code-code 分支误判为类型/变量声明
			//语法：import <字符串token|裸标识符> ;   （字符串 token 已被 lexer 去掉外层引号，Value 是纯文件名）
			if (tokens[0].Value == "import")
			{
				return new AST_import(tokens);
			}

			if (tokens[0].Value == "do") return new AST_do(tokens);
			// FIX（2026-09-02，P0#1）：tokens[n] 访问前统一加 size 守卫，避免 UB 崩溃
			if (tokens.size() >= 2 && tokens[0].Value == "for" && tokens[1].Value == "(") return new AST_for(tokens);
			if (tokens[0].Value == "goto") return new AST_goto(tokens);
			if (tokens.size() >= 2 && tokens[0].Value == "if" && tokens[1].Value == "(") return new AST_if(tokens);
			if (tokens.size() >= 2 && tokens[0].Value == "while" && tokens[1].Value == "(") return new AST_while(tokens);
			if (tokens[0].Value == "return") return new AST_return(tokens);
			if (tokens[0].Value == "break") return new AST_break(tokens);
			if (tokens[0].Value == "continue") return new AST_continue(tokens);
			// delete 语句：delete 表达式; → 先调析构（若定义）再 free
			if (tokens[0].Value == "delete" && tokens.size() >= 2)
				return new AST_delete(tokens);
			// struct 定义（含可选 public/private 修饰符，与 AST_class 对称；方案 B 对 struct 也启用 import 可见性过滤）
			//   无修饰：struct NAME { → 3 tokens
			//   有修饰：[public|private] struct NAME { → 4 tokens
			{
				bool s_has_vis = (tokens.size() >= 1
					&& tokens[0].type == TOKEN_TYPE::code
					&& (tokens[0].Value == "public" || tokens[0].Value == "private"));
				int s_ti_kw   = s_has_vis ? 1 : 0;  // "struct" keyword 索引
				int s_ti_name = s_has_vis ? 2 : 1;  // struct NAME 的 NAME 索引
				int s_ti_lb   = s_has_vis ? 3 : 2;  // "{" 索引
				if (tokens.size() > s_ti_lb
					&& tokens[s_ti_kw  ].Value == "struct"
					&& tokens[s_ti_name].type  == TOKEN_TYPE::code
					&& tokens[s_ti_lb  ].type  == TOKEN_TYPE::opcode
					&& tokens[s_ti_lb  ].Value == "{")
				{
					return new AST_struct(tokens);
				}
			}

			// class 定义（含可选 public/private 修饰符）
			//   无修饰：class Name { → 3 tokens
			//   有修饰：[public|private] class Name { → 4 tokens
			{
				bool class_has_vis = (tokens.size() >= 1
					&& tokens[0].type == TOKEN_TYPE::code
					&& (tokens[0].Value == "public" || tokens[0].Value == "private"));
				int c_ti_class = class_has_vis ? 1 : 0;  // "class" keyword 索引
				int c_ti_name  = class_has_vis ? 2 : 1;  // class Name 的 Name 索引
				int c_ti_lb    = class_has_vis ? 3 : 2;  // "{" 索引
				if (tokens.size() > c_ti_lb
					&& tokens[c_ti_class].Value == "class"
					&& tokens[c_ti_name ].type  == TOKEN_TYPE::code
					&& tokens[c_ti_lb   ].type  == TOKEN_TYPE::opcode
					&& tokens[c_ti_lb   ].Value == "{")
				{
					return new AST_class(tokens);
				}
			}

			// 用户规则：变量声明「struct T v; / struct T* p;」不支持，只能「T v; / T* p;」
			//   命中"struct T + 变量名"3-token 组合 → 明确报错，避免被下一个 code-code 分支误吃为 int a; 式的"struct 是类型名 + T=变量名"（那样 ir_type 会走到『未定义类型名: struct』难以理解）
			if (tokens[0].Value == "struct" && tokens.size() >= 3 && tokens[1].type == TOKEN_TYPE::code &&
				(tokens[2].type == TOKEN_TYPE::code ||
				 (tokens[2].type != TOKEN_TYPE::string && tokens[2].Value == "*")))
			{
				ErrorExit("'struct NAME var;' is not supported in variable declarations; use 'NAME var;' directly (struct must be declared before use)", tokens);
			}

			// FIX（2026-09-02，P0#1）：tokens[1]/tokens[2] 前补 size 守卫
			if (tokens.size() >= 2 && tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == ":")
				return new AST_label(tokens);

			//Function
			//	Ex:  [public|private] int function_name(int arg1)
			//		 |             |		|              + args
			//       |             |      + 函数名称
			//       |             +  返回值类型
			//       + 访问修饰符（可选；缺省=private）
			//判断依据：
			//   无修饰：code(ret_type) code(func_name) '('  → 3 tokens
			//   有修饰："public"|"private"(code) code(ret_type) code(func_name) '('  → 4 tokens
			{
				bool has_vis = tokens.size() >= 1
					&& tokens[0].type == TOKEN_TYPE::code
					&& (tokens[0].Value == "public" || tokens[0].Value == "private");
				int need_sz = has_vis ? 4 : 3;
				int ti_type = has_vis ? 1 : 0;   // 返回类型 token 索引
				int ti_name = has_vis ? 2 : 1;   // 函数名 token 索引
				int ti_lp   = has_vis ? 3 : 2;   // '(' token 索引
				if (tokens.size() >= need_sz
					&& tokens[ti_type].type == TOKEN_TYPE::code
					&& tokens[ti_name].type == TOKEN_TYPE::code
					&& tokens[ti_lp].type   == TOKEN_TYPE::opcode
					&& tokens[ti_lp].Value  == "(")
				{
					return new AST_function(tokens);
				}
			}

			//模块限定函数调用
			//Ex:	m1.aa("abc");
			if (tokens.size() >= 4 && tokens[0].type == TOKEN_TYPE::code
				&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "."
				&& tokens[2].type == TOKEN_TYPE::code
				&& tokens[3].type == TOKEN_TYPE::opcode && tokens[3].Value == "(")
			{
				return new AST_call(tokens);
			}

			//函数调用
			//Ex:	printf("abc");
			if (tokens.size() >= 2 && tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "(")
			{
				return new AST_call(tokens);
			}

			//变量定义
			//  内建: int a;  int b = 2;
			//  struct: Vec v;     — 要求 Vec 已在 struct 表（struct Vec{} 定义在先，或至少 forward）
			//  指针: Vec* pv;
			// 判断依据：tokens[0] code && (tokens[1] code [var name or * with name following])
			//  注意: 放在 function-definition 分支之后，避免误吞 "Vec make(int x){}" 这种带 struct 返回值的函数定义
			if (tokens.size() >= 2 && tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::code && tokens[1].Value != "(")
			{
				return new AST_var(tokens);
			}
			// Vec *pv; — tokens[1] 是 "*"，前面分支没覆盖到 → 加此分支
			// 同时也兼容: Vec *a, **b; 当前 colang 不做多变量单声明，所以 tokens[2] 应该是变量名 code
			if (tokens.size() >= 3 && tokens[0].type == TOKEN_TYPE::code && tokens[1].type != TOKEN_TYPE::string && tokens[1].Value == "*" && tokens[2].type == TOKEN_TYPE::code)
			{
				return new AST_var(tokens);
			}
			// ---- 模块限定的类型名（"import 模块名.class 名 变量名 / *变量名"）----
			//  Ex: test3.TEST3 tt3;   → tokens[0]=test3 [1]=. [2]=TEST3 [3]=tt3(code)
			//  Ex: test3.TEST3 *tt3;  → tokens[0]=test3 [1]=. [2]=TEST3 [3]=*    [4]=tt3(code)
			//  内部让 AST_var 构造器识别"code . code"开头，并在 ir_type 之前剥离 mod. 前缀
			if (tokens.size() >= 4
				&& tokens[0].type == TOKEN_TYPE::code   // mod
				&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "."   // .
				&& tokens[2].type == TOKEN_TYPE::code   // ClassName
				&& tokens[3].type == TOKEN_TYPE::code)  // varname
			{
				return new AST_var(tokens);
			}
			if (tokens.size() >= 5
				&& tokens[0].type == TOKEN_TYPE::code
				&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "."
				&& tokens[2].type == TOKEN_TYPE::code
				&& tokens[3].type != TOKEN_TYPE::string && tokens[3].Value == "*"
				&& tokens[4].type == TOKEN_TYPE::code)
			{
				return new AST_var(tokens);
			}
			// （防御：Vec **v; / mod.Class **v 没写。当前语法不支持多级指针 *v** 后声明名，若要再加对应分支）
		}

		return ast_parse_expr(tokens);
	}
}



//读取一个表达式
AST* ast_parse_expr1(std::vector<TOKEN>& tokens);
// 字段访问专用「取右 operand」：. 运算符右侧必须是"纯 code 字段名"。
static AST* ast_parse_expr_right_for_dot(std::vector<TOKEN>& tokens);



//解析表达式
//	tokens
//	left_pri=0
//	left=NULL
AST* ast_parse_expr(std::vector<TOKEN>& tokens, int left_pri, AST* left)
{
	if (left_pri == 0) //首次读取
	{
		left = ast_parse_expr1(tokens);
	}
	left = ast_parse_expr_add1(left, tokens); //对++和--操作符进行处理

	while (!tokens.empty())
	{
		int right_pri = 0;
		TOKEN op;
		static const bool _dbg_parse = (std::getenv("COPARSERDBG") != nullptr);
		if (_dbg_parse) {
			const char* tnames[] = {"?", "num", "code", "str", "opc", "kw"};
			int ti = std::min((int)tokens[0].type, (int)5);
			fprintf(stderr, "[P] HEAD lp=%d t0.type=%s t0.val='%s'  head16=",
				left_pri, tnames[ti], tokens[0].Value.c_str());
			for (size_t k = 0; k < std::min((size_t)16, tokens.size()); k++) {
				if (k) fputc(' ', stderr);
				fprintf(stderr, "%s", tokens[k].Value.c_str());
			}
			fputc('\n', stderr);
			fflush(stderr);
		}
		//读取下一个操作符，计算操作优先级
		if (tokens[0].type == TOKEN_TYPE::opcode)
		{
			if (tokens[0].Value == ")")
			{
				tokens.erase(tokens.begin());
				if (_dbg_parse) { fprintf(stderr, "[P] HEAD consume ')'\n"); fflush(stderr); }
				return left;
			}
			// ";" / "," / "{" / "}" 不是表达式的一部分，立即停止表达式解析，留给调用方（for/if/while/codeblock）处理。
			// 否则 "if(COND){ stmts; }" 在 COND 解析完后，while 会把 "{" 当 opcode 查 IR_EXPR_PRI 报错"未识别的运算符"。
			if (tokens[0].Value == ";" || tokens[0].Value == "," || tokens[0].Value == "{" || tokens[0].Value == "}")
			{
				if (_dbg_parse) { fprintf(stderr, "[P] HEAD stop opcode='%s'\n", tokens[0].Value.c_str()); fflush(stderr); }
				return left;
			}

			if (IR_EXPR_PRI.find(tokens[0].Value) == IR_EXPR_PRI.end())
				ErrorExit("unrecognized operator", tokens);
			right_pri = IR_EXPR_PRI[tokens[0].Value];
			// FIX（2026-09-01）：优先级判断必须在 erase 之前！
			//   旧代码先 erase op 再判断 rp<lp，导致递归层（如 "*" 的右操作数是 b.x 触发
			//   ast_parse_expr(lp=6) 右递归）遇到更低优先级运算符（如 "+"）时，"+" 已被
			//   白白消费掉再返回 —— 上层表达式被截断、运算符凭空消失。
			//   复现：return a.x * b.x + a.y * b.y;  → 只返回 a.x*b.x（dot_vec 输出 15 而非 63）。
			//   修复后：rp<lp 直接 return（不消费 op），op 留给上层处理。
			// FIX2（2026-09-01）：结合性！递归层入口 lp=right_pri（见下方 right_rec 的递归调用），
			//   遇到"优先级不高于自己"的运算符必须 return —— 否则同级运算符被递归层吞掉，
			//   左结合变右结合：100-3*7+24 → 100-(3*7+24)=55（应 103）。
			//   仅赋值类（pri==1）例外：同级继续消费构成右结合（a=b=c=7）。
			if (right_pri < left_pri || (right_pri == left_pri && left_pri != 1))
			{
				if (_dbg_parse) { fprintf(stderr, "[P] rp(%d) <=%s lp(%d) -> return left (op kept)\n", right_pri, (right_pri == left_pri ? "=" : ""), left_pri); fflush(stderr); }
				return left;
			}
			op = tokens[0];
			tokens.erase(tokens.begin());
		}
		else
		{
			if (_dbg_parse) { fprintf(stderr, "[P] HEAD not-opcode -> return\n"); fflush(stderr); }
			return left;
		}


		//if (tokens.size() > 2 && tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "++")
		//{
		//	std::vector<TOKEN> tmp;
		//	{
		//		TOKEN t=tokens[1];
		//		t.Value = "(";
		//		tmp.push_back(t);
		//	}
		//	tmp.push_back(tokens[0]);
		//	{
		//		TOKEN t = tokens[1];
		//		t.Value = "=";
		//		tmp.push_back(t);
		//	}
		//	tmp.push_back(tokens[0]);
		//	{
		//		TOKEN t = tokens[1];
		//		t.Value = "+";
		//		tmp.push_back(t);
		//	}
		//	{
		//		TOKEN t = tokens[0];
		//		t.type = TOKEN_TYPE::number;
		//		t.Value = "1";
		//		tmp.push_back(t);
		//	}
		//	{
		//		TOKEN t = tokens[1];
		//		t.Value = ")";
		//		tmp.push_back(t);
		//	}
		//	//
		//	tokens.erase(tokens.begin());
		//	tokens.erase(tokens.begin());
		//	tokens.insert(tokens.begin(), tmp.begin(),tmp.end());
		//}

		AST* right = (op.Value == ".")
			? ast_parse_expr_right_for_dot(tokens)   // . 右侧 = 纯字段名 token
			: ast_parse_expr1(tokens);               // 其余右侧 = 普通表达式（primary）

		int next_pri = 0;
		if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::opcode)
		{
			if (IR_EXPR_PRI.find(tokens[0].Value) != IR_EXPR_PRI.end())
				next_pri = IR_EXPR_PRI[tokens[0].Value];
		}

		// "." 左结合且禁止右穿透：即使 next_pri=="."（如 a.b.c），也不能让"a . (b . c)"右递归。
		// 这里 right_pri==160 (".") 满足 right_pri < next_pri 会走右穿透改成 (b . c)，所以需要排除 "." 自身的右递归。
		bool right_rec = (right_pri < next_pri || (right_pri == 1 && right_pri == next_pri));
		if (op.Value == "." || right_pri == 160) right_rec = false;  // "." 永远不做右穿透
		if (_dbg_parse) {
			std::string ltok;
			for (size_t k = 0; k < std::min((size_t)16, tokens.size()); k++) {
				if (k) ltok += " ";
				ltok += tokens[k].Value;
			}
			fprintf(stderr, "[P] lp=%d op='%s' rp=%d next='%s'(%d) right_rec=%d  tok0...=%s\n",
				left_pri, op.Value.c_str(), right_pri,
				(next_pri ? tokens[0].Value.c_str() : "-"),
				next_pri,
				right_rec ? 1 : 0,
				ltok.c_str());
			fflush(stderr);
		}
		if (right_rec)
		{
			right = ast_parse_expr(tokens, right_pri, right);
			if (!right)
				return nullptr;
		}
		left = new AST_expr(left, op, right);

		//赋值类 (= += -= *= /=) 优先级最低；解析完一个赋值表达式，后续即便优先级更高的运算符，也不能把它当作左操作数。
		//否则 for(float k=0;k<1;k=k+1.0){...} 解析到 expr3 的 “k=k+1.0” 时：吃掉右递归后，外层 while 继续，碰到下一个非 IR_EXPR_PRI 运算符（例如 { ）会被当作 opcode 报错。
		if (right_pri == 1) {
			// 赋值返回前先消费当前上下文中成对的 “)”，否则当赋值没有右递归优先级穿透时（如 k=1.0 / k+=1），
			// 当前 while 不会再进入下一轮，) 就留在 tokens 里，让 for/AST_call 的后续 body 解析错位。
			if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == ")") {
				tokens.erase(tokens.begin());
			}
			return left;
		}
	}

	return left;
}

//读取一个表达式
AST* ast_parse_expr1(std::vector<TOKEN>& tokens)
{
	// new 表达式：new ClassName(args)
	if (tokens.size() >= 2 && tokens[0].type == TOKEN_TYPE::code && tokens[0].Value == "new"
		&& tokens[1].type == TOKEN_TYPE::code)
	{
		return new AST_new(tokens);
	}

	//一元前缀运算符：~ 位取反、- 负号（将来扩展 ! 逻辑非）
	//  注：二元减号的 '-' 在 ast_parse_expr 的 while 循环里被消费，不会落到这里；
	//  能落到 ast_parse_expr1 的 '-' 一定是一元前缀（表达式开头 / '(' 之后 / 运算符之后）。
	//  递归调用 ast_parse_expr1 解析操作数，支持 --x、~~x、-(a+b)、~-x 等链式套嵌
	if (tokens[0].type == TOKEN_TYPE::opcode &&
		(tokens[0].Value == "~" || tokens[0].Value == "-"))
	{
		TOKEN op = tokens[0];
		tokens.erase(tokens.begin());
		AST* operand = ast_parse_expr1(tokens);
		if (!operand) ErrorExit("unary operator missing operand", op);
		return new AST_unary(op, operand);
	}
	//模块限定函数调用（表达式级）：m1.aa(args)
	if (tokens.size() >= 4 && tokens[0].type == TOKEN_TYPE::code
		&& tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "."
		&& tokens[2].type == TOKEN_TYPE::code
		&& tokens[3].type == TOKEN_TYPE::opcode && tokens[3].Value == "(")
	{
		return new AST_call(tokens);
	}
	if (tokens[0].type == TOKEN_TYPE::code && tokens[1].type == TOKEN_TYPE::opcode && tokens[1].Value == "(")
	{
		return new AST_call(tokens);
	}
	if (tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "(")
	{
		tokens.erase(tokens.begin());
		return ast_parse_expr(tokens);
	}
	return new AST_value(tokens);
}

// 字段访问专用「取右 operand」：. 运算符右侧必须是"纯 code 字段名"，不能是任意表达式。
// 正常 ast_parse_expr1 对「.f」会走 AST_value(f)，语义上也成立，但为了"字段名必须是标识符"
// 做语法层限制：手动把下一个 token（应该是 code TOKEN）当字段名；非 code 则报错。
static AST* ast_parse_expr_right_for_dot(std::vector<TOKEN>& tokens)
{
	if (tokens.empty()) ErrorExit("'.' missing field name on the right", tokens);
	if (tokens[0].type != TOKEN_TYPE::code) ErrorExit("'.' right-hand side must be a field identifier", tokens);
	return new AST_value(tokens);   // 构造后 tokens[0] 被 erase（AST_value(tokens) 首行 erase）
}


//对++和--操作符进行处理
//	这里对a++改写为a=a+1
AST* ast_parse_expr_add1(AST* old, std::vector<TOKEN>& tokens)
{
	if (tokens[0].type == TOKEN_TYPE::opcode && (tokens[0].Value == "++" || tokens[0].Value == "--"))
	{
		TOKEN opa = tokens[0];
		if (tokens[0].Value == "++")
			opa.Value = "+";
		else
			opa.Value = "-";

		TOKEN code1 = tokens[0];
		code1.type = TOKEN_TYPE::number;
		code1.Value = "1";
		AST_value* value1 = new AST_value(code1);

		AST* add = new AST_expr(old, opa, value1);

		TOKEN ope = tokens[0];
		ope.Value = "=";

		AST* ret = new AST_expr(old, ope, add);

		tokens.erase(tokens.begin());

		return ret;
	}
	return old;
}


//显示AST信息
void ast_echo(std::vector<AST*> ast_list, std::string pre)
{
	for (AST* a : ast_list)
	{
		a->show(pre);
	}
}



////////////////////////////////////////////////////////////////////////////////
//
// ir
//
////////////////////////////////////////////////////////////////////////////////


void ir(std::vector<AST*>& ast_list, const char* filename)
{
	const bool is_nested = ir_is_nested_mode();

	ir_module = new llvm::Module(filename, ir_context);
	//设置目标三元组：无 triple 的模块会导致 clang 生成时警告并覆盖、llc 退化为输出汇编文本
	ir_module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
	ir_builder = std::make_unique<llvm::IRBuilder<>>((ir_context));

	//预声明外部函数 printf，模板文本（noncode）输出依赖它，无需在 Co 源码中手动声明
	{
		std::vector<llvm::Type*> printf_args;
		printf_args.push_back(llvm::PointerType::get(llvm::Type::getInt8Ty(ir_context), 0));
		llvm::FunctionType* printf_type = llvm::FunctionType::get(ir_builder->getInt32Ty(), printf_args, true);
		llvm::Function::Create(printf_type, llvm::Function::ExternalLinkage, "printf", ir_module);
	}

	//设置当前全局变量作用域
	scope::push("global");

	LABEL_LIST labellist;
	ir_labellist.push_back(labellist);

	llvm::Function* main = nullptr;
	if (!is_nested) {
		// 顶层模式：是否由用户明确写出了「int main()」函数？
		//   有：不预建空 main 占位（预建会冲突，用户函数被 LLVM 自动改名 main.1 后链接 CRT 找不到真正入口）
		//   无：为兼容已有用例（顶层表达式/变量+无显式main），仍预建空 main。
		bool user_has_main = false;
		for (AST* a : ast_list) {
			if (a && a->is_int_main_function_def()) { user_has_main = true; break; }
		}
		if (!user_has_main) {
			llvm::FunctionType* mainType = llvm::FunctionType::get(ir_builder->getInt32Ty(), false);
			main = llvm::Function::Create(mainType, llvm::GlobalValue::ExternalLinkage, "main", ir_module);
			llvm::BasicBlock* entryMain = llvm::BasicBlock::Create(ir_context, "entry_main", main);
			ir_builder->SetInsertPoint(entryMain);
		} else {
			// 有显式 main：留给 AST_function::codegen 自己建 Function + entryBB + SetInsertPoint。
			// 顶层「用户代码以外」的可执行语句（noncode 输出 / 顶层 var alloca）——现有 test_*.co：
			//   当有显式 main 时，用户不应把这些写在 main 外。
			// 为安全起见，ir_builder 先「无插入点」（null）。AST_function main 创建时会 SetInsertPoint。
			// 极端情况：若有人在 main 定义前写了顶层赋值/表达式，builder 在无插入点下 CreateXXX 会断言/崩溃。
			//   → 这是用户错误（Co 约定可执行语句在函数内），保持崩溃给用户反馈。
			// 但 noncode（模板文本输出）是通过 printf call 写入，当前走的是 AST_noncode::codegen() 内部
			//   使用当前 builder → 同样需要插入点。先建立一个临时 "co_nop_main_entry" 挂在占位 BB 上不可行
			//   （占位 Function 会跟用户 main 冲突）。折中：
			//   - 如果用户代码里 import 之前有 noncode，基本没有（模板 <?co 开始于文件），忽略
			// main 指针暂时留空 → ret / sweep 的 is_main 逻辑按指针判断为 F（没 main 函数不会误扫它）
			//   等 AST_function::codegen 运行到 int main() 时内部创建 main Function，但那函数
			//   返回 i32 0（用户自己写 return）—— ir() 末尾的 ret 补全只在 main!=nullptr
			//   （即我们自建的那个空 main）时执行，用户 main 不会被补 return i32 0，
			//   完全符合 C 语义（int main return 可省略但本编译器按 C++ 要求不可省略，用户漏写
			//   会走到 sweep 补 ret void 吗？sweep 里 non-main 且无 terminator → CreateRetVoid。int main
			//   应该 ret i32 0，CreateRetVoid 类型错 → 会导致 IR 验证失败）。
			// 因此：「有用户显式 main」 且 「main 最后 BB 无 terminator」且 「ret_type == int」 → 改为补 ret i32 0。
			// 在 sweep 里用 FunctionType->getReturnType() 选 ret：non-main 函数也应该 ret 对应类型（之前版本
			//   sweep 默认 CreateRetVoid 对返回非 void 的函数是错的，但历史 test_*.co 都能正常过。
			// 现在修复 sweep：用函数自身 return type 生成 ReturnInst。
			main = nullptr;  // 不要预建
		}
	} else {
		// 嵌套（import）模式：不建 main。
		// 为了让顶层语句（变量声明/函数外的表达式赋值/noncode 模板）不崩溃，
		// 创建一个隐藏初始化 BasicBlock 挂到"__init__"函数（只用于 IRBuilder 插入点；
		// 将来 Co 有顶层执行语义时再改为 __co_globals_ctor 类似机制）。
		// 注意：import 模块里若用户写了非函数/struct/var/import 的可执行语句（如 if/while/表达式）
		//       会被塞进 __init__，但当前不会被主程序调用（这是未来扩展点）。
		llvm::FunctionType* initType = llvm::FunctionType::get(llvm::Type::getVoidTy(ir_context), false);
		llvm::Function* initfn = llvm::Function::Create(initType, llvm::GlobalValue::InternalLinkage,
			ir_mangle_symbol("__init__"), ir_module);
		llvm::BasicBlock* initEntry = llvm::BasicBlock::Create(ir_context, "entry", initfn);
		ir_builder->SetInsertPoint(initEntry);
		// 让 sweep 不把 main 相关逻辑误跑在 __init__ 上：main 仍为 nullptr
	}

	//ir_proc(tokens, true);
	for (auto a : ast_list)
		a->codegen();

	if (!is_nested) {
		// 顶层模式：插入点所在 BB 若无 terminator → 按当前 BB 的 parent 函数的 return 类型补 return。
		//  （兼容两种情况：main 是我们预建的 → 插入点在预建 entry_main → ret_type=i32；
		//    main 是用户显式写的 → codegen 完 main 最后一个 return 后，插入点通常已存在 terminator，
		//    如果用户漏写 return 且插入点仍在 main 最后 BB → 按 ret_type 补 return i32 0）
		{
			llvm::BasicBlock* cur = ir_builder->GetInsertBlock();
			if (cur && !cur->getTerminatorOrNull())
			{
				ir_builder->SetInsertPoint(cur);
				llvm::Function* parentFn = cur->getParent();
				// ===== 析构：顶层模式用户漏写 return，补 ret 前必须把所有作用域栈 class 对象反序析构 =====
				scope::generate_all_dtor_calls_to_leave_functions();
				if (parentFn) {
					llvm::Type* rty = parentFn->getReturnType();
					if (rty->isVoidTy()) {
						ir_builder->CreateRetVoid();
					} else {
						ir_builder->CreateRet(llvm::Constant::getNullValue(rty));
					}
				} else {
					// 没有 parent（极端：BB orphan）→ 兜底 ret i32 0
					ir_builder->CreateRet(ir_builder->getInt32(0));
				}
			}
		}
	} else {
		// 嵌套模式：__init__ 函数补 ret void（如果插入点仍在 __init__ 的最后块且无 terminator）
		{
			llvm::BasicBlock* cur = ir_builder->GetInsertBlock();
			if (cur && !cur->getTerminatorOrNull())
			{
				ir_builder->SetInsertPoint(cur);
				// ===== 析构：嵌套模式尾补 ret 前清理（嵌套 ir() 结尾，通常对象在 import 侧不在这里，仅兜底）=====
				scope::generate_all_dtor_calls_to_leave_functions();
				ir_builder->CreateRetVoid();
			}
		}
	}

	std::string mstr;

	//验证之前：模块级 basic block terminator 补全/修剪。
	for (llvm::Function& fn : *ir_module) {
		const bool is_main = (main != nullptr) && (&fn == main);
		// 函数自身的 return type（用于 non-main 缺 terminator 时补 null ret，而不是一律 ret void）
		llvm::Type* fn_rty = fn.getReturnType();
		for (llvm::BasicBlock& bb : fn) {
			const bool is_main_entry = is_main && (&bb == &fn.getEntryBlock());
			llvm::Instruction* last_term = nullptr;
			for (llvm::Instruction& I : bb)
				if (I.isTerminator()) last_term = &I;
			if (last_term) {
				// 删：last_term 之后的指令（terminator 之后的非终止指令 → verify "Terminator found in middle of BB"）
				std::vector<llvm::Instruction*> to_erase;
				llvm::BasicBlock::iterator it = std::next(last_term->getIterator());
				for (; it != bb.end(); ++it) to_erase.push_back(&*it);
				for (llvm::Instruction* I : to_erase) {
					I->dropAllReferences();
					I->eraseFromParent();
				}
				// is_main_entry 且最后 terminator 不是 return/branch：把它换成 ret i32 0（原来只有预建 main 触发）
				//   现在扩大：main 指针是预建 main；但用户显式 main 的 is_main=false（指针没记录）。
				//   为兼容用户显式 main 最后 BB 的 bad terminator，补条件：fn.getName()=="main" 且 is_main_entry 等价。
				//   但 entry_block 一定是第一个 BB，对 main 来说就是它。直接用：fn 叫 main → 当作 main 处理。
				bool really_main = is_main || (!is_nested && fn.getName() == "main");
				if (really_main && is_main_entry && !llvm::isa<llvm::ReturnInst>(last_term) && !llvm::isa<llvm::BranchInst>(last_term)) {
					llvm::Instruction* bad_term = last_term;
					llvm::ConstantInt* zero32 = ir_builder->getInt32(0);
					llvm::IRBuilder<> tmp(ir_context);
					tmp.SetInsertPoint(&bb);
					llvm::ReturnInst* good_ret = tmp.CreateRet(zero32);
					(void)good_ret;
					bad_term->dropAllReferences();
					bad_term->eraseFromParent();
				}
			}
			else {
				bool really_main = is_main || (!is_nested && fn.getName() == "main");
				if (really_main && is_main_entry) {
					llvm::ConstantInt* zero32 = ir_builder->getInt32(0);
					llvm::IRBuilder<> tmp(ir_context);
					tmp.SetInsertPoint(&bb);
					tmp.CreateRet(zero32);
				} else {
					llvm::IRBuilder<> tmp(ir_context);
					tmp.SetInsertPoint(&bb);
					// non-main：按 return type 补 ret（null value）；void 则 CreateRetVoid
					if (fn_rty->isVoidTy())
						tmp.CreateRetVoid();
					else
						tmp.CreateRet(llvm::Constant::getNullValue(fn_rty));
				}
			}
		}
	}

	//验证模块是否存在问题
	llvm::raw_string_ostream merr(mstr);
	bool result = llvm::verifyModule(*ir_module, &merr);
	if (result)
	{
		ir_module->print(llvm::outs(), nullptr);
		printf("\n---------- ERROR ----------\n");
		printf("module verification error: %s", mstr.c_str());
		exit(2);
	}

	//输出文件名：取基名（.co 扩展名替换为 .ll / .bc，见 co_base）
	std::string base = co_base(filename);
	std::error_code ec;

	//输出ll格式文件
	std::string ll = base + ".ll";
	llvm::raw_fd_ostream fout(ll, ec);
	ir_module->print(fout, nullptr);
	fout.close();

	//输出bc格式文件
	std::string bc = base + ".bc";
	llvm::raw_fd_ostream fout_bc(bc, ec);
	llvm::WriteBitcodeToFile(*ir_module, fout_bc);
	fout_bc.close();

	//把本模块 .bc 加入链接清单（顶层+嵌套都要）
	import_add_bc_to_link_list(std::filesystem::absolute(std::filesystem::path(bc)).string());

	// 仅顶层模式清 scope 全局状态。嵌套模式：
	//   IRStateSaver RAII 在 ast_import.cpp 外层已 save/restore 外层 scope，
	//   此处若调 clear_all_compile 会把外层刚恢复的 struct/var/loop 表再次清空；
	//   因此嵌套模式保持"不清理 scope"，外层 RAII swap 回来时一切恢复原位（嵌套层
	//   的 scope 改动只作用在 copy 副本上，不影响外层）。
	//   labellist 同样只在顶层清：嵌套层 labellist 由外层 saver swap 保护。
	if (!is_nested) {
		scope::clear_all_compile();
		ir_labellist.clear();
	}

	// 顶层模式结尾：写 import_list.txt 供 bat 批处理消费
	if (!is_nested) {
		import_write_manifest(filename);
	}
}



//变量类型，从token中获取对应的实类型
llvm::Type* ir_type(std::vector<TOKEN>& tokens)
{
	llvm::Type* type = NULL;
	std::string code = tokens[0].Value;

	if (code == "void") type = llvm::Type::getVoidTy(ir_context);
	else if (code == "bool") type = llvm::Type::getInt1Ty(ir_context); //布尔：i1，true/false 字面量
	else if (code == "char") type = llvm::Type::getInt8Ty(ir_context);
	else if (code == "short") type = llvm::Type::getInt16Ty(ir_context);
	else if (code == "int") type = llvm::Type::getInt32Ty(ir_context);
	else if (code == "long") type = llvm::Type::getInt64Ty(ir_context);
	//无符号整数：LLVM IR 的 i8/i16/i32/i64 本身不区分符号性，与有符号类型同宽，
	//符号差异只在指令选择（udiv/icmp ugt 等）时依据 VARINFO.un 处理
	else if (code == "byte") type = llvm::Type::getInt8Ty(ir_context);
	else if (code == "ushort") type = llvm::Type::getInt16Ty(ir_context);
	else if (code == "uint") type = llvm::Type::getInt32Ty(ir_context);
	else if (code == "ulong") type = llvm::Type::getInt64Ty(ir_context);
	else if (code == "float") type = llvm::Type::getFloatTy(ir_context);
	else if (code == "double") type = llvm::Type::getDoubleTy(ir_context);
	else if (code == "half") type = llvm::Type::getHalfTy(ir_context); //16位半精度浮点
	else if (code == "struct")
	{
		// struct T 两-token 类型：当前 tokens[0] 是 struct，tokens[1] 必须是 struct 名
		// 按用户规则：变量声明处不允许使用「struct T v」（只能写「T v」），但结构体内 self forward 指针字段
		//       「struct Node* next;」在 Node 自身定义内部还必须走这条路（因为 Node 还未登记到 struct 表，走 code-token 分支会查不到）。
		// 所以 ir_type 保留此分支 100% 给 AST_struct 字段 self forward 使用。
		if (tokens.size() < 2 || tokens[1].type != TOKEN_TYPE::code)
			ErrorExit("type definition error: 'struct' requires a struct name", tokens);
		std::string sname = tokens[1].Value;
		llvm::StructType* st = scope::get_struct_type(sname);
		if (!st) {
			st = llvm::StructType::create(ir_context, sname);
			scope::register_struct_type_forward(st, sname, tokens[1]);
		}
		type = st;
		// 吃掉两个 token：struct + NAME
		tokens.erase(tokens.begin()); // struct
		tokens.erase(tokens.begin()); // NAME
	}
	else if (scope::has_struct_type(code))
	{
		// struct 类型当 1-token 类型直接用：Vec v; / Vec* p; / Outer o; 等。
		// 只接受"已在 struct 表"——若 struct 未声明（定义或 forward）则本分支进不去，落到"未定义类型名"错误，符合用户"必须先声明再使用"。
		type = scope::get_struct_type(code);
		// 1 token 类型：下面统一 erase（code!=struct 判断）
	}
	else {
		// 非内建、非 struct、非已知 struct 别名 → 明确报错"未定义类型名"（细化原来的笼统数据类型定义错误）
		std::string msg = "undefined type name: " + code;
		ErrorExit(msg.c_str(), tokens);
	}

	//tokens.erase(tokens.begin()); — 内部分支自己负责 erase（struct 上面已 erase 两次；内建上面没 erase，统一在这里 erase）
	// 对非 struct 情形（上面 1-token 识别），现在才真正 erase type token
	if (type && code != "struct")
		tokens.erase(tokens.begin());

	//如果是指针，则转换指针类型
	if (tokens.size() > 0 && tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "*")
	{
		type = type->getPointerTo();
		//code += "*";
		tokens.erase(tokens.begin());
	}

	return type;
}


//判断类型名是否为无符号整数类型
bool ir_type_unsigned(const std::string& name)
{
	return name == "byte" || name == "ushort" || name == "uint" || name == "ulong";
}


//类型转换
//  is_src_un：源值的符号性（true=无符号整型，false=有符号/非整型/未知）。
//    用于整型→整型扩展时选 ZExt/SExt，及整型→浮点时选 UIToFP/SIToFP。
//    调用方如赋值语句传 right->is_un() 即可；传 nullptr 默认 false（保守有符号语义）。
llvm::Value* ir_type_conver(llvm::Value* value, llvm::Type* to, bool is_src_un)
{
	llvm::Type* src = value->getType();
	if (src == to)
		return value;
	if (src->isIntegerTy())
	{
		if (to->isIntegerTy(1))
		{
			llvm::Value* i0 = llvm::ConstantInt::get(src, 0);
			return ir_builder->CreateICmpNE(value, i0);
		}
		//任意整型宽度间转换（截断/扩展），保证 store 与调用参数的类型合法
		if (to->isIntegerTy())
		{
			// FIX（2026-09-01）：i1 是比较/逻辑运算结果，语义为布尔 0/1，扩宽必须零扩展。
			//   旧代码 CreateIntCast(value, to, true) 统一 sext，把 true(1) 扩成 -1：
			//   printf("%d", 1<2) 输出 -1、(1<2)==1 恒为 0。
			if (src->isIntegerTy(1))
				return ir_builder->CreateZExt(value, to);
			// FIX（2026-09-02，P0#3）：无符号整型扩宽应零扩展而非符号扩展。
			//   例：byte(0xFF) → ushort 应为 0x00FF；旧 IsSigned=true 走 SExt → 0xFFFF，赋值给 uint/ulong 变量时全错。
			return ir_builder->CreateIntCast(value, to, !is_src_un);
		}
		//整型 → 浮点
		// FIX（2026-09-02，P0#3）：旧代码一律 CreateSIToFP，uint 值 0x80000000 赋给 float 会变成 -2147483648。
		if (to->isFloatingPointTy())
			return is_src_un ? ir_builder->CreateUIToFP(value, to) : ir_builder->CreateSIToFP(value, to);
	}
	//浮点 → 浮点（half/float/double 间扩展与截断）/ 浮点 → 整型（含 i1 布尔化）
	if (src->isFloatingPointTy())
	{
		if (to->isFloatingPointTy())
		{
			if (src->getFPMantissaWidth() < to->getFPMantissaWidth())
				return ir_builder->CreateFPExt(value, to);
			return ir_builder->CreateFPTrunc(value, to);
		}
		if (to->isIntegerTy())
		{
			if (to->isIntegerTy(1))
			{
				//浮点布尔化：fcmp one 0.0，非零为真（ordered != NaN 时结果为 false，安全）
				llvm::Value* f0 = llvm::ConstantFP::getZero(src);
				return ir_builder->CreateFCmpONE(value, f0);
			}
			return ir_builder->CreateFPToSI(value, to);
		}
	}

	printf("ERR! type conversion failed!");
	exit(1);
}

////////////////////////////////////////////////////////////////////////////////
//
// 字面量赋值范围检查（方案 B：warning 不改退出码）
//
////////////////////////////////////////////////////////////////////////////////
//
// 支持三种字面量来源：
//   1. 十进制整数 : "1234" / atoll；如果长度>18 视为超 i64 上限用 256-bit APInt 解析（避免 atoll 截断丢失信息，用 StringRef(dec) 直接转 APInt）
//   2. 十六进制整数 : "0xAABBCC..."，按十六进制位宽
//   3. 浮点字面量 : 含 '.' 的字符串，用 APFloat 转，再做 round 的 IEEE 可表示/失精判定
//
// 触发规则（方案 B，十六进制按位宽不按符号）：
//   int 型：按十进制值判断，不在 [SignedMin, SignedMax] 范围（含相等）告警
//   uint 型：按十进制值判断，不在 [0, UnsignedMax] 范围告警
//   十六进制：不管目标符号性，只看位宽——值超过 (1 << N)-1 告警（截断到 N 位）
//   浮点型：转换后比较 APFloat 的 cmp 是否相等（APFloat 是否 "roundsTo 精确"？用 `convert(roundToNearestTiesToEven)` 看 lostInfo 是否 != opOK，或者用 `*this == APFloat(target_semantics, orig_string)` 比较）。
//          APFloat 转换时若 losesInfo/losesPrecision → 失精告警；若变成了 Inf/-Inf/0 vs 原值非 Inf/0 → warning。
//
// 这里不依赖 ast_value.cpp 的 codegen，全部用原始字符串解析，避免与 "先转 i64 再赋值" 的截断路径混在一起。
//

// 解析十进制定点字符串到任意精度 APInt（避免 atoll 截断到 64 bit）。成功返回 true；失败（空串/非十进制字符）返回 false。
// 目前 check_literal_assignment_range 的十进制分支内联了本算法实现；函数保留给将来扩展使用。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4505) // 未引用的本地函数已移除
#endif
static bool parse_decimal_string_to_apint(const std::string& s, llvm::APInt& out)
{
	if (s.empty()) return false;
	unsigned start = 0;
	while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) start++;
	if (start == s.size()) return false;
	unsigned bitwidth = 128;
	for (int retry = 0; retry < 3; retry++, bitwidth *= 2)
	{
		llvm::APInt val(bitwidth, 0, true);
		bool overflow = false;
		for (unsigned i = start; i < s.size(); i++)
		{
			char c = s[i];
			if (c < '0' || c > '9') return false;
			{
				bool ov1 = false;
				llvm::APInt mul10 = val * llvm::APInt(bitwidth, 10, true);
				if (val != 0 && mul10.udiv(llvm::APInt(bitwidth, 10, true)) != val) ov1 = true;
				val = mul10;
				val = val + llvm::APInt(bitwidth, c - '0', true);
				if (ov1) overflow = true;
			}
		}
		if (!overflow && (s.size() - start) <= (unsigned)(bitwidth * 30103 / 100000 + 1)) { out = val; return true; }
	}
	out = llvm::APInt(512, 0); return true;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

void check_literal_assignment_range(
	const std::string& target_type_name,
	llvm::Type* target_llvm_type,
	bool target_un,
	const TOKEN& lit_token)
{
	const std::string& v = lit_token.Value;
	if (v.empty()) return;

	//浮点字面量（含 '.'）
	if (v.find('.') != std::string::npos)
	{
		if (!target_llvm_type->isFloatingPointTy())
		{
			//浮点字面量赋给整型 → 丢失小数部分（会走 FPToSI 截断），warning
			double d = atof(v.c_str());
			char msg[256];
			snprintf(msg, sizeof(msg),
				"floating-point literal %.10g assigned to integer type '%s'; fractional part truncated",
				d, target_type_name.c_str());
			Warning(msg, lit_token);
			return;
		}
		//浮点→浮点：判断原始 double APFloat 与目标语义转换是否失精 / 是否变为 ±inf。
		llvm::APFloat orig_val = llvm::APFloat(atof(v.c_str())); // 原始按 IEEE 双精度读（与 ast_value 一致）
		//目标语义
		const llvm::fltSemantics* to_sem = nullptr;
		switch (target_llvm_type->getTypeID())
		{
		default: return;
		case llvm::Type::HalfTyID:  to_sem = &llvm::APFloat::IEEEhalf(); break;
		case llvm::Type::FloatTyID: to_sem = &llvm::APFloat::IEEEsingle(); break;
		case llvm::Type::DoubleTyID: return; // 字面量本就是 double，赋给 double 不会有精度损失
		}
		llvm::APFloat cpy = orig_val;
		bool loses_info = false;
		llvm::APFloat::opStatus st = cpy.convert(*to_sem, llvm::APFloat::rmNearestTiesToEven, &loses_info);
		if (loses_info || (st & ~llvm::APFloat::opOK))
		{
			double as_dbl = cpy.convertToDouble();
			char msg[320];
			snprintf(msg, sizeof(msg),
				"floating-point literal %s assigned to '%s' loses precision (will become ~%.8g)",
				v.c_str(), target_type_name.c_str(), as_dbl);
			Warning(msg, lit_token);
			return;
		}
		if (cpy.isInfinity())
		{
			char msg[256];
			snprintf(msg, sizeof(msg),
				"floating-point literal %s assigned to '%s' becomes infinity",
				v.c_str(), target_type_name.c_str());
			Warning(msg, lit_token);
			return;
		}
		return;
	}

	//整数字面量：目标必须是整型（float/double/half 允许赋整数字面量，AST 里本来就按整数字面量 i64 生成然后 sitofp 转换，合法，不需要告警）
	if (!target_llvm_type->isIntegerTy())
		return;

	unsigned N = target_llvm_type->getIntegerBitWidth();
	//bool(i1)：非零即 true（ir_type_conver 做 icmp ne 0），不按有符号 -1..0 检查溢出
	if (N == 1) return;
	bool is_hex = (v.size() > 2 && (v[1] == 'x' || v[1] == 'X'));

	//解析字面量到 APInt（足够宽的位宽：512-bit 起步），尽量不用 atoll 避免 64-bit 截断
	llvm::APInt raw;
	if (is_hex)
	{
		std::string hx = v.substr(2);
		unsigned hex_bits = (unsigned)hx.size() * 4;
		unsigned need = hex_bits < 512 ? 512 : hex_bits * 2;
		raw = llvm::APInt(need, 0);
		for (unsigned i = 0; i < hx.size(); i++)
		{
			char c = hx[i];
			unsigned long long digit;
			if (c >= '0' && c <= '9') digit = c - '0';
			else if (c >= 'a' && c <= 'f') digit = 10ull + (c - 'a');
			else if (c >= 'A' && c <= 'F') digit = 10ull + (c - 'A');
			else return; // 非法交给 ast_value::codegen 内 ErrorExit
			raw = raw.shl(4);
			raw = raw | llvm::APInt(need, digit);
		}
	}
	else
	{
		//十进制：最大支持 2^128 范围，39 位数足够 long/ulong/任何整型目标验证
		//parse_decimal_string_to_apint(s, raw)：我们直接复用 ast_value.cpp 里的解析结果太难，重写一份简单按 256-bit APInt 十进制解析。
		raw = llvm::APInt(256, 0);
		for (unsigned i = 0; i < v.size(); i++)
		{
			char c = v[i];
			if (c < '0' || c > '9') return; // 非法交给 ast_value 报错
			raw = raw * llvm::APInt(256, 10ull);
			raw = raw + llvm::APInt(256, (uint64_t)(c - '0'));
		}
	}

	//十六进制：只按位宽检查（不区分 signed/unsigned，与 C 行为一致）
	//值 > 2^N - 1 告警
	if (is_hex)
	{
		//需要的最小位数：countLeadingZeros 拿到高位位宽
		unsigned raw_bits = raw.getBitWidth() - raw.countLeadingZeros();
		if (raw_bits > N)
		{
			//LLVM 23 的 APInt::toString(void -> SmallVectorImpl<char>&, radix, signed)
			llvm::SmallString<64> sstr;
			raw.trunc(N).toString(sstr, 16, false, false, false);
			std::string str_trunc(sstr.begin(), sstr.end());
			char msg[288];
			snprintf(msg, sizeof(msg),
				"hex literal 0x%s truncated to %u-bit type '%s' (will become 0x%s)",
				v.c_str() + 2, N, target_type_name.c_str(), str_trunc.c_str());
			Warning(msg, lit_token);
		}
		return;
	}

	//十进制：严格按目标符号性检查
	if (target_un)
	{
		//目标无符号：范围 [0, 2^N - 1]
		llvm::APInt maxv = llvm::APInt::getMaxValue(N).zext(raw.getBitWidth());
		if (raw.ugt(maxv))
		{
			llvm::SmallString<64> str_after_s, str_max_s;
			raw.trunc(N).toString(str_after_s, 10, false);
			llvm::APInt::getMaxValue(N).toString(str_max_s, 10, false);
			std::string str_after(str_after_s.begin(), str_after_s.end());
			std::string str_max(str_max_s.begin(), str_max_s.end());
			char msg[512];
			snprintf(msg, sizeof(msg),
				"literal %s overflows unsigned type '%s' (range 0..%s); truncated to %s",
				v.c_str(), target_type_name.c_str(), str_max.c_str(), str_after.c_str());
			Warning(msg, lit_token);
		}
	}
	else
	{
		//目标有符号：范围 [ -2^(N-1), 2^(N-1) - 1 ]。
		llvm::APInt signed_max = llvm::APInt::getSignedMaxValue(N).zext(raw.getBitWidth());
		if (raw.ugt(signed_max))
		{
			llvm::SmallString<64> str_after_s, str_max_s, str_min_s;
			llvm::APInt trunc_v = raw.trunc(N);
			trunc_v.sext(raw.getBitWidth()).toStringSigned(str_after_s);
			llvm::APInt::getSignedMaxValue(N).toStringSigned(str_max_s);
			llvm::APInt::getSignedMinValue(N).toStringSigned(str_min_s);
			std::string str_after(str_after_s.begin(), str_after_s.end());
			std::string str_max(str_max_s.begin(), str_max_s.end());
			std::string str_min(str_min_s.begin(), str_min_s.end());
			char msg[576];
			snprintf(msg, sizeof(msg),
				"literal %s overflows signed type '%s' (range %s..%s); truncated to %s",
				v.c_str(), target_type_name.c_str(), str_min.c_str(), str_max.c_str(), str_after.c_str());
			Warning(msg, lit_token);
		}
	}
}




////查找变量
////	name	变量名称
////	var_list	变量列表
//VARINFO ir_var(std::string name, std::vector<VAR_LIST> var_list, TOKEN token)
//{
//	//if (name == "str")
//	//	printf("aa");
//	// 
//	//从下往上查找变量定义
//	while (!var_list.empty())
//	{
//		VAR_LIST vlist = var_list.back();
//		var_list.pop_back();
//
//		if (vlist.info.find(name) == vlist.info.end())
//		{
//			continue;
//		}
//		VAR_INFO vinfo = vlist.info[name];
//		if (vinfo.value)
//		{
//			return vinfo;
//		}
//		//如果当前是function定义，则下一步转到全局变量 20240411 shanmin
//		if (vlist.zone == "function")
//			var_list.resize(1);
//	}
//	//
//	std::vector<TOKEN> tmp;
//	tmp.push_back(token);
//	ErrorExit("ERROR: 变量不存在", tmp);
//}
////读取变量值
//llvm::Value* ir_var_load(VARINFO& var_info)
//{
//	return ir_builder->CreateLoad(var_info.type, var_info.value);
//}


////////////////////////////////////////////////////////////////////////////////
//
//  函数重载基础设施 —— Step 1：TCType + 纯工具函数
//
////////////////////////////////////////////////////////////////////////////////

bool tctype_equal(const TCType& a, const TCType& b)
{
	// 两者 source_name 都非空 → 优先比 source_name（区分 int vs uint）
	if (!a.source_name.empty() && !b.source_name.empty()) {
		if (a.source_name != b.source_name) return false;
		return a.un == b.un;
	}
	// 至少一方 source_name 为空 → 靠 LLVM Type 指针 + un
	if (a.ty != b.ty) return false;
	return a.un == b.un;
}

std::string tctype_str(const TCType& t)
{
	if (!t.source_name.empty()) return t.source_name;
	if (!t.ty) return "<unknown>";
	std::string s;
	llvm::raw_string_ostream os(s);
	t.ty->print(os);
	os.flush();
	if (t.un) s += " [unsigned]";
	return s;
}

std::string overload_bucket_key(const std::string& raw_name, const std::string& class_name, const std::string& mod_prefix)
{
	if (!mod_prefix.empty())
		return mod_prefix + "##" + (class_name.empty() ? raw_name : (class_name + "##" + raw_name));
	if (!class_name.empty())
		return class_name + "##" + raw_name;
	return raw_name;
}


////////////////////////////////////////////////////////////////////////////////
//
//  函数重载基础设施 —— Step 2：全局注册表 + mangling + 决议
//
////////////////////////////////////////////////////////////////////////////////

// 全局重载 bucket（key = overload_bucket_key）
static std::map<std::string, std::vector<OverloadEntry>> g_overload_map;

// 源码级名字的 C 符号安全化：非 [a-zA-Z0-9_] 字符统一转义，避免 mangled 名里出现非法/分隔冲突字符
static std::string safe_name_encode(const std::string& s)
{
	std::string r;
	r.reserve(s.size() + 4);
	for (char c : s) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
			r += c;
		} else if (c == '*') {
			r += "_P_";
		} else if (c == '.') {
			r += "_d_";
		} else {
			char buf[16];
			snprintf(buf, sizeof(buf), "_X%02X_", (unsigned char)c);
			r += buf;
		}
	}
	return r;
}

// TCType → typecode（用于 mangling 签名编码，需唯一且可逆无歧义）
static std::string tctype_code(const TCType& t)
{
	// 优先用 source_name 精确编码（区分 int vs uint、同名 ptr/struct）
	if (!t.source_name.empty()) {
		if (t.source_name == "void")   return "vi";
		if (t.source_name == "bool")   return "b1";
		if (t.source_name == "char")   return "i8c";
		if (t.source_name == "short")  return "i16s";
		if (t.source_name == "int")    return "i32i";
		if (t.source_name == "long")   return "i64l";
		if (t.source_name == "byte")   return "u8b";
		if (t.source_name == "ushort") return "u16us";
		if (t.source_name == "uint")   return "u32ui";
		if (t.source_name == "ulong")  return "u64ul";
		if (t.source_name == "half")   return "f16h";
		if (t.source_name == "float")  return "f32f";
		if (t.source_name == "double") return "f64d";
		// 指针：尾缀 "*" → 递归编码 "p<elem>"
		if (!t.source_name.empty() && t.source_name.back() == '*') {
			TCType elem;
			elem.source_name = t.source_name.substr(0, t.source_name.size() - 1);
			elem.ty = nullptr;
			elem.un = t.un;
			return std::string("p") + tctype_code(elem);
		}
		// 命名 struct/class（不含 * 的其他名字）
		if (t.ty && t.ty->isStructTy()) {
			return std::string("s") + safe_name_encode(t.source_name);
		}
		// 兜底：unknown
		return std::string("n") + safe_name_encode(t.source_name);
	}
	// source_name 空：用 LLVM Type* 分类（粗粒度，不区分 int vs uint——调用方应尽量避免这种情况）
	if (!t.ty) return "t0";
	if (t.ty->isVoidTy()) return "vi";
	if (t.ty->isIntegerTy(1)) return "b1";
	if (t.ty->isIntegerTy()) {
		unsigned w = t.ty->getIntegerBitWidth();
		const char* prefix = t.un ? "u" : "i";
		switch (w) {
		case 8:  return std::string(prefix) + (t.un ? "8b" : "8c");
		case 16: return std::string(prefix) + (t.un ? "16us" : "16s");
		case 32: return std::string(prefix) + (t.un ? "32ui" : "32i");
		case 64: return std::string(prefix) + (t.un ? "64ul" : "64l");
		default: {
			char buf[32];
			snprintf(buf, sizeof(buf), "%s%du", prefix, w);
			return buf;
		}
		}
	}
	if (t.ty->isHalfTy()) return "f16h";
	if (t.ty->isFloatTy()) return "f32f";
	if (t.ty->isDoubleTy()) return "f64d";
	if (t.ty->isPointerTy()) return "pvi"; // opaque pointer 拿不到 pointee，粗编码兜底；调用方应已填 source_name 所以一般不会到这里
	if (t.ty->isStructTy()) {
		llvm::StructType* st = llvm::cast<llvm::StructType>(t.ty);
		if (st->hasName()) return std::string("s") + safe_name_encode(st->getName().str());
		return "sanon";
	}
	return std::string("t") + std::to_string((unsigned)t.ty->getTypeID());
}

std::string overload_mangle(const std::string& raw_name, const std::vector<TCType>& params, bool is_vararg, const std::string& class_name, bool is_ctor)
{
	// 构造函数：用户写的 ClassName 实际统一换成 "__init__"（AST_new 构造决议 bucket_key = ClassName##__init__ 时对齐）
	const std::string& eff_name = is_ctor ? std::string("__init__") : raw_name;

	std::string parts = "__co_";
	if (!class_name.empty()) {
		parts += "c" + safe_name_encode(class_name) + "_";
	}
	parts += safe_name_encode(eff_name);
	parts += "__";
	parts += std::to_string(params.size());
	for (const auto& p : params) {
		parts += "_";
		parts += tctype_code(p);
	}
	if (is_vararg) parts += "_vararg";

	// 非顶层模式：再加一层 ir_mangle_symbol 的模块前缀（保证跨模块同名函数不冲突）
	const std::string& mod = ir_get_current_module_name();
	if (!mod.empty()) {
		return ir_mangle_symbol(parts);
	}
	return parts;
}

void overload_register(const OverloadEntry& entry, const TOKEN& diag_tok)
{
	std::string bkey = overload_bucket_key(entry.raw_name, entry.class_name);
	auto& bucket = g_overload_map[bkey];
	// 同签名重定义校验：param 全相等 + ret 相等 + is_vararg 同 + class_name 同（class_name 已由 bucket_key 区分）
	for (const auto& e : bucket) {
		if (e.params.size() != entry.params.size()) continue;
		if (e.is_vararg != entry.is_vararg) continue;
		if (!tctype_equal(e.ret, entry.ret)) continue;
		bool eq = true;
		for (size_t i = 0; i < e.params.size(); i++) {
			if (!tctype_equal(e.params[i], entry.params[i])) { eq = false; break; }
		}
		if (eq) {
			std::string msg = "function redefinition with same signature: ";
			msg += tctype_str(entry.ret) + " " + entry.raw_name + "(";
			for (size_t i = 0; i < entry.params.size(); i++) {
				if (i) msg += ", ";
				msg += tctype_str(entry.params[i]);
			}
			if (entry.is_vararg) {
				if (!entry.params.empty()) msg += ", ";
				msg += "...";
			}
			msg += ")";
			ErrorExit(msg.c_str(), diag_tok);
		}
	}
	bucket.push_back(entry);
}

// Function::Create 后同步最后一条目的 mangled 名
//   场景：重载构造函数 Counter() 与 Counter(int) 都以 "Counter.__init__" 名注册+创建，
//         LLVM 自动把第二个改名为 "Counter.__init__.1"。重载桶里仍存旧名 → overload_resolve
//         返回的名字与 ir_module->getFunction 实际名不一致 → 调到错误函数。
//   修复：Function::Create 后取 LLVM 实际名回写重载桶最后一条目。
void overload_sync_last_mangled(const std::string& raw_name, const std::string& class_name,
                                const std::string& new_mangled)
{
	std::string bkey = overload_bucket_key(raw_name, class_name);
	auto it = g_overload_map.find(bkey);
	if (it == g_overload_map.end() || it->second.empty()) return;
	it->second.back().mangled = new_mangled;
}

// 隐式转换 src→dst 的 cost：返回 -1 = 不允许隐式；≥ 0 = 成本（越小越优）
static int implicit_convert_cost(const TCType& src, const TCType& dst)
{
	if (tctype_equal(src, dst)) return 0;

	const bool src_int = src.ty && src.ty->isIntegerTy();
	const bool dst_int = dst.ty && dst.ty->isIntegerTy();
	const bool src_fp  = src.ty && src.ty->isFloatingPointTy();
	const bool dst_fp  = dst.ty && dst.ty->isFloatingPointTy();

	// bool(i1) ↔ i32：C 的 _Bool 整型提升，成本 1
	if (src_int && dst_int) {
		if (src.ty->isIntegerTy(1) && dst.ty->isIntegerTy(32)) return 1;
		if (dst.ty->isIntegerTy(1) && src.ty->isIntegerTy(32)) return 1;
	}

	// 整型→整型（i1 已单独处理，这里不重复）
	if (src_int && dst_int && !src.ty->isIntegerTy(1) && !dst.ty->isIntegerTy(1)) {
		unsigned sw = src.ty->getIntegerBitWidth();
		unsigned dw = dst.ty->getIntegerBitWidth();
		if (sw == dw) return -1;                // 同宽不同符号或完全不同 source_name（int/uint）：不允许隐式
		if (sw < dw) return (src.un == dst.un) ? 1 : 2;
		return -1; // 宽→窄收缩不允许隐式
	}

	// 浮点→浮点（窄→宽 cost1；等宽 type_equal 前面过；宽→窄收缩不允许）
	if (src_fp && dst_fp) {
		unsigned sm = src.ty->getFPMantissaWidth();
		unsigned dm = dst.ty->getFPMantissaWidth();
		if (sm < dm) return 1;
		return -1;
	}

	// 整型→浮点：允许（OQ-2），成本 3（比普通提升优先级低，减少与同宽度整型重载的歧义）
	if (src_int && dst_fp) return 3;

	// 浮点→整型：不允许隐式收缩（OQ-1 对齐，float→int 是收缩类不允许）
	if (src_fp && dst_int) return -1;

	// 指针/struct：tctype_equal 前已处理；不同类型不允许隐式
	return -1;
}

std::string overload_resolve(const std::string& bucket_key, const std::vector<TCType>& arg_types, const TOKEN& diag_tok, bool* out_found)
{
	auto it = g_overload_map.find(bucket_key);
	if (it == g_overload_map.end()) {
		if (out_found) *out_found = false;
		return std::string();
	}
	if (out_found) *out_found = true;

	const auto& bucket = it->second;

	// 收集可行候选（累计 cost + bucket 下标）
	struct Candidate { int cost; size_t index; };
	std::vector<Candidate> feas;
	for (size_t ei = 0; ei < bucket.size(); ei++) {
		const OverloadEntry& e = bucket[ei];
		size_t Nfix = e.params.size();
		size_t Narg = arg_types.size();
		if (!e.is_vararg) {
			if (Nfix != Narg) continue;
		} else {
			if (Narg < Nfix) continue;
		}
		int total = 0;
		bool ok = true;
		for (size_t i = 0; i < Nfix; i++) {
			int c = implicit_convert_cost(arg_types[i], e.params[i]);
			if (c < 0) { ok = false; break; }
			total += c;
		}
		if (!ok) continue;
		feas.push_back({ total, ei });
	}

	// 拿 bucket_key 的显示名（最后一段 "##" 后的是函数名）
	auto display_of = [](const std::string& k) -> std::string {
		size_t p = k.rfind("##");
		if (p == std::string::npos) return k;
		return k.substr(p + 2) + std::string(" (in ") + k.substr(0, p) + ")";
	};

	if (feas.empty()) {
		std::string msg = "no matching function call for ";
		msg += display_of(bucket_key);
		msg += " with argument types (";
		for (size_t i = 0; i < arg_types.size(); i++) {
			if (i) msg += ", ";
			msg += tctype_str(arg_types[i]);
		}
		msg += ")";
		msg += "\noverload candidates:";
		msg += overload_candidates_str(bucket_key);
		ErrorExit(msg.c_str(), diag_tok);
	}

	// 取最小 cost，筛选
	int mincost = feas[0].cost;
	for (auto& f : feas) if (f.cost < mincost) mincost = f.cost;
	std::vector<size_t> best;
	for (auto& f : feas) if (f.cost == mincost) best.push_back(f.index);

	if (best.size() == 1) {
		return bucket[best[0]].mangled;
	}

	// 歧义
	std::string msg = "ambiguous call to ";
	msg += display_of(bucket_key);
	msg += ": multiple best-match overload candidates (tie cost = " + std::to_string(mincost) + ")\ncandidates:";
	for (size_t idx : best) {
		const OverloadEntry& e = bucket[idx];
		msg += "\n  ";
		msg += tctype_str(e.ret) + " ";
		if (!e.class_name.empty()) msg += e.class_name + "::";
		msg += e.raw_name;
		msg += "(";
		for (size_t i = 0; i < e.params.size(); i++) {
			if (i) msg += ", ";
			msg += tctype_str(e.params[i]);
		}
		if (e.is_vararg) {
			if (!e.params.empty()) msg += ", ";
			msg += "...";
		}
		msg += ")";
	}
	ErrorExit(msg.c_str(), diag_tok);
	return std::string(); // unreachable
}

std::string overload_candidates_str(const std::string& bucket_key)
{
	auto it = g_overload_map.find(bucket_key);
	if (it == g_overload_map.end()) return "(none)";
	std::string s;
	for (const OverloadEntry& e : it->second) {
		s += "\n  ";
		s += tctype_str(e.ret);
		s += " ";
		if (!e.class_name.empty()) s += e.class_name + "::";
		s += e.raw_name;
		s += "(";
		for (size_t i = 0; i < e.params.size(); i++) {
			if (i) s += ", ";
			s += tctype_str(e.params[i]);
		}
		if (e.is_vararg) {
			if (!e.params.empty()) s += ", ";
			s += "...";
		}
		s += ")";
	}
	return s;
}

void overload_clear()
{
	g_overload_map.clear();
}

// —— import / scope 快照（RAII save/restore 及 merge）——
struct OverloadSnapshot
{
	std::map<std::string, std::vector<OverloadEntry>> map_copy;
};

void* overload_save_state()
{
	OverloadSnapshot* s = new OverloadSnapshot();
	s->map_copy = g_overload_map;
	return (void*)s;
}

void overload_restore_state(void* state)
{
	if (!state) return;
	OverloadSnapshot* s = (OverloadSnapshot*)state;
	std::swap(g_overload_map, s->map_copy);
	delete s;
}

size_t overload_merge_from_snapshot(void* snap_handle, const TOKEN& /*diag_tok*/)
{
	if (!snap_handle) return 0;
	OverloadSnapshot* s = (OverloadSnapshot*)snap_handle;
	size_t added = 0;
	for (const auto& kv : s->map_copy) {
		auto& dst_bucket = g_overload_map[kv.first];
		for (const OverloadEntry& entry : kv.second) {
			if (!entry.is_public_entry) continue;
			// 同签名跳过（幂等，不报错）
			bool dup = false;
			for (const OverloadEntry& e : dst_bucket) {
				if (e.params.size() != entry.params.size()) continue;
				if (e.is_vararg != entry.is_vararg) continue;
				if (!tctype_equal(e.ret, entry.ret)) continue;
				bool eq = true;
				for (size_t i = 0; i < e.params.size(); i++) {
					if (!tctype_equal(e.params[i], entry.params[i])) { eq = false; break; }
				}
				if (eq) { dup = true; break; }
			}
			if (dup) continue;
			dst_bucket.push_back(entry);
			added++;
		}
	}
	return added;
}

void overload_free_snapshot(void* snap_handle)
{
	if (!snap_handle) return;
	OverloadSnapshot* s = (OverloadSnapshot*)snap_handle;
	delete s;
}


//	THE END