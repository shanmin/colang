// ============================================================
// codegen.cpp — LLVM IR 代码生成（P7.4）
//
// 支持范围：
//   函数声明/定义/调用(直接+间接)/变参、全局变量(static/extern/初始化器)、
//   局部变量 alloca/load/store、局部 static、数组 alloca(ArrayType)+GEP+退化、
//   struct/union(布局/成员访问/赋值 memcpy)、enum、typedef、
//   二元运算(算术/位/比较/FP/指针算术)、短路逻辑、一元(-+!~*&sizeof++--),
//   赋值(=/op=/ptr/struct)、三元/逗号/cast(FP↔Int/Ptr↔Int)、
//   if/else/while/do/for/break/continue/switch/goto/label、
//   函数指针(声明/赋值/间接调用)、void*泛型、复合初始化器、
//   字符串字面量、整数/char/float常量、main(argc,argv)。
// 后续 P3 收尾：多文件链接、C89 剩余边界确认。
//
// LLVM 23 注意：
//   - 不透明指针（PointerType::getUnqual）；Type::getInt8PtrTy 已废弃。
//   - CreateGlobalStringPtr → CreateGlobalString。
//   - Module::setTargetTriple 收 llvm::Triple。
// ============================================================

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include "colang/codegen.h"
#include "colang/type_resolver.h"

namespace colang
{

namespace
{

// 解码 C 字符串字面量（含首尾引号 + 转义序列）为字节序列（含末尾 '\0'）。
// 转义解码单一真值源在 common.h::co_decode_string（不含 null），此处补 C 终止符。
std::string decode_string(const std::string &raw)
{
	std::string s = co_decode_string(raw);
	s += '\0'; // C 字符串终止
	return s;
}

// ============================================================
// 内部 CodeGen 结构
// ============================================================
struct CodeGenImpl
{
	llvm::LLVMContext ctx;
	std::unique_ptr<llvm::Module> module;
	llvm::IRBuilder<> builder;
	std::vector<Error> &errs;
	// 局部符号表：变量名 -> alloca/GlobalVariable 指针
	std::unordered_map<std::string, llvm::Value*> vars_;
	// 全局符号表
	std::unordered_map<std::string, llvm::GlobalVariable*> globals_;

	// break/continue 目标栈
	struct LoopContext {
		llvm::BasicBlock *break_target;
		llvm::BasicBlock *continue_target; // nullptr for switch
	};
	std::vector<LoopContext> loop_stack_;

	// goto/label
	std::unordered_map<std::string, llvm::BasicBlock*> label_blocks_;
	// switch case/default 标签→BB（P172：case 视作 label，支持嵌在循环体内的 case，如 Tom Duff）
	std::unordered_map<const Stmt *, llvm::BasicBlock *> case_label_bb_;
	struct PendingGoto { std::string label; llvm::BasicBlock *from_bb; };
	std::vector<PendingGoto> pending_gotos_;

	// struct/union/enum
	std::unordered_map<std::string, llvm::StructType*> struct_types_;
	std::unordered_map<std::string, const StructDef*> struct_defs_;
	// 位域访问信息：key = "tag.member_name"
	struct BitfieldAccess {
		unsigned storage_idx;   // LLVM struct 字段索引（存储单元）
		unsigned bit_offset;    // 存储单元内起始位
		unsigned bit_width;     // 位域宽度
		bool is_signed;         // 是否有符号
		unsigned unit_bits;     // 存储单元总位宽
	};
	std::unordered_map<std::string, BitfieldAccess> bitfield_access_;
	// 统一字段索引映射：key = "tag.member_name", value = LLVM struct field index
	std::unordered_map<std::string, unsigned> field_index_map_;
	// struct/union 有效对齐（含 _Alignas 成员/类型强制对齐）：tag -> 字节；仅手工布局/含对齐者登记
	std::unordered_map<std::string, unsigned> struct_align_;
	// 手工布局（含 _Alignas padding）时：tag -> [C 成员序号 -> LLVM 字段索引]，供位置初始化器映射
	std::unordered_map<std::string, std::vector<unsigned>> struct_pos_map_;
	// 变量名 -> CTypePtr（追踪结构体等复杂类型）
	std::unordered_map<std::string, CTypePtr> var_types_;
	// 全局变量名 -> CTypePtr
	std::unordered_map<std::string, CTypePtr> global_types_;
	// 函数名 -> 返回类型 CTypePtr（_Generic 类型推断用）
	std::unordered_map<std::string, CTypePtr> func_types_;
	// 函数名 -> 完整函数类型 CTypePtr（Function{base=返回, params, variadic}），供 _Generic 函数名衰减为函数指针时匹配（00219 foo→fptr）
	std::unordered_map<std::string, CTypePtr> func_cftypes_;
	// 当前函数名（用于局部 static 命名）
	std::string cur_func_name_;
	// Annex K set_constraint_handler_s 的模块级 handler 存储（缓存以免重复建同名全局）
	llvm::GlobalVariable *annexk_handler_gv_ = nullptr;
	// 文件作用域复合字面量物化的匿名静态全局对象命名计数（00149/00150）
	int anon_ctr_ = 0;

	CodeGenImpl(const std::string &mod_name, std::vector<Error> &errors)
		: builder(ctx), errs(errors)
	{
		module = std::make_unique<llvm::Module>(mod_name, ctx);
		llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
		module->setTargetTriple(triple);
		// 设置 DataLayout（x86_64 Windows，指针 8B，int 4B）
		std::string dl_str;
		if (triple.isArch64Bit() && triple.isOSWindows())
			dl_str = "e-m:w-p270:32:32-p271:32:32-p272:32:32-i64:64-f80:32-n8:16:32:64-S128";
		else if (triple.isArch64Bit())
			dl_str = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128";
		else
			dl_str = "e-m:e-p:32:32-i64:64-f80:32-n8:16:32-a:0:32-S128";
		module->setDataLayout(dl_str);
	}

	// —— 类型转换 ——
	llvm::Type *to_llvm_type(const CTypePtr &t)
	{
		if (!t)
		{
			return llvm::Type::getVoidTy(ctx);
		}
		switch (t->kind)
		{
		case TypeKind::Void:
			return llvm::Type::getVoidTy(ctx);
		case TypeKind::Bool:
			return llvm::Type::getInt1Ty(ctx);  // _Bool: LLVM i1, store size = 1 byte
		case TypeKind::Char:
			return llvm::Type::getInt8Ty(ctx);
		case TypeKind::Char8:
			return llvm::Type::getInt8Ty(ctx);   // char8_t: i8（1 字节恒无符号）
		case TypeKind::Char16:
			return llvm::Type::getInt16Ty(ctx);  // char16_t: i16（2 字节恒无符号）
		case TypeKind::Char32:
			return llvm::Type::getInt32Ty(ctx);  // char32_t: i32（4 字节恒无符号）
		case TypeKind::WChar:
			return llvm::Type::getInt16Ty(ctx);  // wchar_t: i16（2 字节，对齐宿主 CRT unsigned short）
		case TypeKind::Short:
			return llvm::Type::getInt16Ty(ctx);  // short: i16（2 字节，signed/unsigned 由 is_unsigned 区分）
		case TypeKind::Int:
		case TypeKind::Long:
			return llvm::Type::getInt32Ty(ctx); // LLP64: int=i32, long=i32
		case TypeKind::LongLong:
			return llvm::Type::getInt64Ty(ctx); // long long = i64
		case TypeKind::Float:
			return llvm::Type::getFloatTy(ctx);
		case TypeKind::Double:
			return llvm::Type::getDoubleTy(ctx);
		case TypeKind::Pointer:
		case TypeKind::Array:
			return llvm::PointerType::getUnqual(ctx);
		case TypeKind::Function:
			// 函数类型作参数时按 pointer 处理（函数指针）
			return llvm::PointerType::getUnqual(ctx);
		case TypeKind::Enum:
			return llvm::Type::getInt32Ty(ctx);
		case TypeKind::BitInt:
			return llvm::IntegerType::get(ctx, t->bit_width);
		case TypeKind::Struct:
		case TypeKind::Union:
			// struct/union 在值上下文中按 pointer 处理（参数传递/返回）
			return llvm::PointerType::getUnqual(ctx);
		default:
			return llvm::Type::getInt32Ty(ctx);
		}
	}

	// 获取 vars_ 中存储值的 LLVM 类型（ AllocaInst / GlobalVariable 统一处理）
	llvm::Type *get_var_llvm_type(llvm::Value *v)
	{
		if (auto *al = llvm::dyn_cast<llvm::AllocaInst>(v))
			return al->getAllocatedType();
		if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(v))
			return gv->getValueType();
		return llvm::Type::getInt32Ty(ctx);
	}

	// 用于 alloca 的实际类型（数组保留 ArrayType，struct 保留 StructType）
	llvm::Type *to_llvm_alloca_type(const CTypePtr &t)
	{
		if (!t) return llvm::Type::getInt32Ty(ctx);
		if (t->kind == TypeKind::Array)
		{
			llvm::Type *elem = to_llvm_alloca_type(t->base);
			return llvm::ArrayType::get(elem, t->array_size);
		}
		if (t->kind == TypeKind::Struct || t->kind == TypeKind::Union)
			return get_struct_type(t->tag);
		return to_llvm_type(t);
	}

	// —— 对齐计算（C23 _Alignas 全链路）——
	// struct/union tag -> LLVM StructType 反查（供位置初始化器映射）
	std::string tag_for_struct(llvm::StructType *sty)
	{
		for (auto &[t, st] : struct_types_)
			if (st == sty) return t;
		return {};
	}

	// 类型的有效对齐：自然对齐（LLVM DataLayout）与 _Alignas（CType::align / struct_align_）取 max
	unsigned type_effective_align(const CTypePtr &t)
	{
		if (!t) return 4;
		const auto &DL = module->getDataLayout();
		if (t->kind == TypeKind::Array)
		{
			unsigned a = type_effective_align(t->base);
			if (t->align > a) a = t->align;
			return a;
		}
		if (t->kind == TypeKind::Struct || t->kind == TypeKind::Union)
		{
			llvm::Type *lt = get_struct_type(t->tag);
			unsigned a = static_cast<unsigned>(DL.getABITypeAlign(lt).value());
			auto sa = struct_align_.find(t->tag);
			if (sa != struct_align_.end() && sa->second > a) a = sa->second;
			if (t->align > a) a = t->align;
			return a;
		}
		unsigned a = static_cast<unsigned>(DL.getABITypeAlign(to_llvm_type(t)).value());
		if (t->align > a) a = t->align;
		return a;
	}

	// struct/union 成员的有效对齐：max(类型有效对齐, 成员 _Alignas)
	unsigned member_effective_align(const StructMember &m)
	{
		unsigned a = m.type ? type_effective_align(m.type) : 1;
		if (m.align > a) a = m.align;
		return a < 1 ? 1 : a;
	}

	// 对象落位对齐：max(自然对齐(llvm 类型), 类型有效对齐, 声明 _Alignas)
	llvm::MaybeAlign obj_align(const CTypePtr &ct, llvm::Type *ty, unsigned decl_align)
	{
		unsigned a = decl_align;
		unsigned te = ct ? type_effective_align(ct) : 0;
		if (te > a) a = te;
		if (ty)
		{
			unsigned nat = static_cast<unsigned>(module->getDataLayout().getABITypeAlign(ty).value());
			if (nat > a) a = nat;
		}
		if (a == 0) return llvm::MaybeAlign();
		return llvm::MaybeAlign(a);
	}

	// 位置初始化器：C 成员序号 pos → LLVM 字段索引（手工布局含 padding 时查表；否则 = pos）
	std::size_t struct_member_ll_index(llvm::StructType *sty, std::size_t pos)
	{
		std::string tag = tag_for_struct(sty);
		if (!tag.empty())
		{
			auto it = struct_pos_map_.find(tag);
			if (it != struct_pos_map_.end() && pos < it->second.size())
				return it->second[pos];
		}
		return pos;
	}

	// 获取/创建 LLVM StructType
	llvm::StructType *get_struct_type(const std::string &tag)
	{
		auto it = struct_types_.find(tag);
		if (it != struct_types_.end()) return it->second;
		// 查找定义
		auto def_it = struct_defs_.find(tag);
		if (def_it == struct_defs_.end())
		{
			// 未定义，返回空 struct
			auto *st = llvm::StructType::create(ctx, "struct." + tag);
			struct_types_[tag] = st;
			return st;
		}
		const StructDef *sd = def_it->second;
		// 先创建 opaque（支持自引用）
		auto *st = llvm::StructType::create(ctx, "struct." + tag);
		struct_types_[tag] = st;
		const auto &DL = module->getDataLayout();

		if (sd->is_union)
		{
			// union: [max store size x i8]；对齐取成员有效对齐最大值
			uint64_t max_sz = 0;
			unsigned al = 1;
			for (const auto &m : sd->members)
			{
				llvm::Type *mt = to_llvm_alloca_type(m.type);
				uint64_t sz = DL.getTypeStoreSize(mt);
				if (sz > max_sz) max_sz = sz;
				unsigned ea = member_effective_align(m);
				if (ea > al) al = ea;
			}
			if (max_sz == 0) max_sz = 1;
			max_sz = ((max_sz + al - 1) / al) * al; // 尾部补到有效对齐（数组步长/对象对齐一致）
			struct_align_[tag] = al;
			st->setBody(llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), max_sz), false);
			return st;
		}

		// 判断是否需手工 padding 布局（含 _Alignas 且无位域）
		bool has_bitfield = false;
		bool needs_manual = false;
		for (const auto &m : sd->members)
		{
			if (m.bit_width >= 0) { has_bitfield = true; continue; }
			if (m.align > 0) needs_manual = true;
			if (m.type && m.type->align > 0) needs_manual = true; // 数组/typedef 携带 _Alignas
			if (m.type && (m.type->kind == TypeKind::Struct || m.type->kind == TypeKind::Union))
			{
				llvm::Type *mt = to_llvm_alloca_type(m.type); // 触发嵌套 get_struct_type，填 struct_align_
				unsigned nat = static_cast<unsigned>(DL.getABITypeAlign(mt).value());
				auto sa = struct_align_.find(m.type->tag);
				if (sa != struct_align_.end() && sa->second > nat) needs_manual = true;
			}
		}

		if (needs_manual && !has_bitfield)
		{
			// ===== 手工逐字节布局：对齐满足处插入 [gap x i8]，尾部补到 struct 对齐 =====
			std::vector<llvm::Type*> fields;
			std::vector<unsigned> pos_map;
			uint64_t off = 0;
			unsigned sal = 1;
			for (const auto &m : sd->members)
			{
				llvm::Type *mt = to_llvm_alloca_type(m.type);
				unsigned ea = member_effective_align(m);
				if (ea < 1) ea = 1;
				if (ea > sal) sal = ea;
				uint64_t aligned = ((off + ea - 1) / ea) * ea;
				if (aligned > off)
				{
					fields.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), aligned - off));
					off = aligned;
				}
				unsigned fidx = static_cast<unsigned>(fields.size());
				fields.push_back(mt);
				off += DL.getTypeStoreSize(mt);
				if (!m.name.empty())
					field_index_map_[tag + "." + m.name] = fidx;
				pos_map.push_back(fidx);
			}
			uint64_t total = ((off + sal - 1) / sal) * sal;
			if (total > off)
				fields.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), total - off));
			st->setBody(fields, false);
			struct_align_[tag] = sal;
			struct_pos_map_[tag] = std::move(pos_map);
			return st;
		}

		// ===== 自动布局路径（普通 struct / 含位域 / alignas+位域回退）=====
		{
			std::vector<llvm::Type*> fields;
			unsigned bf_idx = 0;  // 当前成员在 fields 中的索引
			unsigned cur_offset = 0; // 当前存储单元内已用位数
			unsigned cur_unit_bits = 0; // 当前存储单元位宽
			bool in_storage = false; // 是否在收集位域存储单元

			for (std::size_t i = 0; i < sd->members.size(); ++i)
			{
				const auto &m = sd->members[i];
				if (m.bit_width >= 0)
				{
					// 位域成员
					unsigned unit_bits = get_bitfield_unit_bits(m.type);
					if (!in_storage || cur_unit_bits != unit_bits)
					{
						// 开新存储单元
						bf_idx = static_cast<unsigned>(fields.size());
						fields.push_back(get_int_ty_for_bits(unit_bits));
						cur_offset = 0;
						cur_unit_bits = unit_bits;
						in_storage = true;
					}
					// 检查是否超宽，需要新单元
					if (cur_offset + static_cast<unsigned>(m.bit_width) > cur_unit_bits)
					{
						bf_idx = static_cast<unsigned>(fields.size());
						fields.push_back(get_int_ty_for_bits(unit_bits));
						cur_offset = 0;
						cur_unit_bits = unit_bits;
					}
					// 记录位域访问信息
					if (!m.name.empty())
					{
						BitfieldAccess ba;
						ba.storage_idx = bf_idx;
						ba.bit_offset = cur_offset;
						ba.bit_width = static_cast<unsigned>(m.bit_width);
						ba.is_signed = m.type && (m.type->kind == TypeKind::Int || m.type->kind == TypeKind::Short) && !m.type->is_unsigned;
						ba.unit_bits = cur_unit_bits;
						bitfield_access_[tag + "." + m.name] = ba;
					}
					cur_offset += static_cast<unsigned>(m.bit_width);
				}
				else
				{
					// 普通成员: 结束当前位域段
					in_storage = false;
					cur_offset = 0;
					cur_unit_bits = 0;
					unsigned fidx = static_cast<unsigned>(fields.size());
					fields.push_back(to_llvm_alloca_type(m.type));
					if (!m.name.empty())
						field_index_map_[tag + "." + m.name] = fidx;
				}
			}
			st->setBody(fields, false);
			// struct 对齐：LLVM 自然对齐与成员有效对齐取 max（覆盖 alignas+位域回退情形）
			unsigned sal = static_cast<unsigned>(DL.getABITypeAlign(st).value());
			for (const auto &m : sd->members)
			{
				if (m.bit_width >= 0) continue;
				unsigned ea = member_effective_align(m);
				if (ea > sal) sal = ea;
			}
			struct_align_[tag] = sal;
		}
		return st;
	}

	// get_bitfield_unit_bits：P140 起移至共享解析器 type_resolver.h（单一真值源）

	llvm::Type *get_int_ty_for_bits(unsigned bits)
	{
		if (bits <= 8) return llvm::Type::getInt8Ty(ctx);
		if (bits <= 16) return llvm::Type::getInt16Ty(ctx);
		if (bits <= 32) return llvm::Type::getInt32Ty(ctx);
		return llvm::Type::getInt64Ty(ctx);
	}

	// stdbit_parse/stdbit_is_value_op/stdbit_unsigned_ty：P140 起移至 type_resolver.h

	llvm::FunctionType *to_func_type(const FuncDecl &fn)
	{
		llvm::Type *ret_ty = to_llvm_type(fn.return_type);
		std::vector<llvm::Type *> param_tys;
		for (const auto &p : fn.params)
		{
			// 数组参数退化为指针
			param_tys.push_back(to_llvm_type(p.type));
		}
		return llvm::FunctionType::get(ret_ty, param_tys, fn.is_variadic);
	}

	// —— 翻译单元 ——
	void gen_tu(const TranslationUnit &tu)
	{
		// 预注册 struct/union 定义（供 get_struct_type 查询）
		for (const auto &sd : tu.structs)
		{
			if (!sd.tag.empty() && !sd.members.empty())
				struct_defs_[sd.tag] = &sd;
		}
		// 预声明所有函数符号：使全局初始化器里的 &函数名 能折叠为函数地址
		//   （module->getFunction 可见）。getOrInsertFunction 幂等，下方第一遍仍据
		//   同一 Function 设 linkage/uwtable。修复 00089：s={&zero} 早于函数声明发射。
		for (const auto &fn : tu.funcs)
		{
			if (module->getFunction(fn.name)) continue;
			llvm::FunctionType *ft = to_func_type(fn);
			module->getOrInsertFunction(fn.name, ft);
		}
		// 第零遍：发射全局变量
		for (const auto &gv : tu.globals)
		{
			// extern 纯声明（无定义）：创建 declaration（无 initializer）
			if (gv.is_extern && !gv.init)
			{
				llvm::Type *ty = to_llvm_alloca_type(gv.type);
				if (globals_.count(gv.name)) continue;
				auto *gvar = new llvm::GlobalVariable(*module, ty, false,
					llvm::GlobalValue::ExternalLinkage, nullptr, gv.name);
				globals_[gv.name] = gvar;
				global_types_[gv.name] = gv.type;
				continue;
			}
			llvm::Type *ty = to_llvm_alloca_type(gv.type);
			auto linkage = gv.is_static
				? llvm::GlobalValue::InternalLinkage
				: llvm::GlobalValue::ExternalLinkage;
			bool is_const = false; // P3: const 全局可考虑 constant
			llvm::Constant *init_val = llvm::Constant::getNullValue(ty);
			if (gv.init)
				init_val = fold_global_init(gv.init, ty);
			auto *gvar = new llvm::GlobalVariable(*module, ty, is_const,
				linkage, init_val, gv.name);
			if (auto a = obj_align(gv.type, ty, gv.align)) gvar->setAlignment(*a);
			globals_[gv.name] = gvar;
			global_types_[gv.name] = gv.type;
		}
		// 第一遍：声明所有函数（保证调用可见）
		for (const auto &fn : tu.funcs)
		{
			func_types_[fn.name] = fn.return_type;
			{
				std::vector<CTypePtr> ptypes;
				for (const auto &p : fn.params) ptypes.push_back(p.type);
				func_cftypes_[fn.name] = make_func_type(fn.return_type, std::move(ptypes), fn.is_variadic);
			}
			llvm::FunctionType *ft = to_func_type(fn);
			llvm::FunctionCallee FC = module->getOrInsertFunction(fn.name, ft);
			if (auto *F = llvm::dyn_cast<llvm::Function>(FC.getCallee()))
			{
				F->setLinkage(fn.is_static
					? llvm::GlobalValue::InternalLinkage
					: llvm::GlobalValue::ExternalLinkage);
				// Win64 ABI 要求每个函数带 .pdata/.xdata 展开表；clang 从源码编译时默认
				// 加 "uwtable"，但 colang 手工建 IR 不会自动带。缺它则基于 RtlUnwind 的栈
				// 回溯（UCRT longjmp、异常处理、backtrace）找不到帧 → 崩溃。统一补上。
				F->addFnAttr("uwtable");
			}
		}
		// 第二遍：定义有 body 的函数
		for (const auto &fn : tu.funcs)
		{
			if (fn.body)
			{
				gen_function(fn);
			}
		}
	}

	// 推断间接调用的 FunctionType（从类型信息或回退到 i32(i32...)）
	llvm::FunctionType *infer_call_ft(const ExprPtr &e)
	{
		// 尝试从 var_types_ / global_types_ 获取函数指针的 Function CType
		CTypePtr fty;
		if (e->lhs && e->lhs->kind == ExprKind::Unary && e->lhs->op == "*" && e->lhs->lhs && e->lhs->lhs->kind == ExprKind::VarRef)
		{
			// (*fp)(...)
			const std::string &nm = e->lhs->lhs->name;
			if (var_types_.count(nm)) fty = var_types_[nm];
			else if (global_types_.count(nm)) fty = global_types_[nm];
		}
		else if (!e->name.empty())
		{
			// fp(...)
			if (var_types_.count(e->name)) fty = var_types_[e->name];
			else if (global_types_.count(e->name)) fty = global_types_[e->name];
		}
		// 经 Call/Member 等表达式取被调类型（如 go()()、s->zerofunc()）：
		//   上述具名表未解析时，回退到共享类型源取 Pointer(Function)，避免误落 i32() 回退。
		if (!fty && e->lhs) fty = get_expr_ctype(e->lhs);
		// fty 应该是指针到函数: Pointer{Function{base=ret, params=[...]}}
		if (fty && fty->kind == TypeKind::Pointer && fty->base && fty->base->kind == TypeKind::Function)
		{
			CTypePtr fn = fty->base;
			llvm::Type *ret = to_llvm_type(fn->base);
			std::vector<llvm::Type *> pts;
			for (const auto &pt : fn->params)
				pts.push_back(to_llvm_type(pt));
			// 无原型 () 函数指针（params 空且非变参）却带实参调用：C 允许并作默认提升，
			//   LLVM 以 ret(...) 建模，避免按 i32() 零参发射触发 verifier「参数数不符」（00209 fptr1）。
			bool va = fn->is_variadic;
			if (fn->params.empty() && !va && !e->args.empty()) va = true;
			return llvm::FunctionType::get(ret, pts, va);
		}
		// 回退：假设返回 i32，参数全 i32
		std::vector<llvm::Type *> arg_tys(e->args.size(), llvm::Type::getInt32Ty(ctx));
		return llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx), arg_tys, false);
	}

	// 将 “&已声明全局对象”（含 struct 成员 .p=&x、数组名退化为指针）折叠为该全局的
	// 地址常量。无法静态解析（前向引用、成员/下标取址、复合字面量等）返回 nullptr，
	// 交由调用方继续处理（不误折成 null）。修复 c-testsuite 00045/00049：旧路径缺此项，
	// `int *p = &x;` 落到 ConstantPointerNull → 运行时 *p 解引用地址 0 崩溃。
	llvm::Constant *fold_addr_of_global(const ExprPtr &e)
	{
		if (!e) return nullptr;
		ExprPtr p = e;
		while (p && p->kind == ExprKind::Cast) p = p->lhs;
		if (!p) return nullptr;
		bool has_amp = false;
		if (p->kind == ExprKind::Unary && p->op == "&")
		{
			has_amp = true;
			p = p->lhs;
			while (p && p->kind == ExprKind::Cast) p = p->lhs;
		}
		if (!p || p->kind != ExprKind::VarRef) return nullptr;
		const std::string nm = p->name;
		auto git = globals_.find(nm);
		if (git == globals_.end())
		{
			// &函数名 → 函数地址常量（Function 继承自 Constant，可直接作初始化器）。
			if (llvm::Function *F = module->getFunction(nm)) return F;
			return nullptr;   // 前向引用尚未声明 → 不误折
		}
		// 无 & 的直接 VarRef 赋给指针：仅数组名退化合法（标量 rvalue 非地址常量）
		if (!has_amp)
		{
			auto tit = global_types_.find(nm);
			if (tit == global_types_.end() || !tit->second ||
				tit->second->kind != TypeKind::Array)
				return nullptr;
		}
		return git->second; // GlobalVariable* 即其地址（opaque ptr）
	}

	// 文件作用域复合字面量取址：按 C 语义具静态存储期，物化为内部链接全局对象
	// （初始化器由 fold_global_init 递归折叠字面量得到），返回其地址。
	// 仅匹配 Unary("&") 且操作数（剥 Cast 后）为带 type 的 InitList；否则 nullptr 交回调用方。
	llvm::Constant *fold_compound_literal_addr(const ExprPtr &e)
	{
		if (!e) return nullptr;
		ExprPtr p = e;
		while (p && p->kind == ExprKind::Cast) p = p->lhs;
		if (!p || p->kind != ExprKind::Unary || p->op != "&") return nullptr;
		ExprPtr lit = p->lhs;
		while (lit && lit->kind == ExprKind::Cast) lit = lit->lhs;
		if (!lit || lit->kind != ExprKind::InitList || !lit->type) return nullptr;
		// 字面量的 LLVM 类型：聚合走 alloca 型（与 Decl 分支同判据），标量走普通型
		TypeKind k = lit->type->kind;
		bool agg = (k == TypeKind::Array || k == TypeKind::Struct || k == TypeKind::Union);
		llvm::Type *fty = agg ? to_llvm_alloca_type(lit->type) : to_llvm_type(lit->type);
		if (!fty) return nullptr;
		llvm::Constant *init = fold_global_init(lit, fty);
		std::string nm = "__compoundliteral." + std::to_string(anon_ctr_++);
		auto *gv = new llvm::GlobalVariable(*module, fty, /*isConstant*/ false,
			llvm::GlobalValue::InternalLinkage, init, nm);
		return gv; // opaque ptr 即地址常量
	}

	// 将整型常量表达式（字面量/Cast/一元/Binary/三元）递归求值为 intmax_t。
	// 非上述形态（如 VarRef/调用/除零）返回 false，交回调用方兼以 null 兑底（不崩）。
	bool fold_int_const(const ExprPtr &e, intmax_t &out)
	{
		if (!e) return false;
		switch (e->kind)
		{
		case ExprKind::IntLiteral:
		case ExprKind::CharLiteral:
			out = static_cast<intmax_t>(e->int_val); return true;
		case ExprKind::Cast:
		{
			if (!fold_int_const(e->lhs, out)) return false;
			// C 值转换（§6.3.1.3）：整型→整型 cast 须按目标位宽截断后按目标符号性重解读
			// （gcc/clang 实现定义 = mod 2^N）。旧版对 cast 直接透传，全局初始化折叠丢符号：
			// (unsigned short)(signed char)0x81<<2 折成 0x204、应为 0xFE04（CTS D 族 93 例）。
			// _Bool 目标按非零化 0/1（C99 §6.3.2.1p3：任何非零值转 1，非按位截断）；
			// 非整型目标（指针/struct/浮点等）维持原透传行为。
			if (e->type)
			{
				if (e->type->kind == TypeKind::Bool) { out = (out != 0); return true; }
				if (llvm::Type *tt = to_llvm_type(e->type))
				{
					if (auto *iti = llvm::dyn_cast<llvm::IntegerType>(tt))
					{
						unsigned bits = iti->getBitWidth();
						if (bits < 64)
						{
							uint64_t m = static_cast<uint64_t>(out) & ((1ull << bits) - 1);
							if (!is_unsigned_int_type(e->type) && (m >> (bits - 1)) != 0)
								out = static_cast<intmax_t>(m) - static_cast<intmax_t>(1ull << bits);
							else
								out = static_cast<intmax_t>(m);
						}
					}
				}
			}
			return true;
		}
		case ExprKind::Unary:
		{
			intmax_t v; if (!fold_int_const(e->lhs, v)) return false;
			if (e->op == "-") { out = -v; return true; }
			if (e->op == "+") { out = v; return true; }
			if (e->op == "~") { out = ~v; return true; }
			if (e->op == "!") { out = v ? 0 : 1; return true; }
			return false;
		}
		case ExprKind::Binary:
		{
			intmax_t l, r;
			if (!fold_int_const(e->lhs, l) || !fold_int_const(e->rhs, r)) return false;
			const std::string &o = e->op;
			if (o == "+") out = l + r; else if (o == "-") out = l - r;
			else if (o == "*") out = l * r;
			else if (o == "/") { if (r == 0) return false; out = l / r; }
			else if (o == "%") { if (r == 0) return false; out = l % r; }
			else if (o == "<<") out = static_cast<intmax_t>(static_cast<uintmax_t>(l) << r);
			else if (o == ">>") out = l >> r;
			else if (o == "<") out = (l < r); else if (o == "<=") out = (l <= r);
			else if (o == ">") out = (l > r); else if (o == ">=") out = (l >= r);
			else if (o == "==") out = (l == r); else if (o == "!=") out = (l != r);
			else if (o == "&") out = l & r; else if (o == "|") out = l | r; else if (o == "^") out = l ^ r;
			else if (o == "&&") out = (l && r); else if (o == "||") out = (l || r);
			else return false;
			return true;
		}
		case ExprKind::Ternary:
		{
			intmax_t c; if (!fold_int_const(e->lhs, c)) return false;
			return c ? fold_int_const(e->args.size() > 0 ? e->args[0] : nullptr, out)
				: fold_int_const(e->args.size() > 1 ? e->args[1] : nullptr, out);
		}
		default:
			return false;
		}
	}

	// union 常量槽：目标为具名 struct 且其 tag 是 union（colang union 表征 = struct{[N x i8]}）。
	// 标量或花括号初值均作用于首成员（§6.7.9p19）；取首成员常量按小端写入 [N x i8]。
	// 已知限制：首成员为 float/ptr/struct 等非整型时回退全零（罕见，与旧行为一致）。
	llvm::Constant *fold_union_slot(llvm::StructType *sty, const ExprPtr &e)
	{
		llvm::Type *body = sty->getNumElements() == 1 ? sty->getElementType(0) : nullptr;
		auto *arr = body && body->isArrayTy() ? llvm::cast<llvm::ArrayType>(body) : nullptr;
		std::string tag = sty->hasName()
			? std::string(llvm::StringRef(sty->getName()).drop_front(7)) // 去 "struct." 前缀
			: std::string();
		if (!arr || tag.empty())
			return llvm::ConstantStruct::get(sty, llvm::Constant::getNullValue(body ? body : arr));
		auto sd = struct_defs_.find(tag);
		if (sd == struct_defs_.end() || sd->second->members.empty())
			return llvm::ConstantStruct::get(sty, llvm::Constant::getNullValue(arr));
		CTypePtr fm = sd->second->members[0].type; // 首成员类型
		llvm::Type *fmt = to_llvm_alloca_type(fm);
		// 初始化式：花括号取首元素，否则直接用 e
		ExprPtr item = (e->kind == ExprKind::InitList && !e->args.empty()) ? e->args[0] : e;
		llvm::Constant *mc = fmt ? fold_global_init(item, fmt) : nullptr;
		uint64_t N = arr->getNumElements();
		std::vector<uint8_t> bytes(N, 0);
		if (llvm::ConstantInt *CI = mc ? llvm::dyn_cast<llvm::ConstantInt>(mc) : nullptr)
		{
			llvm::APInt v = CI->getValue();
			if (v.getBitWidth() < N * 8) v = v.zext(N * 8);
			else if (v.getBitWidth() > N * 8) v = v.trunc(N * 8);
			for (uint64_t b = 0; b < N; ++b)
				bytes[b] = static_cast<uint8_t>(v.extractBitsAsZExtValue(8, static_cast<unsigned>(b * 8)));
		}
		llvm::Constant *arr_c = llvm::ConstantDataArray::get(ctx, bytes);
		return llvm::ConstantStruct::get(sty, arr_c);
	}

	// 目标 struct 是否为 union 包装（colang union 表征 = 具名 struct 包 [N x i8]）。
	bool is_union_struct(llvm::StructType *sty)
	{
		if (!sty || !sty->hasName()) return false;
		std::string tag = std::string(llvm::StringRef(sty->getName()).drop_front(7)); // 去 "struct."
		if (tag.empty()) return false;
		auto sd = struct_defs_.find(tag);
		return sd != struct_defs_.end() && sd->second->is_union;
	}

	// brace elision 感知的全局常量折叠：从 flat args 游标 pos 起，按目标类型 ty 消费若干项。
	// 非聚合(标量/字符串/指针/fp)：消费一项交 fold_global_init。
	// 聚合遇带括号 InitList：整体交 fold_global_init（其内部正常位置/指定式路径），消费一项。
	// 聚合遇非括号项（brace elision）：数组逐元素、struct 逐 LLVM 槽共享游标递归；union 槽取一项。
	llvm::Constant *elide_fold(llvm::Type *ty, const std::vector<ExprPtr> &args, std::size_t &pos)
	{
		if (!ty) return nullptr;
		bool agg = ty->isArrayTy() || ty->isStructTy();
		if (!agg)
		{
			if (pos >= args.size()) return llvm::Constant::getNullValue(ty);
			ExprPtr it = args[pos++];
			if (it && it->kind == ExprKind::InitList && it->args.size() == 1) it = it->args[0];
			return fold_global_init(it, ty);
		}
		// 带括号子初始化器 → 整体交 fold_global_init，消费一项
		if (pos < args.size() && args[pos] && args[pos]->kind == ExprKind::InitList)
		{
			ExprPtr sub = args[pos++];
			return fold_global_init(sub, ty);
		}
		// union 包装：取一项（标量/括号）作用于首成员（顶层 fold_global_init 会检测 union→fold_union_slot）
		if (ty->isStructTy())
		{
			auto *sty = llvm::cast<llvm::StructType>(ty);
			if (is_union_struct(sty))
			{
				if (pos >= args.size()) return llvm::Constant::getNullValue(ty);
				ExprPtr it = args[pos++];
				return fold_global_init(it, sty);
			}
		}
		// brace elision：逐元素/槽共享游标
		if (ty->isArrayTy())
		{
			auto *aty = llvm::cast<llvm::ArrayType>(ty);
			llvm::Type *ety = aty->getElementType();
			std::vector<llvm::Constant *> els;
			els.reserve(aty->getNumElements());
			for (std::size_t k = 0; k < aty->getNumElements(); ++k)
				els.push_back(elide_fold(ety, args, pos));
			return llvm::ConstantArray::get(aty, els);
		}
		// struct：逐 LLVM 槽（非位域/无手工 padding 时即 C 成员序，identity）
		auto *sty = llvm::cast<llvm::StructType>(ty);
		std::size_t n = sty->getNumElements();
		std::vector<llvm::Constant *> els(n, nullptr);
		for (std::size_t idx = 0; idx < n; ++idx)
			els[idx] = elide_fold(sty->getElementType(idx), args, pos);
		for (std::size_t i = 0; i < n; ++i)
			if (!els[i]) els[i] = llvm::Constant::getNullValue(sty->getElementType(i));
		return llvm::ConstantStruct::get(sty, els);
	}

	// 将初始化表达式折叠为 LLVM 常量（全局变量初始化器用）
	llvm::Constant *fold_global_init(const ExprPtr &e, llvm::Type *ty)
	{
		if (!e) return llvm::Constant::getNullValue(ty);
		// union 目标（具名 struct 包 [N x i8]）：标量/花括号均归首成员
		if (ty->isStructTy())
		{
			auto *sty = llvm::cast<llvm::StructType>(ty);
			if (sty->hasName())
			{
				std::string tag = std::string(llvm::StringRef(sty->getName()).drop_front(7)); // 去 "struct."
				auto sd = struct_defs_.find(tag);
				if (sd != struct_defs_.end() && sd->second->is_union)
					return fold_union_slot(sty, e);
			}
		}
		// 标量整型/浮点：接受整型常量表达式（含 Binary/Unary/Cast/Ternary，如 `1+1`）
		if (ty->isIntegerTy() || ty->isFloatTy() || ty->isDoubleTy())
		{
			intmax_t v;
			if (fold_int_const(e, v))
			{
				if (ty->isIntegerTy())
					return llvm::ConstantInt::get(ty, static_cast<uint64_t>(v));
				return llvm::ConstantFP::get(ty, static_cast<double>(v));
			}
		}
		if (e->kind == ExprKind::FloatLiteral && (ty->isFloatTy() || ty->isDoubleTy()))
			return llvm::ConstantFP::get(ty, e->float_val);
		// 字符串字面量 -> 字符数组（全局/局部静态初始化，局部 wchar_t[] 路径亦复用本 fold）
		if (e->kind == ExprKind::StringLiteral && ty->isArrayTy())
		{
			auto *aty = llvm::cast<llvm::ArrayType>(ty);
			auto *elem = aty->getElementType();
			// 必须走解码取内容字节（旧代码误用原始 str_val、把首尾引号与未展开转义当作
			// 内容写入）。内容长 < N 时余位补 0，天然涵盖 '\0' 终止与显式定长截断（§6.7.9p14）。
			// 按元素位宽分派：宽字符数组（elem 位宽 > 8，即 wchar_t->i16）填 UTF-16 码元
			// （修 plct 00220）；其余（char->i8）沿用 co_decode_string 逐字节路径、逐字不变（00115）。
			std::vector<llvm::Constant*> elems;
			auto *it = llvm::dyn_cast<llvm::IntegerType>(elem);
			const unsigned bw = it ? it->getBitWidth() : 8;
			if (bw >= 32)
			{
				// char32_t[]：UTF-32 码点（一字一码点、无代理）
				const std::vector<uint32_t> u = co_decode_utf32(e->str_val);
				for (std::size_t i = 0; i < aty->getNumElements(); ++i)
					elems.push_back(llvm::ConstantInt::get(elem, i < u.size() ? u[i] : 0));
			}
			else if (bw >= 16)
			{
				// wchar_t[] / char16_t[]：UTF-16 码元（原 >8 分支、逐字不变）
				const std::vector<uint16_t> u = co_decode_utf16(e->str_val);
				for (std::size_t i = 0; i < aty->getNumElements(); ++i)
					elems.push_back(llvm::ConstantInt::get(elem, i < u.size() ? u[i] : 0));
			}
			else
			{
				// char[] / char8_t[]：UTF-8 字节（原路径逐字不变）
				const std::string s = co_decode_string(e->str_val);
				for (std::size_t i = 0; i < aty->getNumElements(); ++i)
					elems.push_back(llvm::ConstantInt::get(elem, i < s.size() ? static_cast<uint8_t>(s[i]) : 0));
			}
			return llvm::ConstantArray::get(aty, elems);
		}
		// 复合初始化器 {e1, e2, ...} -> Array/Struct（支持指定初始化器）
		if (e->kind == ExprKind::InitList)
		{
			if (ty->isArrayTy())
			{
				auto *aty = llvm::cast<llvm::ArrayType>(ty);
				llvm::Type *elem_ty = aty->getElementType();
				std::vector<llvm::Constant*> elems(aty->getNumElements(),
					llvm::Constant::getNullValue(elem_ty));
				// brace elision（P173）：无指定符 + 元素为聚合 + 首项非花括号 → 游标消费扁平标量。
				// 指定式/逐元素带括号/标量元素数组 → 走下方既有位置式循环（行为逐字不变）。
				bool has_desig = false;
				for (const auto &ae : e->args)
					if (ae && ae->desig_index >= 0) { has_desig = true; break; }
				if (!has_desig && elem_ty->isAggregateType()
					&& !e->args.empty() && e->args[0] && e->args[0]->kind != ExprKind::InitList)
				{
					std::size_t cur = 0;
					for (std::size_t k = 0; k < aty->getNumElements(); ++k)
						elems[k] = elide_fold(elem_ty, e->args, cur);
					return llvm::ConstantArray::get(aty, elems);
				}
				std::size_t pos = 0;
				for (const auto &ae : e->args)
				{
					std::size_t idx = (ae->desig_index >= 0)
						? static_cast<std::size_t>(ae->desig_index) : pos;
					pos = idx + 1;
					if (idx < aty->getNumElements())
						elems[idx] = fold_global_init(ae, elem_ty);
				}
				return llvm::ConstantArray::get(aty, elems);
			}
			if (ty->isStructTy())
			{
				auto *sty = llvm::cast<llvm::StructType>(ty);
				std::vector<llvm::Constant*> elems(sty->getNumElements(), nullptr);
				for (std::size_t i = 0; i < sty->getNumElements(); ++i)
					elems[i] = llvm::Constant::getNullValue(sty->getElementType(i));
				// 位域 struct 的全局初始化（P191）：旧路径按「一 C 成员一 LLVM 槽」用 struct_member_ll_index(pos)（含位域的 auto-layout 不建 struct_pos_map_故为恒等）。
				// 位域折叠使 LLVM 槽数 < C 成员数 → ①误判为 brace elision、②同存储单元后续位域越界被丢、③无 (值<<偏移)| 累加。坐实：@b={i8 1,i8 2}(丢 m3:3)、@e={i32 1,i32 2}(丢 m2:4、m3 误吞 2)。
				// 修法＝含位域的 struct 按成员名路由：位域成员掩码到 bit_width、左移 bit_offset、或入其存储单元常量；普通成员走 field_index_map_。
				std::string gtag = tag_for_struct(sty);
				bool ghas_bf = false;
				auto gsd = struct_defs_.find(gtag);
				if (gsd != struct_defs_.end())
					for (const auto &mm : gsd->second->members)
						if (mm.bit_width >= 0) { ghas_bf = true; break; }
				if (ghas_bf)
				{
					const auto &gm = gsd->second->members;
					std::size_t cpos = 0; // C 成员序游标（位置初始化）
					for (const auto &se : e->args)
					{
						std::string mname;
						if (!se->desig_field.empty())
						{
							mname = se->desig_field;
							for (std::size_t mi = 0; mi < gm.size(); ++mi)
								if (gm[mi].name == mname) { cpos = mi + 1; break; }
						}
						else
						{
							while (cpos < gm.size() && gm[cpos].name.empty()) ++cpos; // 匿名位域不占初始化位（§6.7.9）
							if (cpos < gm.size()) mname = gm[cpos].name;
							++cpos;
						}
						if (mname.empty()) continue;
						std::string key = gtag + "." + mname;
						auto bf = bitfield_access_.find(key);
						if (bf != bitfield_access_.end())
						{
							const auto &ba = bf->second;
							std::size_t idx = ba.storage_idx;
							auto *ft = llvm::cast<llvm::IntegerType>(sty->getElementType(idx));
							unsigned fw = static_cast<unsigned>(ft->getBitWidth());
							llvm::Constant *vc = fold_global_init(se, ft);
							if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(vc))
							{
								llvm::APInt mask = (ba.bit_width >= fw)
									? llvm::APInt::getMaxValue(fw)
									: ((llvm::APInt(fw, 1) << ba.bit_width) - 1);
								llvm::APInt add = (ci->getValue().zextOrTrunc(fw) & mask).shl(ba.bit_offset);
								llvm::APInt cur = llvm::cast<llvm::ConstantInt>(elems[idx])->getValue();
								elems[idx] = llvm::ConstantInt::get(ft, cur | add);
							}
						}
						else
						{
							auto fit = field_index_map_.find(key);
							std::size_t idx = (fit != field_index_map_.end()) ? fit->second : struct_member_ll_index(sty, cpos - 1);
							if (idx < sty->getNumElements())
								elems[idx] = fold_global_init(se, sty->getElementType(idx));
						}
					}
					return llvm::ConstantStruct::get(sty, elems);
				}
				// brace elision（P185，C99 §6.7.9p7）：无指定符 + 首项非括号 + 平铺项数 > LLVM 槽数
				// （槽数 ≥ C 成员数，故项数超槽只能是在扁平填充某子聚合）→ 共享游标逐 LLVM 槽 elide_fold。
				// 旧路径「一项一成员」，`struct C g={1,2,3,4}`（C{long a[4];}）标量 1 交 [4 x i64] 折叠得 null → 全零。
				// 判据用「项数 > 槽数」（非「含聚合槽」、非「成员数」）：_Alignas padding 使槽多而项少→不触发
				// （保护 `struct S1 pi={7,8,9}` 定位消费）；且槽数取自 LLVM、对匿名子 struct 亦可判（struct_defs_ 不录匿名）。
				bool has_desig = false;
				for (const auto &se : e->args)
					if (se && (!se->desig_field.empty() || se->desig_index >= 0)) { has_desig = true; break; }
				if (!has_desig && !e->args.empty() && e->args[0] && e->args[0]->kind != ExprKind::InitList
					&& e->args.size() > sty->getNumElements())
				{
					std::size_t cur = 0;
					for (std::size_t idx = 0; idx < sty->getNumElements() && cur < e->args.size(); ++idx)
					{
						llvm::Type *ft = sty->getElementType(idx);
						if (llvm::Constant *c = elide_fold(ft, e->args, cur)) elems[idx] = c;
					}
					return llvm::ConstantStruct::get(sty, elems);
				}
				std::size_t pos = 0;
				for (const auto &se : e->args)
				{
					std::size_t idx;
					if (!se->desig_field.empty())
					{
						idx = find_struct_field_idx(sty, se->desig_field, pos);
						pos = idx + 1;
					}
					else
					{
						idx = struct_member_ll_index(sty, pos); // 手工布局: C 成员序→LLVM 索引
						++pos;
					}
					if (idx < sty->getNumElements())
						elems[idx] = fold_global_init(se, sty->getElementType(idx));
				}
				return llvm::ConstantStruct::get(sty, elems);
			}
		}
		// 指针/nullptr
		if (ty->isPointerTy())
		{
			// 字符串字面量初始化指针（char *p = "abc"; 及 struct{char *a;} x={"A";}）：按 C 数组→指针退化
			// （§6.3.2.1）物化为静态常量字符数组全局、返回其首元素地址。旧代码无此形→落到下方
			// ConstantPointerNull，使全局指针成员初始化后为 null（CTS 0030 族 printf("%s") 得 (null)）。
			if (e->kind == ExprKind::StringLiteral)
			{
				std::string decoded = co_decode_string(e->str_val);
				llvm::Constant *data = llvm::ConstantDataArray::getString(ctx, llvm::StringRef(decoded), /*AddNull*/ true);
				auto *saty = llvm::cast<llvm::ArrayType>(data->getType());
				auto *sgv = new llvm::GlobalVariable(*module, saty, /*isConstant*/ true,
					llvm::GlobalValue::PrivateLinkage, data, ".str");
				sgv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
				llvm::Constant *z32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
				llvm::Constant *idxs[] = { z32, z32 };
				return llvm::ConstantExpr::getGetElementPtr(saty, sgv, llvm::ArrayRef<llvm::Constant*>(idxs));
			}
			// &复合字面量 (T){...} → 物化为静态存储全局对象，返回其地址（00149/00150）
			if (llvm::Constant *c = fold_compound_literal_addr(e)) return c;
			// &全局对象 / 数组名退化 → 地址常量（优先于下方整型常量→指针路径）
			if (llvm::Constant *c = fold_addr_of_global(e)) return c;
			// 缺陷 #28：整数常量（含经 Cast 包裹）初始化指针型全局时，旧代码一律落到
			// ConstantPointerNull，使 (void(*)(int))1 / (void(*)(int))-1 被错误折成 null。
			// 按 C 语义应为「整型常量 → 指针常量」：0 → null，其余 → IntToPtr 常量。
			// 覆盖 signal.h 的 SIG_DFL(0)/SIG_IGN(1)/SIG_ERR(-1)，及任何 `T *g = (T*)常量;`。
			// 注：-1 在 AST 中是 Unary("-") 包 IntLiteral(1)，需连同一元 +/- 一起剥并记符号。
			ExprPtr cur = e;
			bool negate = false;
			while (cur)
			{
				if (cur->kind == ExprKind::Cast) { cur = cur->lhs; continue; }
				if (cur->kind == ExprKind::Unary && cur->op == "-") { negate = !negate; cur = cur->lhs; continue; }
				if (cur->kind == ExprKind::Unary && cur->op == "+") { cur = cur->lhs; continue; }
				break;
			}
			if (cur && (cur->kind == ExprKind::IntLiteral || cur->kind == ExprKind::CharLiteral))
			{
				intmax_t ival = cur->int_val;
				if (negate) ival = -ival;
				uint64_t bits = static_cast<uint64_t>(ival);
				if (bits == 0)
					return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty));
				llvm::Constant *ci = llvm::ConstantInt::get(llvm::IntegerType::get(ctx, 64), bits);
				return llvm::ConstantExpr::getIntToPtr(ci, llvm::cast<llvm::PointerType>(ty));
			}
			return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty));
		}
		return llvm::Constant::getNullValue(ty);
	}

	void gen_function(const FuncDecl &fn)
	{
		llvm::Function *F = module->getFunction(fn.name);
		if (!F)
		{
			return;
		}
		cur_func_name_ = fn.name;
		vars_.clear();
		var_types_.clear();
		loop_stack_.clear();
		label_blocks_.clear();
		case_label_bb_.clear();
		pending_gotos_.clear();
		llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", F);
		builder.SetInsertPoint(entry);
		// 为参数建 alloca + store（使地址可及，统一查找）
		{
			auto it = F->arg_begin();
			for (std::size_t i = 0; i < fn.params.size() && it != F->arg_end(); ++i, ++it)
			{
				llvm::Value *argval = &*it;
				CTypePtr pty = fn.params[i].type;
				llvm::AllocaInst *al = nullptr;
				if (pty && (pty->kind == TypeKind::Struct || pty->kind == TypeKind::Union))
				{
					// struct/union 形参按值收编（修 plct 00204 arg()）：colang 把 struct 值按 ptr 传，
					// 旧代码统一 CreateAlloca(arg_ty=ptr) 建了 ptr 型 alloca，而 var_types_ 记的是 struct 型，
					// 成员访问 a.x 把 ptr* 当 struct 指针 GEP→多一层解引用乱码。此处建真实 StructType alloca
					// 并从入参指针 memcpy（手动 byval），与局部 struct Decl 路径(to_llvm_alloca_type)一致。
					llvm::Type *sty = to_llvm_alloca_type(pty);
					al = builder.CreateAlloca(sty, nullptr, fn.params[i].name.c_str());
					emit_memcpy(al, argval, module->getDataLayout().getTypeAllocSize(sty));
				}
				else
				{
					llvm::Type *arg_ty = argval->getType();
					al = builder.CreateAlloca(arg_ty, nullptr, fn.params[i].name.c_str());
					builder.CreateStore(argval, al);
				}
				if (!fn.params[i].name.empty())
				{
					vars_[fn.params[i].name] = al;
					var_types_[fn.params[i].name] = pty;
				}
			}
		}
		gen_stmt(fn.body, F);
		// 当前插入点无终止则补 ret
		if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
		{
			if (F->getReturnType()->isVoidTy())
				builder.CreateRetVoid();
			else
				builder.CreateRet(llvm::ConstantInt::get(F->getReturnType(), 0));
		}
		std::string verify_err;
		llvm::raw_string_ostream vs(verify_err);
		if (llvm::verifyFunction(*F, &vs))
		{
			errs.push_back(Error{fn.loc.file, Pos{fn.loc.line, fn.loc.column},
				"IR verification: " + verify_err});
		}
	}

	// brace elision 感知的运行时局部初始化（与 elide_fold 同构）：从 flat args 游标 pos 按 ty 消费若干项 store 到 addr。
	void elide_store(llvm::Type *ty, llvm::Value *addr, const std::vector<ExprPtr> &args, std::size_t &pos)
	{
		if (!ty) return;
		bool agg = ty->isArrayTy() || ty->isStructTy();
		if (!agg)
		{
			if (pos >= args.size()) return;
			ExprPtr it = args[pos++];
			if (it && it->kind == ExprKind::InitList && it->args.size() == 1) it = it->args[0];
			llvm::Value *v = gen_expr(it);
			if (v && v->getType() != ty) v = convert_value(v, ty, get_expr_ctype(it));
			if (v) builder.CreateStore(v, addr);
			return;
		}
		// 带括号子初始化器 → 交 emit_init_list_store（现有位置/指定式路径），消费一项
		if (pos < args.size() && args[pos] && args[pos]->kind == ExprKind::InitList)
		{
			ExprPtr sub = args[pos++];
			emit_init_list_store(sub, addr, ty);
			return;
		}
		// union 包装：取一项作用于首成员（store 到偏移 0）
		if (ty->isStructTy())
		{
			auto *sty = llvm::cast<llvm::StructType>(ty);
			if (is_union_struct(sty))
			{
				if (pos >= args.size()) return;
				ExprPtr it = args[pos++];
				if (it && it->kind == ExprKind::InitList && !it->args.empty()) it = it->args[0];
				std::string utag = std::string(llvm::StringRef(sty->getName()).drop_front(7));
				auto usd = struct_defs_.find(utag);
				llvm::Type *fmt = (usd != struct_defs_.end() && !usd->second->members.empty())
					? to_llvm_alloca_type(usd->second->members[0].type) : nullptr;
				if (fmt)
				{
					llvm::Value *v = gen_expr(it);
					if (v && v->getType() != fmt) v = convert_value(v, fmt, get_expr_ctype(it));
					if (v) builder.CreateStore(v, addr);
				}
				return;
			}
		}
		// brace elision：逐元素/槽共享游标
		if (ty->isArrayTy())
		{
			auto *aty = llvm::cast<llvm::ArrayType>(ty);
			llvm::Type *ety = aty->getElementType();
			for (std::size_t k = 0; k < aty->getNumElements(); ++k)
			{
				llvm::Value *ii = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), k);
				llvm::Value *ptr = builder.CreateGEP(aty, addr, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), ii});
				elide_store(ety, ptr, args, pos);
			}
			return;
		}
		auto *sty = llvm::cast<llvm::StructType>(ty);
		for (std::size_t idx = 0; idx < sty->getNumElements(); ++idx)
		{
			llvm::Value *ptr = builder.CreateStructGEP(sty, addr, static_cast<unsigned>(idx));
			elide_store(sty->getElementType(idx), ptr, args, pos);
		}
	}

	// 局部复合初始化器: 逐元素 store 到 alloca/GlobalVariable
	void emit_init_list_store(const ExprPtr &e, llvm::Value *addr, llvm::Type *ty)
	{
		if (e->kind != ExprKind::InitList) return;
		if (ty->isArrayTy())
		{
			auto *aty = llvm::cast<llvm::ArrayType>(ty);
			llvm::Type *elem_ty = aty->getElementType();
			// brace elision（P173）：无指定符 + 元素为聚合 + 首项非花括号 → 游标消费扁平标量（与全局 elide_fold 同构）。
			// 指定式/逐元素带括号/标量元素 → 走下方既有循环（行为逐字不变）。
			{
				bool has_desig = false;
				for (const auto &el : e->args)
					if (el && el->desig_index >= 0) { has_desig = true; break; }
				if (!has_desig && elem_ty->isAggregateType()
					&& !e->args.empty() && e->args[0] && e->args[0]->kind != ExprKind::InitList)
				{
					std::size_t cur = 0;
					for (std::size_t k = 0; k < aty->getNumElements(); ++k)
					{
						llvm::Value *ii = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), k);
						llvm::Value *ptr = builder.CreateGEP(aty, addr, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), ii});
						elide_store(elem_ty, ptr, e->args, cur);
					}
					return;
				}
			}
			std::size_t pos = 0; // 当前位置（用于非指定元素）
			for (const auto &elem : e->args)
			{
				// 指定索引或顺序
				std::size_t idx = (elem->desig_index >= 0)
					? static_cast<std::size_t>(elem->desig_index) : pos;
				pos = idx + 1;
				if (idx >= aty->getNumElements()) break;
				llvm::Value *ii = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), idx);
				llvm::Value *ptr = builder.CreateGEP(aty, addr, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), ii});
				if (elem_ty->isAggregateType() && elem->kind == ExprKind::InitList)
					emit_init_list_store(elem, ptr, elem_ty);
				else
				{
					llvm::Value *v = gen_expr(elem);
					if (v->getType() != elem_ty) v = convert_value(v, elem_ty, get_expr_ctype(elem));
					builder.CreateStore(v, ptr);
				}
			}
		}
		else if (ty->isStructTy())
		{
			auto *sty = llvm::cast<llvm::StructType>(ty);
			// brace elision（P185，C99 §6.7.9p7）：无指定符 + 首项非括号 + 平铺项数 > LLVM 槽数
			// → 共享游标逐槽 elide_store（与全局 elide_fold 同构）。旧路径「一项一成员」位置消费，
			// `struct C m = {5,6,7,8}`（C{long a[4];}）首项 store 到 a 后次项 idx 越界 break → 只写首元素。
			// 判据「项数 > 槽数」：_Alignas padding 使槽多项少→不触发（保护定位消费）；槽数取自 LLVM、匿名子 struct 亦可判。
			{
				bool has_desig = false;
				for (const auto &se : e->args)
					if (se && (!se->desig_field.empty() || se->desig_index >= 0)) { has_desig = true; break; }
				if (!has_desig && !e->args.empty() && e->args[0] && e->args[0]->kind != ExprKind::InitList
					&& e->args.size() > sty->getNumElements())
				{
					std::size_t cur = 0;
					for (std::size_t idx = 0; idx < sty->getNumElements() && cur < e->args.size(); ++idx)
						elide_store(sty->getElementType(idx), builder.CreateStructGEP(sty, addr, static_cast<unsigned>(idx)), e->args, cur);
					return;
				}
			}
			std::size_t pos = 0;
			for (const auto &elem : e->args)
			{
				std::size_t idx;
				// 通过 .name 查找字段索引
				if (!elem->desig_field.empty())
				{
					// 从 struct_defs_ 查成员名列表
					idx = find_struct_field_idx(sty, elem->desig_field, pos);
					pos = idx + 1;
				}
				else
				{
					idx = struct_member_ll_index(sty, pos); // 手工布局: C 成员序→LLVM 索引
					++pos;
				}
				if (idx >= sty->getNumElements()) break;
				llvm::Value *ptr = builder.CreateStructGEP(sty, addr, static_cast<unsigned>(idx));
				llvm::Type *ft = sty->getElementType(static_cast<unsigned>(idx));
				// union 槽：标量/花括号初值均按首成员类型写入偏移 0（匿名 union 与具名 union 通用）
				if (ft->isStructTy())
				{
					auto *ust = llvm::cast<llvm::StructType>(ft);
					if (ust->hasName())
					{
						std::string utag = std::string(llvm::StringRef(ust->getName()).drop_front(7)); // 去 "struct."
						auto usd = struct_defs_.find(utag);
						if (usd != struct_defs_.end() && usd->second->is_union && !usd->second->members.empty())
						{
							llvm::Type *fmt = to_llvm_alloca_type(usd->second->members[0].type);
							ExprPtr item = (elem->kind == ExprKind::InitList && !elem->args.empty()) ? elem->args[0] : elem;
							if (fmt)
							{
								llvm::Value *v = gen_expr(item);
								if (v->getType() != fmt) v = convert_value(v, fmt, get_expr_ctype(item));
								builder.CreateStore(v, ptr);
								continue;
							}
						}
					}
				}
				if (ft->isAggregateType() && elem->kind == ExprKind::InitList)
					emit_init_list_store(elem, ptr, ft);
				else
				{
					llvm::Value *v = gen_expr(elem);
					if (v->getType() != ft) v = convert_value(v, ft, get_expr_ctype(elem));
					builder.CreateStore(v, ptr);
				}
			}
		}
	}

	// 查找 struct 字段在 LLVM StructType 中的索引
	std::size_t find_struct_field_idx(llvm::StructType *sty, const std::string &fname, std::size_t fallback)
	{
		// 通过 struct_types_ 找到 tag
		for (auto &[tag, st] : struct_types_)
		{
			if (st != sty) continue;
			// 优先查 field_index_map_（处混位域布局）
			std::string key = tag + "." + fname;
			auto fi_it = field_index_map_.find(key);
			if (fi_it != field_index_map_.end())
				return fi_it->second;
			// 位域成员: 返回其存储单元索引
			auto bf_it = bitfield_access_.find(key);
			if (bf_it != bitfield_access_.end())
				return bf_it->second.storage_idx;
			// fallback: 源码顺序索引
			auto dit = struct_defs_.find(tag);
			if (dit == struct_defs_.end()) continue;
			const auto &members = dit->second->members;
			for (std::size_t i = 0; i < members.size(); ++i)
				if (members[i].name == fname)
					return i;
		}
		return fallback;
	}

	void gen_stmt(const StmtPtr &s, llvm::Function *F)
	{
		if (!s)
		{
			return;
		}
		switch (s->kind)
		{
		case StmtKind::Compound:
		{
			// 真块 {...}：进入前快照 name→alloca / name→CType 两张表，出块后复原，
			// 实现块级作域（内层同名声明不污染外层）。合成 Compound（is_block_scope=false）
			// 不快照，使多声明符继续绑定到外层。
			if (s->is_block_scope)
			{
				auto saved_vars = vars_;
				auto saved_types = var_types_;
				for (const auto &inner : s->body)
					gen_stmt(inner, F);
				vars_ = std::move(saved_vars);
				var_types_ = std::move(saved_types);
			}
			else
			{
				for (const auto &inner : s->body)
					gen_stmt(inner, F);
			}
			break;
		}
		case StmtKind::Return:
		{
			// 若当前块已被上一终止符结束（如 goto/return 之后的死代码、或同块内
			// 连续的 return），先开一个新延续块，避免在同一块终止符后继续发射。
			if (!builder.GetInsertBlock()->empty() && builder.GetInsertBlock()->back().isTerminator())
				builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "after.ret", F));
			if (s->expr)
			{
				llvm::Value *rv = gen_expr(s->expr);
				llvm::Type *rt = F->getReturnType();
				// 返回値隐式转换到函数返回类型（C §6.8.7.5）；聚合/void 不转
				if (rv->getType() != rt && !rt->isVoidTy() &&
					!rt->isStructTy() && !rt->isArrayTy())
					rv = convert_value(rv, rt, get_expr_ctype(s->expr));
				builder.CreateRet(rv);
			}
			else
				builder.CreateRetVoid();
			break;
		}
		case StmtKind::ExprStmt:
			if (s->expr)
				gen_expr(s->expr);
			break;
		case StmtKind::Decl:
		{
			// 块作用域函数声明（如 `int f1(char *);`）：不占存储、不绑 vars_——否则调用
			// 名会命中「局部指针间接调用」分支、读未初始化 alloca 跳转→崩溃（00078）。
			// 使其回退到 getFunction(名) 直调（函数指针对象 `int (*fp)(..)` 为 Pointer 型，不受影响）。
			if (s->var_type && s->var_type->kind == TypeKind::Function)
				break;
			bool is_arr = (s->var_type && s->var_type->kind == TypeKind::Array);
			bool is_struct = (s->var_type && (s->var_type->kind == TypeKind::Struct || s->var_type->kind == TypeKind::Union));
			llvm::Type *ty = (is_arr || is_struct) ? to_llvm_alloca_type(s->var_type) : to_llvm_type(s->var_type);
			llvm::Value *addr = nullptr;
			if (s->is_static)
			{
				// 局部 static → GlobalVariable
				std::string gname = "__static_" + cur_func_name_ + "." + s->var_name;
				llvm::Constant *sv = llvm::Constant::getNullValue(ty);
				auto *gv = new llvm::GlobalVariable(*module, ty, false,
					llvm::GlobalValue::InternalLinkage, sv, gname);
				if (auto a = obj_align(s->var_type, ty, s->var_align)) gv->setAlignment(*a);
				addr = gv;
			}
			else
			{
				// 定长局部 alloca 提升到函数 entry 块：entry 支配全函数，修「goto 向前跳入声明块」
				// 导致 alloca 不支配 use（00199/00207「Instruction does not dominate all uses」）。
				// 仅指令位置移入 entry；下方初始化 store 仍在当前块、vars_ 绑定与块作用域快照/复原不变。
				llvm::BasicBlock *saved_bb = builder.GetInsertBlock();
				llvm::BasicBlock::iterator saved_ip = builder.GetInsertPoint();
				llvm::BasicBlock &EB = F->getEntryBlock();
				builder.SetInsertPoint(&EB, EB.begin());
				addr = builder.CreateAlloca(ty, nullptr, s->var_name.c_str());
				if (auto a = obj_align(s->var_type, ty, s->var_align))
					llvm::cast<llvm::AllocaInst>(addr)->setAlignment(*a);
				builder.SetInsertPoint(saved_bb, saved_ip);
			}
			vars_[s->var_name] = addr;
			var_types_[s->var_name] = s->var_type;
			// 初始化
			if (s->var_init)
			{
				if (s->is_static)
				{
					// static 局部：初始化值写入 GlobalVariable initializer，不发射运行时 store
					if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(addr))
						gv->setInitializer(fold_global_init(s->var_init, ty));
				}
				else if (s->var_init->kind == ExprKind::InitList && (is_arr || is_struct))
				{
					// 先零填 alloca（指定初始化器可能跳过某些位置）
					uint64_t sz = module->getDataLayout().getTypeAllocSize(ty);
					builder.CreateMemSet(addr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0), sz, llvm::MaybeAlign(1));
					emit_init_list_store(s->var_init, addr, ty);
				}
				else if (is_arr && s->var_init->kind == ExprKind::StringLiteral)
				{
					// 局部字符数组由字符串字面量初始化：旧代码无任何分支命中→alloca 保垃圾。
					// 复用 fold_global_init 构造精确 N 元素常量数组（已含补零/截断），整体 store。
					if (llvm::Constant *c = fold_global_init(s->var_init, ty))
						builder.CreateStore(c, addr);
				}
				else if (is_struct)
				{
					// struct/union 用运行期表达式初始化（调用返回、另一左值拷贝、*ptr、成员、按值复合字面量）：
					//   旧代码四分支均不命中→alloca 保未初始化垃圾（修 plct 00204 ret() 首行即乱码的根因）。
					//   本前端把 struct 值按 ptr 传/返；gen_lvalue_addr 取源地址后整体 memcpy 进 alloca。
					llvm::Value *src = gen_lvalue_addr(s->var_init);
					uint64_t sz = module->getDataLayout().getTypeAllocSize(ty);
					emit_memcpy(addr, src, sz);
				}
				else if (!is_arr && !is_struct)
				{
					llvm::Value *v = gen_expr(s->var_init);
					if (v->getType() != ty)
						v = convert_value(v, ty, get_expr_ctype(s->var_init));
					builder.CreateStore(v, addr);
				}
			}
			break;
		}
		case StmtKind::If:
		{
			// C23 init-statement
			if (s->init_stmt) gen_stmt(s->init_stmt, F);
			llvm::Value *cond = gen_cond(s->expr);
			llvm::BasicBlock *then_bb = llvm::BasicBlock::Create(ctx, "if.then", F);
			llvm::BasicBlock *end_bb = llvm::BasicBlock::Create(ctx, "if.end", F);
			llvm::BasicBlock *else_bb = s->else_branch
				? llvm::BasicBlock::Create(ctx, "if.else", F)
				: end_bb; // 无 else 时直接跳 end
			builder.CreateCondBr(cond, then_bb, else_bb);
			builder.SetInsertPoint(then_bb);
			gen_stmt(s->then_branch, F);
			if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
				builder.CreateBr(end_bb);
			if (s->else_branch)
			{
				builder.SetInsertPoint(else_bb);
				gen_stmt(s->else_branch, F);
				if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
					builder.CreateBr(end_bb);
			}
			builder.SetInsertPoint(end_bb);
			break;
		}
		case StmtKind::While:
		{
			llvm::BasicBlock *cond_bb = llvm::BasicBlock::Create(ctx, "while.cond", F);
			llvm::BasicBlock *body_bb = llvm::BasicBlock::Create(ctx, "while.body", F);
			llvm::BasicBlock *end_bb = llvm::BasicBlock::Create(ctx, "while.end", F);
			builder.CreateBr(cond_bb);
			builder.SetInsertPoint(cond_bb);
			llvm::Value *cond = gen_cond(s->expr);
			builder.CreateCondBr(cond, body_bb, end_bb);
			builder.SetInsertPoint(body_bb);
			loop_stack_.push_back({end_bb, cond_bb});
			gen_stmt(s->then_branch, F);
			loop_stack_.pop_back();
			if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
				builder.CreateBr(cond_bb);
			builder.SetInsertPoint(end_bb);
			break;
		}
		case StmtKind::DoWhile:
		{
			llvm::BasicBlock *body_bb = llvm::BasicBlock::Create(ctx, "do.body", F);
			llvm::BasicBlock *cond_bb = llvm::BasicBlock::Create(ctx, "do.cond", F);
			llvm::BasicBlock *end_bb = llvm::BasicBlock::Create(ctx, "do.end", F);
			builder.CreateBr(body_bb);
			builder.SetInsertPoint(body_bb);
			loop_stack_.push_back({end_bb, cond_bb});
			gen_stmt(s->then_branch, F);
			loop_stack_.pop_back();
			if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
				builder.CreateBr(cond_bb);
			builder.SetInsertPoint(cond_bb);
			llvm::Value *cond = gen_cond(s->expr);
			builder.CreateCondBr(cond, body_bb, end_bb);
			builder.SetInsertPoint(end_bb);
			break;
		}
		case StmtKind::For:
		{
			// for(init; cond; incr) body
			if (s->for_init)
				gen_stmt(s->for_init, F);
			llvm::BasicBlock *cond_bb = llvm::BasicBlock::Create(ctx, "for.cond", F);
			llvm::BasicBlock *body_bb = llvm::BasicBlock::Create(ctx, "for.body", F);
			llvm::BasicBlock *incr_bb = llvm::BasicBlock::Create(ctx, "for.incr", F);
			llvm::BasicBlock *end_bb = llvm::BasicBlock::Create(ctx, "for.end", F);
			builder.CreateBr(cond_bb);
			builder.SetInsertPoint(cond_bb);
			if (s->expr)
			{
				llvm::Value *cond = gen_cond(s->expr);
				builder.CreateCondBr(cond, body_bb, end_bb);
			}
			else
				builder.CreateBr(body_bb); // 无 cond = 始终为真
			builder.SetInsertPoint(body_bb);
			loop_stack_.push_back({end_bb, incr_bb});
			gen_stmt(s->then_branch, F);
			loop_stack_.pop_back();
			if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
				builder.CreateBr(incr_bb);
			builder.SetInsertPoint(incr_bb);
			if (s->for_incr)
				gen_expr(s->for_incr);
			builder.CreateBr(cond_bb);
			builder.SetInsertPoint(end_bb);
			break;
		}
		case StmtKind::Break:
		{
			if (!loop_stack_.empty())
			{
				if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
					builder.CreateBr(loop_stack_.back().break_target);
			}
			// 后续代码死化
			llvm::BasicBlock *dead = llvm::BasicBlock::Create(ctx, "after.break", F);
			builder.SetInsertPoint(dead);
			break;
		}
		case StmtKind::Continue:
		{
			if (!loop_stack_.empty() && loop_stack_.back().continue_target)
			{
				if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
					builder.CreateBr(loop_stack_.back().continue_target);
			}
			llvm::BasicBlock *dead = llvm::BasicBlock::Create(ctx, "after.cont", F);
			builder.SetInsertPoint(dead);
			break;
		}
		case StmtKind::Switch:
			gen_switch(s, F);
			break;
		case StmtKind::Goto:
		{
			llvm::BasicBlock *&target = label_blocks_[s->label_name];
			if (!target)
				target = llvm::BasicBlock::Create(ctx, ("label." + s->label_name).c_str(), F);
			if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
				builder.CreateBr(target);
			llvm::BasicBlock *dead = llvm::BasicBlock::Create(ctx, "after.goto", F);
			builder.SetInsertPoint(dead);
			break;
		}
		case StmtKind::Label:
		{
			llvm::BasicBlock *lbl_bb;
			auto lit = label_blocks_.find(s->label_name);
			if (lit != label_blocks_.end())
				lbl_bb = lit->second;
			else {
				lbl_bb = llvm::BasicBlock::Create(ctx, ("label." + s->label_name).c_str(), F);
				label_blocks_[s->label_name] = lbl_bb;
			}
			if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
				builder.CreateBr(lbl_bb);
			builder.SetInsertPoint(lbl_bb);
			if (s->then_branch)
				gen_stmt(s->then_branch, F);
			break;
		}
		case StmtKind::Case:
		case StmtKind::Default:
		{
			// case/default 与 label 同构（P172）：重定向到预收集建好的标签 BB，再发射其 then_branch。
			// 自边护栏（当前已是目标 BB 则不 br），使 case 嵌于循环体时 fallthrough/回边自然成立。
			llvm::BasicBlock *tbb = case_label_bb_[s.get()];
			if (!tbb)
				tbb = case_label_bb_[s.get()] = llvm::BasicBlock::Create(ctx, "switch.landing", F);
			if (builder.GetInsertBlock() != tbb &&
				(builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator()))
				builder.CreateBr(tbb);
			builder.SetInsertPoint(tbb);
			if (s->then_branch)
				gen_stmt(s->then_branch, F);
			break;
		}
		default:
			break;
		}
	}

	// —— switch/case/default ——
	void gen_switch(const StmtPtr &s, llvm::Function *F)
	{
		// C23 init-statement
		if (s->init_stmt) gen_stmt(s->init_stmt, F);
		llvm::Value *disc = to_i32(gen_expr(s->expr));
		llvm::BasicBlock *switch_end = llvm::BasicBlock::Create(ctx, "switch.end", F);

		// 递归收集全部 case/default 标签（含嵌套在循环/分支体内者），各建一个 BB 并登记 case_label_bb_。
		std::vector<std::pair<std::int64_t, llvm::BasicBlock *>> cases;
		llvm::BasicBlock *default_bb = nullptr;
		collect_switch_labels(s->then_branch, cases, F, default_bb);

		// 发射分发：value→标签 BB；无 default 落 switch_end。重复 case 值跳过 addCase（其 BB 仍可经 fallthrough 达）。
		llvm::SwitchInst *sw = builder.CreateSwitch(disc,
			default_bb ? default_bb : switch_end, static_cast<unsigned>(cases.size()));
		std::vector<std::int64_t> seen;
		for (auto &c : cases)
		{
			bool dup = false;
			for (auto v : seen) if (v == c.first) { dup = true; break; }
			if (dup) continue;
			seen.push_back(c.first);
			sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx),
				static_cast<uint64_t>(c.first), true), c.second);
		}

		// break 目标 = switch_end（C 语义：switch 内 break 跳出 switch）
		loop_stack_.push_back({switch_end, nullptr});
		// 体内按源码顺序线性发射；Case/Default 经 gen_stmt 的 label 分支重定向到各自 BB。
		// 前导非 case 语句落进此 entry（C 语义本就不可达，属合法不可达块，-O 后清除）。
		llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "switch.body", F);
		builder.SetInsertPoint(entry);
		gen_stmt(s->then_branch, F);
		if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
			builder.CreateBr(switch_end);
		loop_stack_.pop_back();
		builder.SetInsertPoint(switch_end);
	}

	// 递归收集 switch 体内所有 case/default 标签（含嵌套在循环/分支体内者），各建一个 BB。
	// 遇嵌套 Switch 停止下探（内层 case 由内层 gen_switch 自行收集）。
	void collect_switch_labels(const StmtPtr &s,
		std::vector<std::pair<std::int64_t, llvm::BasicBlock *>> &cases,
		llvm::Function *F, llvm::BasicBlock* &default_bb)
	{
		if (!s) return;
		switch (s->kind)
		{
		case StmtKind::Case:
		{
			auto *bb = llvm::BasicBlock::Create(ctx, "switch.case", F);
			case_label_bb_[s.get()] = bb;
			cases.push_back({ s->case_val, bb });
			collect_switch_labels(s->then_branch, cases, F, default_bb);
			return;
		}
		case StmtKind::Default:
		{
			if (!default_bb)
			{
				default_bb = llvm::BasicBlock::Create(ctx, "switch.default", F);
				case_label_bb_[s.get()] = default_bb;
			}
			else
			{
				// 多 default（非法 C）：仍给独立落地块，仅 fallthrough 可达，不接进分发
				auto *bb = llvm::BasicBlock::Create(ctx, "switch.default.dup", F);
				case_label_bb_[s.get()] = bb;
			}
			collect_switch_labels(s->then_branch, cases, F, default_bb);
			return;
		}
		case StmtKind::Compound:
			for (const auto &item : s->body)
				collect_switch_labels(item, cases, F, default_bb);
			return;
		case StmtKind::If:
			collect_switch_labels(s->then_branch, cases, F, default_bb);
			collect_switch_labels(s->else_branch, cases, F, default_bb);
			return;
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::For:
		case StmtKind::Label:
			collect_switch_labels(s->then_branch, cases, F, default_bb);
			return;
		case StmtKind::Switch:
			return; // 嵌套 switch：其 case 由内层 gen_switch 自行收集
		default:
			return; // Goto/Return/Break/Continue/ExprStmt/Decl 等不含 case
		}
	}

	// 生成布尔条件值（对于 if/while/do，非零为真）
	llvm::Value *gen_cond(const ExprPtr &e)
	{
		llvm::Value *v = gen_expr(e);
		llvm::Type *vt = v->getType();
		if (vt == llvm::Type::getInt1Ty(ctx))
			return v;
		if (vt->isPointerTy())
			return builder.CreateICmpNE(v, llvm::ConstantPointerNull::get(
				llvm::cast<llvm::PointerType>(vt)));
		// 整数非零判定
		return builder.CreateICmpNE(v, llvm::ConstantInt::get(vt, 0));
	}

	// 选择 cast opcode（仅整型之间）
	static llvm::Instruction::CastOps get_cast_op(llvm::Type *from, llvm::Type *to)
	{
		if (from->isPointerTy() && !to->isPointerTy())
			return llvm::Instruction::PtrToInt;
		if (!from->isPointerTy() && to->isPointerTy())
			return llvm::Instruction::IntToPtr;
		if (from->isPointerTy() && to->isPointerTy())
			return llvm::Instruction::BitCast;
		// 两者都是整型
		auto fw = llvm::cast<llvm::IntegerType>(from)->getBitWidth();
		auto tw = llvm::cast<llvm::IntegerType>(to)->getBitWidth();
		if (tw < fw) return llvm::Instruction::Trunc;
		if (tw > fw) {
			if (fw == 1) return llvm::Instruction::ZExt; // bool 扩展用零扩展
			return llvm::Instruction::SExt;
		}
		return llvm::Instruction::BitCast;
	}

	// 通用值转换（int↔int/int↔fp/fp↔fp/ptr↔int）。src_t 为源表达式 CType（可空），
	// 仅用于整数加宽时判定源符号性（缺陷 #30：无符号源不得符号扩展）。
	llvm::Value *convert_value(llvm::Value *v, llvm::Type *target, const CTypePtr &src_t = nullptr)
	{
		llvm::Type *from = v->getType();
		if (from == target) return v;
		if (from->isPointerTy() && !target->isPointerTy())
			return builder.CreatePtrToInt(v, target);
		if (!from->isPointerTy() && target->isPointerTy())
			return builder.CreateIntToPtr(v, target);
		if (from->isFloatingPointTy() && target->isFloatingPointTy())
		{
			if (from->getPrimitiveSizeInBits() > target->getPrimitiveSizeInBits())
				return builder.CreateFPTrunc(v, target);
			return builder.CreateFPExt(v, target);
		}
		if (from->isFloatingPointTy() && target->isIntegerTy())
			return builder.CreateFPToSI(v, target);
		if (from->isIntegerTy() && target->isFloatingPointTy())
			return builder.CreateSIToFP(v, target);
		if (from->isIntegerTy() && target->isIntegerTy())
		{
			// _Bool (i1): 非零→1, 零→0
			if (target == llvm::Type::getInt1Ty(ctx) && from->getIntegerBitWidth() > 1)
			{
				llvm::Value *nz = builder.CreateICmpNE(v,
					llvm::ConstantInt::get(from, 0));
				return nz;
			}
			// i1 → iN (widening bool to int): zext
			if (from == llvm::Type::getInt1Ty(ctx) && target->getIntegerBitWidth() > 1)
				return builder.CreateZExt(v, target);
			// 加宽（get_cast_op 选 SExt 方向）时按源类型符号性改选 ZExt：否则
			// (long long)(unsigned int)0x80000000 会被符号扩展成负数（缺陷 #30）。
			llvm::Instruction::CastOps op = get_cast_op(from, target);
			if (op == llvm::Instruction::SExt && src_t && is_unsigned_int_type(src_t))
				op = llvm::Instruction::ZExt;
			return builder.CreateCast(op, v, target);
		}
		return v;
	}

	// C §6.3.1.1/.1.2「默认参数提升」：传给变参省略号(...)的实参须做——
	//   窄整型(_Bool/char/short，位宽<32)按符号性扩展→int、float→double；
	//   int/long/long long/double/指针原样。供「有原型变参函数的变参尾」(coerce_call_args)
	//   与「隐式函数声明 int f() 全变参」两路共用（单一真值源）。
	llvm::Value *default_arg_promote(llvm::Value *v, CTypePtr ct)
	{
		llvm::Type *t = v->getType();
		llvm::Type *I32 = llvm::Type::getInt32Ty(ctx);
		if (t->isIntegerTy())
		{
			if (t == llvm::Type::getInt1Ty(ctx))
				return builder.CreateZExt(v, I32); // _Bool → int
			if (t->getIntegerBitWidth() < 32)
			{
				bool uns = ct && is_unsigned_int_type(ct);
				return uns ? builder.CreateZExt(v, I32) : builder.CreateSExt(v, I32);
			}
		}
		else if (t->isFloatTy())
			return builder.CreateFPExt(v, llvm::Type::getDoubleTy(ctx)); // float → double
		return v;
	}

	// 将实参列表逐个隐式转换到函数类型的固定形参类型（C §6.5.2.2p5）。
	// 变参尾（超出 getNumParams、匹配 ... 的实参）做默认参数提升（§6.5.2.2p6）。
	void coerce_call_args(llvm::FunctionType *ft, std::vector<llvm::Value *> &args,
		const std::vector<CTypePtr> &arg_cts)
	{
		if (!ft) return;
		const unsigned n = ft->getNumParams();
		for (unsigned i = 0; i < args.size() && i < n; ++i)
			args[i] = convert_value(args[i], ft->getParamType(i),
				i < arg_cts.size() ? arg_cts[i] : nullptr);
		if (ft->isVarArg())
			for (unsigned i = n; i < args.size(); ++i)
				args[i] = default_arg_promote(args[i],
					i < arg_cts.size() ? arg_cts[i] : nullptr);
	}

	llvm::Value *gen_expr(const ExprPtr &e)
	{
		if (!e)
		{
			return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
		}
		switch (e->kind)
		{
		case ExprKind::IntLiteral:
			// nullptr (int_val==0 && type is Pointer) → 常量空指针
			if (e->int_val == 0 && e->type && e->type->kind == TypeKind::Pointer)
				return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx));
			// 带后缀的整数字面量：long long → i64（其余 LLP64 下 int/long 均 i32）
			if (e->type && e->type->kind == TypeKind::LongLong)
				return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx),
					static_cast<uint64_t>(e->int_val), true);
			return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx),
				static_cast<uint64_t>(e->int_val), true);
		case ExprKind::FloatLiteral:
			return llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx), e->float_val);
		case ExprKind::CharLiteral:
			// int_val 已由 parser 经 co_decode_char 解码为真实码值（跨层单一真值源）
			return llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx),
				static_cast<uint8_t>(e->int_val));
		case ExprKind::StringLiteral:
		{
			std::string decoded = decode_string(e->str_val);
			return builder.CreateGlobalString(llvm::StringRef(decoded), ".str");
		}
		case ExprKind::VarRef:
		{
			// __func__: 当前函数名的字符串常量
			if (e->name == "__func__")
				return builder.CreateGlobalString(cur_func_name_, ".func");
			auto it = vars_.find(e->name);
			if (it != vars_.end())
			{
				// 数组/结构体：返回指针（退化），不 load
				llvm::Type *aty = get_var_llvm_type(it->second);
				if (aty->isArrayTy() || aty->isStructTy())
					return it->second;
				llvm::Value *v = builder.CreateLoad(aty, it->second);
				// C 整型提升：所有 <i32 的整数类型扩展到 i32
				if (aty->isIntegerTy() && aty->getIntegerBitWidth() < 32)
				{
					if (aty == llvm::Type::getInt1Ty(ctx))
						return builder.CreateZExt(v, llvm::Type::getInt32Ty(ctx));
					// _BitInt(N) unsigned → zext; wchar_t(无符号) → zext; 其他 → sext
					auto vt = var_types_.find(e->name);
					if (vt != var_types_.end() && vt->second && is_unsigned_int_type(vt->second))
						return builder.CreateZExt(v, llvm::Type::getInt32Ty(ctx));
					return builder.CreateSExt(v, llvm::Type::getInt32Ty(ctx));
				}
				return v;
			}
			// 全局变量
			auto git = globals_.find(e->name);
			if (git != globals_.end())
			{
				llvm::Type *gty = git->second->getValueType();
				if (gty->isArrayTy() || gty->isStructTy())
					return git->second;
				llvm::Value *v = builder.CreateLoad(gty, git->second);
				// C 整型提升：所有 <i32 的整数类型扩展到 i32
				if (gty->isIntegerTy() && gty->getIntegerBitWidth() < 32)
				{
					if (gty == llvm::Type::getInt1Ty(ctx))
						return builder.CreateZExt(v, llvm::Type::getInt32Ty(ctx));
					auto gt = global_types_.find(e->name);
					if (gt != global_types_.end() && gt->second && is_unsigned_int_type(gt->second))
						return builder.CreateZExt(v, llvm::Type::getInt32Ty(ctx));
					return builder.CreateSExt(v, llvm::Type::getInt32Ty(ctx));
				}
				return v;
			}
			// 回退：查找函数符号
			llvm::Function *F = module->getFunction(e->name);
			if (F) return F;
			return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
		}
		case ExprKind::Call:
		{
			// 变参内建（标准 §7.16）：va_start/va_arg/va_end/va_copy。与 clang Win64 产出对齐：
			// va_list 为 char*(ptr)；va_start/va_end/va_copy 走 llvm intrinsic（取 ap 地址）；
			// va_arg 为纯指针算术（load ap、按 align8(sizeof(T)) 前移并回写、从旧址 load T）。
			if (!e->lhs && (e->name == "va_start" || e->name == "va_end" ||
					e->name == "va_arg" || e->name == "va_copy") &&
					!e->args.empty())
			{
				llvm::Type *ptr_ty = llvm::PointerType::getUnqual(ctx);
				if (e->name == "va_arg")
				{
					llvm::Value *apaddr = gen_lvalue_addr(e->args[0]);
					llvm::Type *vty = e->type ? to_llvm_type(e->type) : llvm::Type::getInt32Ty(ctx);
					llvm::Value *ap = builder.CreateLoad(ptr_ty, apaddr);
					const auto &DL = module->getDataLayout();
					uint64_t sz = DL.getTypeStoreSize(vty).getFixedValue();
					uint64_t stride = (sz + 7) & ~static_cast<uint64_t>(7);
					if (stride == 0) stride = 8;
					llvm::Value *strV = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), stride);
					llvm::Value *nxt = builder.CreateGEP(llvm::Type::getInt8Ty(ctx), ap, strV);
					builder.CreateStore(nxt, apaddr);
					return builder.CreateLoad(vty, ap);
				}
				if (e->name == "va_start")
				{
					llvm::Value *apaddr = gen_lvalue_addr(e->args[0]);
					return builder.CreateIntrinsic(llvm::Type::getVoidTy(ctx), llvm::Intrinsic::vastart, {apaddr});
				}
				if (e->name == "va_end")
				{
					llvm::Value *apaddr = gen_lvalue_addr(e->args[0]);
					return builder.CreateIntrinsic(llvm::Type::getVoidTy(ctx), llvm::Intrinsic::vaend, {apaddr});
				}
				// va_copy(dst, src)：两侧均取地址
				{
					llvm::Value *d = gen_lvalue_addr(e->args[0]);
					llvm::Value *s = e->args.size() > 1 ? gen_lvalue_addr(e->args[1]) : d;
					return builder.CreateIntrinsic(llvm::Type::getVoidTy(ctx), llvm::Intrinsic::vacopy, {d, s});
				}
			}
			// C23 <stdckdint.h>：ckd_add/ckd_sub/ckd_mul(result, a, b) 溢出判定（§7.20）。
			// colang 直建 IR、clang 前端 __builtin_*_overflow 对生成码不可用，故按名特判 lowering。
			// 语义（本机 clang ground-truth 校准）：溢出相对 *result 的类型 T 判定；在
			// CW=max(w(a),w(b),w(T)) 上精确运算，*result 存回绕值；CW>TW 时额外做 T 可表示范围检查。
			if (!e->lhs && (e->name == "ckd_add" || e->name == "ckd_sub" || e->name == "ckd_mul") &&
				e->args.size() == 3)
			{
				CTypePtr rptr_t = get_expr_ctype(e->args[0]);
				CTypePtr T = (rptr_t && rptr_t->kind == TypeKind::Pointer) ? rptr_t->base : nullptr;
				if (!T) T = make_type(TypeKind::Int); // 兜底：按 int
				auto ckd_bits = [](const CTypePtr &tt) -> unsigned {
					if (!tt) return 32;
					if (tt->kind == TypeKind::BitInt) return tt->bit_width ? tt->bit_width : 32;
					return get_bitfield_unit_bits(tt);
				};
				unsigned Tw = ckd_bits(T);
				bool Tu = is_unsigned_int_type(T);
				CTypePtr at = get_expr_ctype(e->args[1]);
				CTypePtr bt = get_expr_ctype(e->args[2]);
				unsigned aw = ckd_bits(at), bw = ckd_bits(bt);
				bool au = is_unsigned_int_type(at), bu = is_unsigned_int_type(bt);
				unsigned CW = Tw; if (aw > CW) CW = aw; if (bw > CW) CW = bw;
				llvm::Type *CWty = get_int_ty_for_bits(CW);
				llvm::Value *av = widen_int(gen_expr(e->args[1]), CW, au);
				llvm::Value *bv = widen_int(gen_expr(e->args[2]), CW, bu);
				bool comp_u = Tu && au && bu; // 全无符号→无符号 intrinsic；否则有符号（负操作数经范围检查捕获）
				llvm::Intrinsic::ID iid;
				if (e->name == "ckd_add") iid = comp_u ? llvm::Intrinsic::uadd_with_overflow : llvm::Intrinsic::sadd_with_overflow;
				else if (e->name == "ckd_sub") iid = comp_u ? llvm::Intrinsic::usub_with_overflow : llvm::Intrinsic::ssub_with_overflow;
				else iid = comp_u ? llvm::Intrinsic::umul_with_overflow : llvm::Intrinsic::smul_with_overflow;
				llvm::Type *resTy = llvm::StructType::get(ctx, {CWty, llvm::Type::getInt1Ty(ctx)});
				llvm::Value *res = builder.CreateIntrinsic(resTy, iid, {av, bv});
				llvm::Value *sumv = builder.CreateExtractValue(res, 0);
				llvm::Value *ovwide = builder.CreateExtractValue(res, 1);
				llvm::Type *Tty = get_int_ty_for_bits(Tw);
				llvm::Value *storev = sumv;
				llvm::Value *overflow = ovwide;
				if (CW != Tw)
				{
					// 仅在操作数严格宽于结果类型时触发（此处 Tw<CW<=64，移位安全）
					storev = builder.CreateTrunc(sumv, Tty);
					llvm::Value *fits = nullptr;
					if (Tu)
					{
						llvm::Value *tmax = llvm::ConstantInt::get(CWty, (Tw >= 64 ? ~0ULL : ((1ULL << Tw) - 1)), false);
						if (comp_u) fits = builder.CreateICmpULE(sumv, tmax);
						else {
							llvm::Value *nonneg = builder.CreateICmpSGE(sumv, llvm::ConstantInt::get(CWty, 0, true));
							fits = builder.CreateAnd(nonneg, builder.CreateICmpULE(sumv, tmax));
						}
					}
					else
					{
						long long lo = -(1LL << (Tw - 1));
						long long hi = (1LL << (Tw - 1)) - 1;
						llvm::Value *tlo = llvm::ConstantInt::get(CWty, lo, true);
						llvm::Value *thi = llvm::ConstantInt::get(CWty, hi, true);
						fits = builder.CreateAnd(builder.CreateICmpSGE(sumv, tlo), builder.CreateICmpSLE(sumv, thi));
					}
					overflow = builder.CreateOr(ovwide, builder.CreateNot(fits));
				}
				llvm::Value *raddr = gen_expr(e->args[0]);
				llvm::Type *peTy = to_llvm_type(T);
				if (storev->getType() != peTy)
				{
					unsigned sw = llvm::cast<llvm::IntegerType>(storev->getType())->getBitWidth();
					unsigned dw = llvm::cast<llvm::IntegerType>(peTy)->getBitWidth();
					storev = sw > dw ? builder.CreateTrunc(storev, peTy)
								  : builder.CreateSExt(storev, peTy);
				}
				builder.CreateStore(storev, raddr);
				return builder.CreateZExt(overflow, llvm::Type::getInt32Ty(ctx));
			}
			// C23 <stdbit.h>（§7.18）：stdc_<op>[_uc|_us|_ui|_ul|_ull] 位操作内建。
			// colang 直建 IR、本机 clang 无 stdbit.h，故按名特判 lowering 到 llvm.ctpop/ctlz/cttz + 算术。
			// 语义严格对齐发布版 §7.18.3–7.18.16 Returns 条款：
			//   计数类(leading/trailing/first/count_*/bit_width)→unsigned int；
			//   bit_floor/bit_ceil→同宽无符号值；has_single_bit→bool(0/1)。
			//   first_* =「该方向连续同类位计数，若未越界则 +1，全同类则 0」；bit_width=W-clz。
			// 参数按无符号 W 位处理（signed 输入取其模 2^W 表示，§7.18 泛型说明）。
			if (!e->lhs && e->args.size() == 1 && e->name.rfind("stdc_", 0) == 0)
			{
				std::string core;
				unsigned fw = stdbit_parse(e->name, core);
				static const char *cores[] = {"leading_zeros","leading_ones","trailing_zeros",
					"trailing_ones","first_leading_zero","first_leading_one","first_trailing_zero",
					"first_trailing_one","count_zeros","count_ones","has_single_bit","bit_width",
					"bit_floor","bit_ceil"};
				bool known = false;
				for (const char *c : cores) if (core == c) { known = true; break; }
				if (known)
				{
					unsigned W = fw;
					if (!W) {
						CTypePtr at = get_expr_ctype(e->args[0]);
						W = get_bitfield_unit_bits(at);
					}
					llvm::Type *Wty = get_int_ty_for_bits(W);
					llvm::Value *x = gen_expr(e->args[0]);
					if (x->getType()->isPointerTy()) x = builder.CreateLoad(Wty, x);
					unsigned vw = llvm::cast<llvm::IntegerType>(x->getType())->getBitWidth();
					if (vw > W) x = builder.CreateTrunc(x, Wty);
					else if (vw < W) x = builder.CreateZExt(x, Wty);
					llvm::Value *zero = llvm::ConstantInt::get(Wty, 0);
					llvm::Value *one  = llvm::ConstantInt::get(Wty, 1);
					llvm::Value *Wv   = llvm::ConstantInt::get(Wty, W);
					llvm::Value *shmask = llvm::ConstantInt::get(Wty, W - 1); // 移位量取模，防 shift>=宽度 poison
					auto clz = [&](llvm::Value *a){ return builder.CreateIntrinsic(Wty, llvm::Intrinsic::ctlz, {a, llvm::ConstantInt::getFalse(ctx)}); };
					auto ctz = [&](llvm::Value *a){ return builder.CreateIntrinsic(Wty, llvm::Intrinsic::cttz, {a, llvm::ConstantInt::getFalse(ctx)}); };
					auto pop = [&](llvm::Value *a){ return builder.CreateIntrinsic(Wty, llvm::Intrinsic::ctpop, {a}); };
					llvm::Value *nx = builder.CreateNot(x); // W 位按位取反
					// 计数/标志在 Wty 上求值，末尾归一到 unsigned int；value-op 直接返回 Wty
					auto to_i32 = [&](llvm::Value *v) -> llvm::Value *{
						llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
						if (W > 32) return builder.CreateTrunc(v, i32);
						if (W < 32) return builder.CreateZExt(v, i32);
						return v;
					};
					if (core == "leading_zeros")  return to_i32(clz(x));
					if (core == "leading_ones")   return to_i32(clz(nx));
					if (core == "trailing_zeros") return to_i32(ctz(x));
					if (core == "trailing_ones")  return to_i32(ctz(nx));
					if (core == "count_ones")     return to_i32(pop(x));
					if (core == "count_zeros")    return to_i32(builder.CreateSub(Wv, pop(x)));
					if (core == "bit_width")      return to_i32(builder.CreateSub(Wv, clz(x)));
					auto first_of = [&](llvm::Value *cnt) -> llvm::Value *{
						// cnt==W 表示「全同类，未找到」→0；否则 cnt+1
						llvm::Value *plus = builder.CreateAdd(cnt, one);
						return builder.CreateSelect(builder.CreateICmpEQ(cnt, Wv), zero, plus);
					};
					if (core == "first_leading_one")  return to_i32(first_of(clz(x)));
					if (core == "first_leading_zero") return to_i32(first_of(clz(nx)));
					if (core == "first_trailing_one") return to_i32(first_of(ctz(x)));
					if (core == "first_trailing_zero")return to_i32(first_of(ctz(nx)));
					if (core == "has_single_bit") {
						llvm::Value *nz = builder.CreateICmpNE(x, zero);
						llvm::Value *single = builder.CreateICmpEQ(builder.CreateAnd(x, builder.CreateSub(x, one)), zero);
						return builder.CreateZExt(builder.CreateAnd(nz, single), llvm::Type::getInt32Ty(ctx));
					}
					if (core == "bit_floor") {
						// (x==0)?0 : 1<<(bit_width(x)-1)
						llvm::Value *bw = builder.CreateSub(Wv, clz(x));
						llvm::Value *val = builder.CreateShl(one, builder.CreateAnd(builder.CreateSub(bw, one), shmask));
						return builder.CreateSelect(builder.CreateICmpEQ(x, zero), zero, val);
					}
					if (core == "bit_ceil") {
						// (x<=1)?1 : (bit_width(x-1)==W ? 0(不可表示) : 1<<bit_width(x-1))
						llvm::Value *bw = builder.CreateSub(Wv, clz(builder.CreateSub(x, one))); // bit_width(x-1)
						llvm::Value *val = builder.CreateShl(one, builder.CreateAnd(bw, shmask));
						llvm::Value *calc = builder.CreateSelect(builder.CreateICmpEQ(bw, Wv), zero, val);
						return builder.CreateSelect(builder.CreateICmpULE(x, one), one, calc);
					}
				}
			}
			// Annex K（C11 §K.3）务实子集（P9.30）：本机 UCRT 不提供 ISO 名的
			// memset_s/strnlen_s 与约束 handler 三件套，故 colang 按名特判 lowering：
			//   memset_s(s,smax,c,n) → 越界钳制（n>smax 取 smax；s 空或 smax=0 填 0）后 memset，返回非0/0；
			//   strnlen_s(s,max)    → memchr(s,0,max) 定位 NUL（无则 max），s 空返 0；
			//   abort_handler_s     → abort()；ignore_handler_s → llvm.donothing；
			//   set_constraint_handler_s(h) → 存于模块级内部全局，返回旧值。
			// 非严格符合：UCRT 直连的其余 _s 函数走 MS invalid-parameter 而非本 handler。
			if (!e->lhs && (e->name == "memset_s" || e->name == "strnlen_s" ||
					e->name == "abort_handler_s" || e->name == "ignore_handler_s" ||
					e->name == "set_constraint_handler_s"))
			{
				llvm::Type *PTY = llvm::PointerType::getUnqual(ctx);
				llvm::Type *I32 = llvm::Type::getInt32Ty(ctx);
				llvm::Type *I64 = llvm::Type::getInt64Ty(ctx);
				llvm::Type *VOID = llvm::Type::getVoidTy(ctx);
				llvm::Value *nullp = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(PTY));
				auto as_i64 = [&](llvm::Value *v) -> llvm::Value * {
					if (v->getType() == I64) return v;
					if (v->getType()->isPointerTy()) return builder.CreatePtrToInt(v, I64);
					if (v->getType()->isIntegerTy())
					{
						unsigned w = v->getType()->getIntegerBitWidth();
						if (w < 64) return builder.CreateZExt(v, I64);
						return builder.CreateTrunc(v, I64);
					}
					return v;
				};
				if (e->name == "memset_s" && e->args.size() == 4)
				{
					llvm::Function *F = builder.GetInsertBlock()->getParent();
					llvm::Value *sp = gen_expr(e->args[0]);
					llvm::Value *smax = as_i64(gen_expr(e->args[1]));
					llvm::Value *craw = gen_expr(e->args[2]);
					llvm::Value *n = as_i64(gen_expr(e->args[3]));
					llvm::Value *ci = craw;
					if (ci->getType() != I32)
						ci = ci->getType()->getIntegerBitWidth() > 32 ? builder.CreateTrunc(ci, I32)
																		: builder.CreateSExt(ci, I32);
					llvm::Value *isnull = builder.CreateICmpEQ(sp, nullp);
					llvm::Value *smax0 = builder.CreateICmpEQ(smax, llvm::ConstantInt::get(I64, 0));
					llvm::Value *over = builder.CreateICmpUGT(n, smax);
					llvm::Value *viol = builder.CreateOr(builder.CreateOr(isnull, smax0), over);
					llvm::Value *mn = builder.CreateSelect(over, smax, n);
					llvm::Value *efflen = builder.CreateSelect(isnull, llvm::ConstantInt::get(I64, 0), mn);
					llvm::BasicBlock *do_bb = llvm::BasicBlock::Create(ctx, "annexk.mset", F);
					llvm::BasicBlock *end_bb = llvm::BasicBlock::Create(ctx, "annexk.mset.end", F);
					builder.CreateCondBr(builder.CreateNot(isnull), do_bb, end_bb);
					builder.SetInsertPoint(do_bb);
					llvm::FunctionCallee mc = module->getOrInsertFunction("memset",
						llvm::FunctionType::get(PTY, {PTY, I32, I64}, false));
					builder.CreateCall(mc, {sp, ci, efflen});
					builder.CreateBr(end_bb);
					builder.SetInsertPoint(end_bb);
					return builder.CreateZExt(viol, I32);
				}
				if (e->name == "strnlen_s" && e->args.size() == 2)
				{
					llvm::Function *F = builder.GetInsertBlock()->getParent();
					llvm::Value *sp = gen_expr(e->args[0]);
					llvm::Value *mx = as_i64(gen_expr(e->args[1]));
					llvm::Value *isnull = builder.CreateICmpEQ(sp, nullp);
					llvm::BasicBlock *do_bb = llvm::BasicBlock::Create(ctx, "annexk.strnlen", F);
					llvm::BasicBlock *nul_bb = llvm::BasicBlock::Create(ctx, "annexk.strnlen.null", F);
					llvm::BasicBlock *end_bb = llvm::BasicBlock::Create(ctx, "annexk.strnlen.end", F);
					builder.CreateCondBr(isnull, nul_bb, do_bb);
					builder.SetInsertPoint(do_bb);
					llvm::FunctionCallee mc = module->getOrInsertFunction("memchr",
						llvm::FunctionType::get(PTY, {PTY, I32, I64}, false));
					llvm::Value *p = builder.CreateCall(mc, {sp, llvm::ConstantInt::get(I32, 0), mx});
					llvm::Value *pnull = builder.CreateICmpEQ(p, nullp);
					llvm::Value *diff = builder.CreateSub(builder.CreatePtrToInt(p, I64),
						builder.CreatePtrToInt(sp, I64));
					llvm::Value *resdo = builder.CreateSelect(pnull, mx, diff);
					builder.CreateBr(end_bb);
					builder.SetInsertPoint(nul_bb);
					llvm::Value *resnul = llvm::ConstantInt::get(I64, 0);
					builder.CreateBr(end_bb);
					builder.SetInsertPoint(end_bb);
					llvm::PHINode *ph = builder.CreatePHI(I64, 2);
					ph->addIncoming(resdo, do_bb);
					ph->addIncoming(resnul, nul_bb);
					return ph;
				}
				if (e->name == "abort_handler_s" && e->args.size() == 3)
				{
					for (const auto &a : e->args) gen_expr(a); // 仅保留实参副作用
					llvm::FunctionCallee ab = module->getOrInsertFunction("abort",
						llvm::FunctionType::get(VOID, {}, false));
					return builder.CreateCall(ab, {});
				}
				if (e->name == "ignore_handler_s" && e->args.size() == 3)
				{
					for (const auto &a : e->args) gen_expr(a);
					return builder.CreateIntrinsic(VOID, llvm::Intrinsic::donothing, {});
				}
				if (e->name == "set_constraint_handler_s" && e->args.size() == 1)
				{
					llvm::Value *h = gen_expr(e->args[0]);
					if (!annexk_handler_gv_)
						annexk_handler_gv_ = new llvm::GlobalVariable(*module, PTY, false,
							llvm::GlobalValue::InternalLinkage,
							llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(PTY)),
							"__colang_constraint_handler");
					llvm::Value *old = builder.CreateLoad(PTY, annexk_handler_gv_);
					builder.CreateStore(h, annexk_handler_gv_);
					return old;
				}
			}
			// stdatomic（C11 §7.17，P9.33 务实子集）：atomic_* 为编译器内建（clang 走 __c11_atomic_*），
			// colang 直建 IR 且不用 clang 前端，故按名特判 lowering 到 LLVM 原子指令。
			// 务实：memory_order 实参一律忽略、统一按 seq_cst（最严格序，语义安全超集）；仅对
			// 自然字长类型（int 等）保证 lock-free（x86-64 上 LLVM 原子指令天然 lock-free）。
			// atomic_flag 以 { int } 承载，_Lock 在偏移 0，故对 flag 地址直接做 i32 原子操作。
			if (!e->lhs && !e->args.empty() &&
				(e->name == "atomic_load" || e->name == "atomic_load_explicit" ||
					e->name == "atomic_store" || e->name == "atomic_store_explicit" ||
					e->name == "atomic_init" || e->name == "atomic_exchange" ||
					e->name == "atomic_exchange_explicit" || e->name == "atomic_fetch_add" ||
					e->name == "atomic_fetch_add_explicit" || e->name == "atomic_fetch_sub" ||
					e->name == "atomic_fetch_sub_explicit" || e->name == "atomic_fetch_and" ||
					e->name == "atomic_fetch_or" || e->name == "atomic_fetch_xor" ||
					e->name == "atomic_compare_exchange_strong" ||
					e->name == "atomic_compare_exchange_strong_explicit" ||
					e->name == "atomic_thread_fence" || e->name == "atomic_signal_fence" ||
					e->name == "atomic_flag_test" || e->name == "atomic_flag_test_and_set" ||
					e->name == "atomic_flag_test_and_set_explicit" || e->name == "atomic_flag_clear"))
			{
				llvm::Type *I32 = llvm::Type::getInt32Ty(ctx);
				const std::string nm = e->name;
				auto SEQ = llvm::AtomicOrdering::SequentiallyConsistent;
				bool isflag = (nm.rfind("atomic_flag", 0) == 0);
				// 值类型：flag 恒 i32；否则由所指对象类型推得，推不出默认 i32（测试仅用 int，默认即正确）
				llvm::Type *vt = I32;
				if (!isflag)
				{
					CTypePtr pt = get_expr_ctype(e->args[0]);
					if (pt && (pt->kind == TypeKind::Pointer || pt->kind == TypeKind::Array) &&
						pt->base && pt->base->kind != TypeKind::Void)
						vt = to_llvm_type(pt->base);
				}
				llvm::Value *sp = gen_expr(e->args[0]);
				auto coerce = [&](llvm::Value *v) -> llvm::Value * {
					if (v->getType() == vt) return v;
					if (v->getType()->isIntegerTy() && vt->isIntegerTy())
					{
						unsigned w = v->getType()->getIntegerBitWidth(), t = vt->getIntegerBitWidth();
						return w < t ? builder.CreateSExt(v, vt) : builder.CreateTrunc(v, vt);
					}
					if (v->getType()->isPointerTy() && vt->isIntegerTy()) return builder.CreatePtrToInt(v, vt);
					if (v->getType()->isIntegerTy() && vt->isPointerTy()) return builder.CreateIntToPtr(v, vt);
					return v;
				};
				auto zero = llvm::ConstantInt::get(vt, 0);
				if (nm == "atomic_load" || nm == "atomic_load_explicit")
				{
					llvm::LoadInst *li = builder.CreateLoad(vt, sp);
					li->setOrdering(SEQ);
					return li;
				}
				if (nm == "atomic_store" || nm == "atomic_store_explicit")
				{
					llvm::StoreInst *si = builder.CreateStore(coerce(gen_expr(e->args[1])), sp);
					si->setOrdering(SEQ);
					return si;
				}
				if (nm == "atomic_init")
					return builder.CreateStore(coerce(gen_expr(e->args[1])), sp); // 非原子
				if (nm == "atomic_thread_fence" || nm == "atomic_signal_fence")
					return builder.CreateFence(SEQ);
				if (nm == "atomic_flag_test")
				{
					llvm::LoadInst *li = builder.CreateLoad(vt, sp);
					li->setOrdering(SEQ);
					return builder.CreateZExt(builder.CreateICmpNE(li, zero), I32);
				}
				if (nm == "atomic_flag_clear")
				{
					llvm::StoreInst *si = builder.CreateStore(llvm::ConstantInt::get(vt, 0), sp);
					si->setOrdering(SEQ);
					return si;
				}
				{
					llvm::AtomicRMWInst::BinOp op = llvm::AtomicRMWInst::Xchg;
					bool isrmw = true, isftas = false;
					if (nm == "atomic_exchange" || nm == "atomic_exchange_explicit") op = llvm::AtomicRMWInst::Xchg;
					else if (nm == "atomic_fetch_add" || nm == "atomic_fetch_add_explicit") op = llvm::AtomicRMWInst::Add;
					else if (nm == "atomic_fetch_sub" || nm == "atomic_fetch_sub_explicit") op = llvm::AtomicRMWInst::Sub;
					else if (nm == "atomic_fetch_and") op = llvm::AtomicRMWInst::And;
					else if (nm == "atomic_fetch_or") op = llvm::AtomicRMWInst::Or;
					else if (nm == "atomic_fetch_xor") op = llvm::AtomicRMWInst::Xor;
					else if (nm == "atomic_flag_test_and_set" || nm == "atomic_flag_test_and_set_explicit") { op = llvm::AtomicRMWInst::Xchg; isftas = true; }
					else isrmw = false;
					if (isrmw)
					{
						llvm::Value *val = isftas ? (llvm::Value *)llvm::ConstantInt::get(vt, 1)
												  : coerce(gen_expr(e->args[1]));
						llvm::AtomicRMWInst *rmw = builder.CreateAtomicRMW(op, sp, val, llvm::MaybeAlign(), SEQ);
						if (isftas) return builder.CreateZExt(builder.CreateICmpNE(rmw, zero), I32);
						return rmw;
					}
				}
				if (nm == "atomic_compare_exchange_strong" || nm == "atomic_compare_exchange_strong_explicit")
				{
					llvm::Value *eptr = gen_expr(e->args[1]);
					llvm::Value *desired = coerce(gen_expr(e->args[2]));
					llvm::Value *cmp = builder.CreateLoad(vt, eptr);
					llvm::AtomicCmpXchgInst *cx = builder.CreateAtomicCmpXchg(sp, cmp, desired, llvm::MaybeAlign(), SEQ, SEQ);
					llvm::Value *oldval = builder.CreateExtractValue(cx, 0);
					llvm::Value *success = builder.CreateExtractValue(cx, 1); // i1
					builder.CreateStore(oldval, eptr); // 失败置 *expected=old；成功 old==旧值==*expected，写回不变
					return builder.CreateZExt(success, I32);
				}
				return llvm::ConstantInt::get(I32, 0);
			}
			std::vector<llvm::Value *> args;
			std::vector<CTypePtr> arg_cts;
			for (const auto &a : e->args)
			{
				arg_cts.push_back(get_expr_ctype(a));
				args.push_back(gen_expr(a));
			}
			// 纯间接调用：callee 是表达式（如 (*fp)(...) 或 funcs[i](...) ）
			if (e->lhs)
			{
				llvm::Value *fp = gen_expr(e->lhs);
				llvm::FunctionType *ft = infer_call_ft(e);
				coerce_call_args(ft, args, arg_cts);
				return builder.CreateCall(ft, fp, args);
			}
			// 命名调用：先查本地变量（函数指针），再查全局，最后查函数
			if (!e->name.empty())
			{
				// 局部变量（函数指针）
				auto vit = vars_.find(e->name);
				if (vit != vars_.end())
				{
					llvm::Type *vt = get_var_llvm_type(vit->second);
					if (vt->isPointerTy())
					{
						llvm::Value *fp = builder.CreateLoad(vt, vit->second);
						llvm::FunctionType *ft = infer_call_ft(e);
						coerce_call_args(ft, args, arg_cts);
						return builder.CreateCall(ft, fp, args);
					}
				}
				// 全局变量（函数指针）
				auto git = globals_.find(e->name);
				if (git != globals_.end())
				{
					CTypePtr gt = global_types_.count(e->name) ? global_types_[e->name] : nullptr;
					if (gt && gt->kind == TypeKind::Pointer && gt->base && gt->base->kind == TypeKind::Function)
					{
						llvm::Value *fp = builder.CreateLoad(llvm::PointerType::getUnqual(ctx), git->second);
						llvm::FunctionType *ft = infer_call_ft(e);
						coerce_call_args(ft, args, arg_cts);
						return builder.CreateCall(ft, fp, args);
					}
				}
				// 直接函数调用
				llvm::Function *callee = module->getFunction(e->name);
				if (callee)
				{
					// UCRT(x64) setjmp ABI：_setjmp 必须以调用者帧地址作隐藏第二参数
					// （等价 clang 前端的 llvm.frameaddress(0) 注入），且“返回两次”（直/回路径）。
					// colang 走 .bc 后端、不经 clang 前端内建重写，故在此手工补齐：否则 jmp_buf
					// 帧槽为垃圾、longjmp 回跳时崩溃。setjmp.h 的 setjmp(env) 宏只传 env 一参，
					// 第二参由此处补齐（_setjmp 声明为 2 参以对齐 IR 类型）。
					if (e->name == "_setjmp" &&
						args.size() + 1 == callee->getFunctionType()->getNumParams())
					{
						callee->addFnAttr(llvm::Attribute::ReturnsTwice);
						llvm::Value *fa = builder.CreateIntrinsic(
							llvm::PointerType::getUnqual(ctx), llvm::Intrinsic::frameaddress,
							{llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0)});
						args.push_back(fa);
					}
					// C §6.5.2.2p5：实参隐式转换到（固定）形参类型（如 int→size_t）
					coerce_call_args(callee->getFunctionType(), args, arg_cts);
					return builder.CreateCall(callee, args);
				}
				// 未声明的函数：按 C 语言实现「隐式函数声明」（§6.5.2.2p4 + §6.3.1.1/.1.2，
				// C89/C99 规则；C23 已移除，本项目按用户决策实现之）：
				//   1) 隐式声明为 int f()——返回 int、**无原型**（未知形参），LLVM 表为 int(...)
				//      纯变参（不再像旧写法那样把每个实参塞进固定 i32 形参——那会让指针/long/
				//      float/窄整型实参与 i32 形参类型不符而触发 verifier 断言/崩溃）；
				//   2) 每个实参做「默认参数提升」：窄整型(<i32)按符号性扩展→i32、float→double、
				//      指针/数组退化指针/int/long/long long/double 原样保持，再按变参 ABI 传递。
				{
					llvm::Type *I32 = llvm::Type::getInt32Ty(ctx);
					// 默认参数提升（§6.3.1.1/.1.2）：隐式声明 int f() 全变参，每个实参都提升——
					//   与有原型变参函数共用同一 helper（单一真值源，行为与旧内联写法逐字一致）。
					for (size_t i = 0; i < args.size(); ++i)
						args[i] = default_arg_promote(args[i],
							i < arg_cts.size() ? arg_cts[i] : nullptr);
					llvm::FunctionType *ft = llvm::FunctionType::get(I32, /*Params=*/{}, /*isVarArg=*/true);
					callee = llvm::cast<llvm::Function>(module->getOrInsertFunction(e->name, ft).getCallee());
					return builder.CreateCall(callee, args);
				}
			}
			return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
		}
		case ExprKind::Binary:
			return gen_binary(e);
		case ExprKind::Unary:
			return gen_unary(e);
		case ExprKind::Assign:
			return gen_assign(e);
		case ExprKind::Ternary:
			return gen_ternary(e);
		case ExprKind::Comma:
			return gen_comma(e);
		case ExprKind::Cast:
			return gen_cast_expr(e);
		case ExprKind::Member:
		{
			// rvalue: 取成员地址
			llvm::Value *addr = gen_member_addr(e);
			// 确定成员类型（decl_tag/decl_idx 指向字段真正声明处，支持匿名 union 名提升）
			MemberInfo mi = resolve_member_info(e);
			const std::string &tag = mi.decl_tag;
			unsigned idx = mi.decl_idx;
			// 位域读访问: load storage → shift → mask
			std::string bf_key = tag + "." + e->name;
			auto bf_it = bitfield_access_.find(bf_key);
			if (bf_it != bitfield_access_.end())
			{
				const auto &ba = bf_it->second;
				llvm::Type *stor_ty = get_int_ty_for_bits(ba.unit_bits);
				llvm::Value *sv = builder.CreateLoad(stor_ty, addr);
				// shift right
				if (ba.bit_offset > 0)
					sv = builder.CreateLShr(sv, llvm::ConstantInt::get(stor_ty, ba.bit_offset));
				// mask
				if (ba.bit_width < ba.unit_bits)
				{
					uint64_t mask = (1ULL << ba.bit_width) - 1;
					sv = builder.CreateAnd(sv, llvm::ConstantInt::get(stor_ty, mask));
				}
				// 结果宽度：<32 的存储单元按 C 整型提升扩到 i32；==32 保持 i32；==64 保持 i64（旧代码一律 ZExt/SExt→i32，
				// 对 64 位存储单元的宽位域（如 unsigned long long m:40/:61）产生非法「zext i64→i32」，CTS 0013）。
				if (sv->getType()->getIntegerBitWidth() < 32)
					sv = ba.is_signed ? builder.CreateSExt(sv, llvm::Type::getInt32Ty(ctx))
									  : builder.CreateZExt(sv, llvm::Type::getInt32Ty(ctx));
				return sv;
			}
			// 普通成员
			llvm::Type *field_ty = llvm::Type::getInt32Ty(ctx);
			auto sd = struct_defs_.find(tag);
			if (sd != struct_defs_.end() && idx < sd->second->members.size())
				field_ty = to_llvm_alloca_type(sd->second->members[idx].type);
			// 数组/struct 成员返回地址（decay），不 load
			if (field_ty->isArrayTy() || field_ty->isStructTy())
				return addr;
			llvm::Value *mv = builder.CreateLoad(field_ty, addr);
			// 整型提升: 所有 <i32 整数扩展到 i32
			if (field_ty->isIntegerTy() && field_ty->getIntegerBitWidth() < 32)
			{
				if (field_ty == llvm::Type::getInt1Ty(ctx))
					return builder.CreateZExt(mv, llvm::Type::getInt32Ty(ctx));
				// 检查成员类型是否 unsigned _BitInt
				auto sd_it = struct_defs_.find(tag);
				if (sd_it != struct_defs_.end() && idx < sd_it->second->members.size())
				{
					const CTypePtr &mt = sd_it->second->members[idx].type;
					if (mt && is_unsigned_int_type(mt))
						return builder.CreateZExt(mv, llvm::Type::getInt32Ty(ctx));
				}
				return builder.CreateSExt(mv, llvm::Type::getInt32Ty(ctx));
			}
			return mv;
		}
		case ExprKind::InitList:
		{
			// 复合字面量 (type){vals} → alloca + store + return ptr
			if (!e->type)
				return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
			llvm::Type *ty = to_llvm_alloca_type(e->type);
			llvm::Value *addr = builder.CreateAlloca(ty, nullptr, ".cl");
			// 零填（指定初始化器可能跳过位置）
			if (ty->isArrayTy() || ty->isStructTy())
			{
				uint64_t sz = module->getDataLayout().getTypeAllocSize(ty);
				builder.CreateMemSet(addr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0), sz, llvm::MaybeAlign(1));
			}
			emit_init_list_store(e, addr, ty);
			// 标量类型返回加载值，聚合返回指针
			if (ty->isArrayTy() || ty->isStructTy())
				return addr;
			return builder.CreateLoad(ty, addr);
		}
		case ExprKind::Generic:
		{
			// _Generic: 编译期按控制表达式类型（经 lvalue 转换 + 秩保持）选择关联分支；仅发射命中分支，控制表达式不求值。
			const ExprPtr *assoc = generic_select(e);
			if (assoc) return gen_expr((*assoc)->rhs);
			errs.push_back(Error{e->loc.file, Pos{e->loc.line, e->loc.column}, "_Generic: no matching type and no default"});
			return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
		}
		default:
			return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
		}
	}

	// 某 CType 是否为「无符号整数」（wchar_t 恒无符号；char/short/int/long/long long/_BitInt 看 is_unsigned）
	static bool is_unsigned_int_type(const CTypePtr &t)
	{
		if (!t) return false;
		switch (t->kind)
		{
		case TypeKind::WChar: case TypeKind::Char8: case TypeKind::Char16: case TypeKind::Char32: return true;
		case TypeKind::BitInt: case TypeKind::Char: case TypeKind::Short: case TypeKind::Int:
		case TypeKind::Long: case TypeKind::LongLong:
			return t->is_unsigned;
		default:
			return false;
		}
	}

	// IntPromo/int_promo_of/int_uac/int_ctype_of：P140 起移至 type_resolver.h
	// 把整数 Value 调到目标位宽：按源实际符号选 sext/zext（bool i1→zext，宽者截断）
	llvm::Value *widen_int(llvm::Value *v, unsigned tgt_bits, bool src_unsigned)
	{
		llvm::Type *t = v->getType();
		if (t->isPointerTy())
			return builder.CreatePtrToInt(v, llvm::IntegerType::get(ctx, tgt_bits));
		unsigned w = t->isIntegerTy() ? t->getIntegerBitWidth() : 32;
		if (w == tgt_bits) return v;
		llvm::Type *dty = llvm::IntegerType::get(ctx, tgt_bits);
		if (w > tgt_bits) return builder.CreateTrunc(v, dty);
		if (w == 1) return builder.CreateZExt(v, dty);
		return src_unsigned ? builder.CreateZExt(v, dty) : builder.CreateSExt(v, dty);
	}

	// —— 二元运算符 ——
	// C 常用算术转换（§6.3.2.8）整数分支：把两侧提升到较宽整型（至少 i32；指针按 64 位）。
	// 窄者符号扩展、宽者截断；i32↔i32 保持不变（不改变现有整数代码的 IR）。
	std::pair<llvm::Value *, llvm::Value *> usual_arith_int(llvm::Value *l, llvm::Value *r)
	{
		auto width_of = [](llvm::Value *v) -> unsigned {
			llvm::Type *t = v->getType();
			if (t->isPointerTy()) return 64;
			if (t->isIntegerTy()) return t->getIntegerBitWidth();
			return 32;
		};
		unsigned lw = width_of(l), rw = width_of(r);
		unsigned cw = lw > rw ? lw : rw;
		if (cw < 32) cw = 32;
		llvm::Type *cty = llvm::IntegerType::get(ctx, cw);
		auto conv = [&](llvm::Value *v) -> llvm::Value * {
			llvm::Type *t = v->getType();
			if (t == cty) return v;
			if (t->isPointerTy()) return builder.CreatePtrToInt(v, cty);
			if (t->isIntegerTy())
			{
				unsigned w = t->getIntegerBitWidth();
				if (w < cw) return builder.CreateSExt(v, cty);
				return builder.CreateTrunc(v, cty);
			}
			return v;
		};
		return { conv(l), conv(r) };
	}

	llvm::Value *gen_binary(const ExprPtr &e)
	{
		// 数组下标：ptr + idx -> load
		if (e->op == "[]")
		{
			llvm::Value *ptr = gen_expr(e->lhs);
			llvm::Value *idx = gen_expr(e->rhs);
			idx = to_index(idx);
			// 确定元素类型
			llvm::Type *elem_ty = llvm::Type::getInt32Ty(ctx);
			CTypePtr lhs_t = get_expr_ctype(e->lhs);
			if (lhs_t && (lhs_t->kind == TypeKind::Array || lhs_t->kind == TypeKind::Pointer))
				elem_ty = to_llvm_alloca_type(lhs_t->base);
			llvm::Value *addr = builder.CreateGEP(elem_ty, ptr, idx);
			// 数组/结构体元素不 load，返回地址（数组退化为指针）
			if (elem_ty->isArrayTy() || elem_ty->isStructTy())
				return addr;
			return builder.CreateLoad(elem_ty, addr);
		}
		// 短路求值 && / ||（内部自行求值左右，不可提前调用 gen_expr）
		if (e->op == "&&")
			return gen_short_circuit(e, true);
		if (e->op == "||")
			return gen_short_circuit(e, false);
		// 普通二元：求值两侧，根据类型选择 FP/INT 路径
		llvm::Value *l_raw = gen_expr(e->lhs);
		llvm::Value *r_raw = gen_expr(e->rhs);
		bool is_fp = l_raw->getType()->isFloatingPointTy() ||
					r_raw->getType()->isFloatingPointTy();
		const std::string &op = e->op;
		if (is_fp)
		{
			// 统一提升到 double
			llvm::Type *dty = llvm::Type::getDoubleTy(ctx);
			llvm::Value *l = to_fp(l_raw, dty);
			llvm::Value *r = to_fp(r_raw, dty);
			if (op == "+") return builder.CreateFAdd(l, r);
			if (op == "-") return builder.CreateFSub(l, r);
			if (op == "*") return builder.CreateFMul(l, r);
			if (op == "/") return builder.CreateFDiv(l, r);
			if (op == "%") return builder.CreateFRem(l, r);
			if (op == "==") return builder.CreateFCmpOEQ(l, r);
			// C §6.5.9/IEEE-754：!= 须取「无序非等」——任一操作数为 NaN 时结果为真（NaN != NaN）。one 会判假（CTS 0099 NaN 族）。
			if (op == "!=") return builder.CreateFCmpUNE(l, r);
			if (op == "<") return builder.CreateFCmpOLT(l, r);
			if (op == "<=") return builder.CreateFCmpOLE(l, r);
			if (op == ">") return builder.CreateFCmpOGT(l, r);
			if (op == ">=") return builder.CreateFCmpOGE(l, r);
			return l;
		}
		// 指针算术: ptr + N / ptr - N / ptr - ptr → 元素差
		if ((op == "+" || op == "-") &&
			(l_raw->getType()->isPointerTy() || r_raw->getType()->isPointerTy()))
		{
			// ptr - ptr: 指针差，返回元素个数
			if (op == "-" && l_raw->getType()->isPointerTy() && r_raw->getType()->isPointerTy())
			{
				llvm::Type *i64ty = llvm::Type::getInt64Ty(ctx);
				llvm::Value *li = builder.CreatePtrToInt(l_raw, i64ty);
				llvm::Value *ri = builder.CreatePtrToInt(r_raw, i64ty);
				llvm::Value *diff = builder.CreateSub(li, ri);
				// 除以元素大小
				CTypePtr lt = get_expr_ctype(e->lhs);
				unsigned elem_sz = 4; // default int
				// 数组名（TypeKind::Array）与指针（TypeKind::Pointer）皆取 base 作元素类型；
				// 步长须与 GEP/下标一致，用元素类型的 getTypeAllocSize（含 padding struct 的显式尾补）
				if (lt && (lt->kind == TypeKind::Pointer || lt->kind == TypeKind::Array) && lt->base)
				{
					llvm::Type *ety = to_llvm_alloca_type(lt->base);
					elem_sz = (unsigned)module->getDataLayout().getTypeAllocSize(ety);
				}
				if (elem_sz > 1)
					diff = builder.CreateSDiv(diff, llvm::ConstantInt::get(i64ty, elem_sz));
				return builder.CreateTrunc(diff, llvm::Type::getInt32Ty(ctx));
			}
			llvm::Value *ptr_val = nullptr;
			llvm::Value *idx_val = nullptr;
			CTypePtr elem_ct;
			if (l_raw->getType()->isPointerTy())
			{
				ptr_val = l_raw; idx_val = to_i32(r_raw);
				CTypePtr lt = get_expr_ctype(e->lhs);
				if (lt && (lt->kind == TypeKind::Pointer || lt->kind == TypeKind::Array)) elem_ct = lt->base;
			}
			else
			{
				ptr_val = r_raw; idx_val = to_i32(l_raw);
				CTypePtr rt = get_expr_ctype(e->rhs);
				if (rt && (rt->kind == TypeKind::Pointer || rt->kind == TypeKind::Array)) elem_ct = rt->base;
			}
			if (op == "-")
			{
				llvm::Value *neg = builder.CreateSub(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), idx_val);
				idx_val = neg;
			}
			llvm::Type *ety = elem_ct ? to_llvm_alloca_type(elem_ct) : llvm::Type::getInt32Ty(ctx);
			return builder.CreateGEP(ety, ptr_val, to_index(idx_val));
		}
		// 指针参与（比较等）：沿用旧宽度基路径，保持既有 IR
		if (l_raw->getType()->isPointerTy() || r_raw->getType()->isPointerTy())
		{
			auto pr = usual_arith_int(l_raw, r_raw);
			llvm::Value *l = pr.first;
			llvm::Value *r = pr.second;
			llvm::Value *cmp = nullptr;
			if (op == "==") cmp = builder.CreateICmpEQ(l, r);
			else if (op == "!=") cmp = builder.CreateICmpNE(l, r);
			else if (op == "<") cmp = builder.CreateICmpSLT(l, r);
			else if (op == "<=") cmp = builder.CreateICmpSLE(l, r);
			else if (op == ">") cmp = builder.CreateICmpSGT(l, r);
			else if (op == ">=") cmp = builder.CreateICmpSGE(l, r);
			if (cmp) return builder.CreateZExt(cmp, llvm::Type::getInt32Ty(ctx));
			return l;
		}
		// 整数路径：按操作数 C 类型做整型提升 + 常用算术转换（§6.3.2.8），据符号选有/无符号指令
		IntPromo lp = int_promo_of(get_expr_ctype(e->lhs));
		IntPromo rp = int_promo_of(get_expr_ctype(e->rhs));
		bool lext = is_unsigned_int_type(get_expr_ctype(e->lhs));
		bool rext = is_unsigned_int_type(get_expr_ctype(e->rhs));
		bool is_shift = (op == "<<" || op == ">>");
		// 移位只按左操作数提升类型定宽度与符号；其余走常用算术转换
		IntPromo res = is_shift ? lp : int_uac(lp, rp);
		unsigned cw = res.bits < 32 ? 32u : res.bits;
		bool u = res.is_unsigned;
		llvm::Value *l = widen_int(l_raw, cw, lext);
		llvm::Value *r = widen_int(r_raw, cw, rext);
		if (op == "+") return builder.CreateAdd(l, r);
		if (op == "-") return builder.CreateSub(l, r);
		if (op == "*") return builder.CreateMul(l, r);
		if (op == "/") return u ? builder.CreateUDiv(l, r) : builder.CreateSDiv(l, r);
		if (op == "%") return u ? builder.CreateURem(l, r) : builder.CreateSRem(l, r);
		if (op == "<<") return builder.CreateShl(l, r);
		if (op == ">>") return u ? builder.CreateLShr(l, r) : builder.CreateAShr(l, r);
		if (op == "&") return builder.CreateAnd(l, r);
		if (op == "|") return builder.CreateOr(l, r);
		if (op == "^") return builder.CreateXor(l, r);
		// 比较（返回 i1，提升为 i32）
		llvm::Value *cmp = nullptr;
		if (op == "==") cmp = builder.CreateICmpEQ(l, r);
		else if (op == "!=") cmp = builder.CreateICmpNE(l, r);
		else if (op == "<") cmp = u ? builder.CreateICmpULT(l, r) : builder.CreateICmpSLT(l, r);
		else if (op == "<=") cmp = u ? builder.CreateICmpULE(l, r) : builder.CreateICmpSLE(l, r);
		else if (op == ">") cmp = u ? builder.CreateICmpUGT(l, r) : builder.CreateICmpSGT(l, r);
		else if (op == ">=") cmp = u ? builder.CreateICmpUGE(l, r) : builder.CreateICmpSGE(l, r);
		if (cmp) return builder.CreateZExt(cmp, llvm::Type::getInt32Ty(ctx));
		return l;
	}

	// —— 一元运算符 ——
	llvm::Value *gen_unary(const ExprPtr &e)
	{
		if (e->op == "&")
		{
			// 取地址（lvalue -> pointer）
			return gen_lvalue_addr(e->lhs);
		}
		if (e->op == "*")
		{
			// 解引用：根据指针元素类型确定 load 宽度
			llvm::Value *ptr = gen_expr(e->lhs);
			CTypePtr pt = get_expr_ctype(e->lhs);
			llvm::Type *load_ty = llvm::Type::getInt32Ty(ctx);
			if (pt && pt->kind == TypeKind::Pointer && pt->base)
			{
				// 函数指针: *fp 返回指针本身（不 load）
				if (pt->base->kind == TypeKind::Function)
					return ptr;
				// struct/union/array 的解引用: 返回地址（不 load）
				if (pt->base->kind == TypeKind::Struct || pt->base->kind == TypeKind::Union || pt->base->kind == TypeKind::Array)
					return ptr;
				load_ty = to_llvm_alloca_type(pt->base);
			}
			return builder.CreateLoad(load_ty, ptr);
		}
		if (e->op == "sizeof")
		{
			const auto &DL = module->getDataLayout();
			if (e->type) // sizeof(type)
			{
				llvm::Type *ty = to_llvm_alloca_type(e->type);
				return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx),
					static_cast<uint64_t>(DL.getTypeStoreSize(ty)));
			}
			else if (e->lhs) // sizeof(expr)
			{
				CTypePtr et = get_expr_ctype(e->lhs);
				llvm::Type *ty = et ? to_llvm_alloca_type(et) : llvm::Type::getInt32Ty(ctx);
				return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx),
					static_cast<uint64_t>(DL.getTypeStoreSize(ty)));
			}
			return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 4);
		}
		if (e->op == "alignof")
		{
			// _Alignof：反映 _Alignas（struct_align_/CType::align/数组递归）与自然对齐的 max
			CTypePtr at = e->type ? e->type : (e->lhs ? get_expr_ctype(e->lhs) : nullptr);
			uint64_t align = at
				? type_effective_align(at)
				: static_cast<uint64_t>(module->getDataLayout().getABITypeAlign(llvm::Type::getInt32Ty(ctx)).value());
			return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), align);
		}
		// ++ / -- 自增/自减（前缀/后缀）
		if (e->op == "++" || e->op == "--")
		{
			llvm::Value *addr = gen_lvalue_addr(e->lhs);
			CTypePtr ct = get_expr_ctype(e->lhs);
			bool is_ptr = (ct && ct->kind == TypeKind::Pointer);
			llvm::Value *old;
			llvm::Value *nv;
			if (is_ptr)
			{
				// 指针自增: load ptr → GEP ±1
				llvm::Type *ptr_ty = llvm::PointerType::getUnqual(ctx);
				old = builder.CreateLoad(ptr_ty, addr);
				llvm::Value *one = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1);
				if (e->op == "--")
					one = builder.CreateSub(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), one);
				llvm::Type *ety = ct->base ? to_llvm_alloca_type(ct->base) : llvm::Type::getInt32Ty(ctx);
				nv = builder.CreateGEP(ety, old, to_index(one));
				builder.CreateStore(nv, addr);
			}
			else
			{
				old = to_i32(builder.CreateLoad(llvm::Type::getInt32Ty(ctx), addr));
				llvm::Value *one = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1);
				nv = (e->op == "++")
					? builder.CreateAdd(old, one)
					: builder.CreateSub(old, one);
				builder.CreateStore(nv, addr);
			}
			return e->postfix ? old : nv;
		}
		// 一元 +/-：浮点操作数走 FNeg/直通；整型按 C 整型提升（§6.3.1.1）——窄整型（含
		// unsigned char/short）先据其**实际符号性** widen_int 提升到 int/long（无符号零扩展、
		// 有符号符号扩展），再 0-sub/直通。此前用 to_i32 一律符号扩展，把 unsigned short 的
		// 0xFFFF 误当 -1，致 -(unsigned short)(-1) 判非负、PTYPE 符号错（c-testsuite 00200）。
		if (e->op == "-" || e->op == "+")
		{
			llvm::Value *raw = gen_expr(e->lhs);
			if (raw->getType()->isFloatingPointTy())
				return (e->op == "-") ? builder.CreateFNeg(raw) : raw;
			CTypePtr ot = get_expr_ctype(e->lhs);
			IntPromo p = int_promo_of(ot);
			unsigned tgt = p.bits < 32 ? 32u : p.bits;
			llvm::Value *iv = widen_int(raw, tgt, is_unsigned_int_type(ot));
			if (e->op == "-") return builder.CreateSub(llvm::ConstantInt::get(iv->getType(), 0), iv);
			return iv;
		}
		llvm::Value *v = to_i32(gen_expr(e->lhs));
		if (e->op == "!")
		{
			llvm::Value *c = gen_cond(e->lhs);
			return builder.CreateXor(c, llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), 1));
		}
		if (e->op == "~") return builder.CreateXor(v, llvm::ConstantInt::get(v->getType(), static_cast<uint64_t>(-1), true));
		return v;
	}

	// 发射 llvm.memcpy
	void emit_memcpy(llvm::Value *dst, llvm::Value *src, uint64_t size)
	{
		llvm::FunctionCallee fc = module->getOrInsertFunction("llvm.memcpy.p0.p0.i64",
			llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
				{llvm::PointerType::getUnqual(ctx), llvm::PointerType::getUnqual(ctx),
					llvm::Type::getInt64Ty(ctx), llvm::Type::getInt1Ty(ctx)}, false));
		llvm::Value *sz = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), size);
		llvm::Value *vol = llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), 0);
		builder.CreateCall(fc, {dst, src, sz, vol});
	}

	// —— 赋值 ——
	llvm::Value *gen_assign(const ExprPtr &e)
	{
		// struct/union 整体赋值: dst = src → memcpy
		CTypePtr lhs_t = get_expr_ctype(e->lhs);
		if (lhs_t && (lhs_t->kind == TypeKind::Struct || lhs_t->kind == TypeKind::Union))
		{
			llvm::Value *dst = gen_lvalue_addr(e->lhs);
			llvm::Value *src = gen_lvalue_addr(e->rhs);
			const auto &DL = module->getDataLayout();
			llvm::Type *sty = to_llvm_alloca_type(lhs_t);
			uint64_t sz = DL.getTypeStoreSize(sty);
			emit_memcpy(dst, src, sz);
			return dst;
		}
		// 位域写访问: read-modify-write
		if (e->lhs->kind == ExprKind::Member)
		{
			MemberInfo mi2 = resolve_member_info(e->lhs);
			std::string bftag = mi2.decl_tag;
			get_struct_type(bftag); // 确保布局已建立
			std::string bf_key = bftag + "." + e->lhs->name;
			auto bf_it = bitfield_access_.find(bf_key);
			if (bf_it != bitfield_access_.end())
			{
				const auto &ba = bf_it->second;
				llvm::Type *stor_ty = get_int_ty_for_bits(ba.unit_bits);
				llvm::Value *stor_addr = gen_member_addr(e->lhs);
				llvm::Value *rhs_val = gen_expr(e->rhs);
				// 复合赋值: 先读旧位域值
				if (e->op != "=")
				{
					std::string base_op = e->op.substr(0, e->op.size() - 1);
					// 读当前位域值
					llvm::Value *cur = builder.CreateLoad(stor_ty, stor_addr);
					if (ba.bit_offset > 0)
						cur = builder.CreateLShr(cur, llvm::ConstantInt::get(stor_ty, ba.bit_offset));
					if (ba.bit_width < ba.unit_bits)
						cur = builder.CreateAnd(cur, llvm::ConstantInt::get(stor_ty, (1ULL << ba.bit_width) - 1));
					llvm::Value *old32 = cur;
					if (stor_ty != llvm::Type::getInt32Ty(ctx))
						old32 = builder.CreateZExt(cur, llvm::Type::getInt32Ty(ctx));
					rhs_val = to_i32(rhs_val); old32 = to_i32(old32);
					if (base_op == "+") rhs_val = builder.CreateAdd(old32, rhs_val);
					else if (base_op == "-") rhs_val = builder.CreateSub(old32, rhs_val);
					else if (base_op == "*") rhs_val = builder.CreateMul(old32, rhs_val);
					else if (base_op == "/") rhs_val = builder.CreateSDiv(old32, rhs_val);
					else if (base_op == "&") rhs_val = builder.CreateAnd(old32, rhs_val);
					else if (base_op == "|") rhs_val = builder.CreateOr(old32, rhs_val);
					else if (base_op == "^") rhs_val = builder.CreateXor(old32, rhs_val);
					else if (base_op == "<<") rhs_val = builder.CreateShl(old32, rhs_val);
					else if (base_op == ">>") rhs_val = builder.CreateAShr(old32, rhs_val);
				}
				else
					rhs_val = to_i32(rhs_val);
				// 截断 rhs 到位域宽度
				if (ba.bit_width < 32)
				{
					uint64_t mask = (1ULL << ba.bit_width) - 1;
					rhs_val = builder.CreateAnd(rhs_val,
						llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), mask));
				}
				// 扩展到存储单元宽度
				if (stor_ty != llvm::Type::getInt32Ty(ctx))
					rhs_val = builder.CreateTrunc(rhs_val, stor_ty);
				// 移位
				llvm::Value *shifted = rhs_val;
				if (ba.bit_offset > 0)
					shifted = builder.CreateShl(shifted, llvm::ConstantInt::get(stor_ty, ba.bit_offset));
				// 清除旧位
				llvm::Value *cur = builder.CreateLoad(stor_ty, stor_addr);
				uint64_t clear_mask = ~(((1ULL << ba.bit_width) - 1) << ba.bit_offset);
				llvm::Value *cm = llvm::ConstantInt::get(stor_ty, clear_mask);
				cur = builder.CreateAnd(cur, cm);
				// OR 新值
				cur = builder.CreateOr(cur, shifted);
				builder.CreateStore(cur, stor_addr);
				return rhs_val;
			}
		}
		llvm::Value *rhs = gen_expr(e->rhs);
		llvm::Value *addr = gen_lvalue_addr(e->lhs);
		// 复合赋值 op=：先 load 旧值，做运算
		if (e->op != "=")
		{
			std::string base_op = e->op.substr(0, e->op.size() - 1); // "+=" -> "+"
			// 指针复合赋值: p += N / p -= N → GEP
			CTypePtr lhs_ct = get_expr_ctype(e->lhs);
			if (lhs_ct && lhs_ct->kind == TypeKind::Pointer &&
				(base_op == "+" || base_op == "-"))
			{
				llvm::Type *ptr_ty = llvm::PointerType::getUnqual(ctx);
				llvm::Value *lhs_val = builder.CreateLoad(ptr_ty, addr);
				llvm::Value *idx = to_i32(rhs);
				if (base_op == "-")
					idx = builder.CreateSub(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), idx);
				llvm::Type *ety = (lhs_ct->base) ? to_llvm_alloca_type(lhs_ct->base) : llvm::Type::getInt32Ty(ctx);
				rhs = builder.CreateGEP(ety, lhs_val, to_index(idx));
			}
			else
			{
				// 确定 load 类型：按左值实际 CType（覆盖 double/float/long long/int/char）
				llvm::Type *ld_ty = llvm::Type::getInt32Ty(ctx);
				if (lhs_ct)
				{
					if (lhs_ct->kind == TypeKind::Pointer)
						ld_ty = llvm::PointerType::getUnqual(ctx);
					else
						ld_ty = to_llvm_alloca_type(lhs_ct);
				}
				llvm::Value *lhs_val = builder.CreateLoad(ld_ty, addr);
				if (ld_ty->isPointerTy())
				{
					// 指针整数运算回退（不应到达这里）
					rhs = lhs_val;
				}
				else if (ld_ty->isFloatingPointTy())
				{
					// 浮点复合赋值：按 double 运算（与 gen_binary 一致），右侧 double 操作数不再被误截到 float；
					// 存回由函数尾部 convert_value(rhs, stor_ty) 单次截回左值真实存储类型（float）（修 plct 00174）
					llvm::Type *dty = llvm::Type::getDoubleTy(ctx);
					llvm::Value *a = to_fp(lhs_val, dty);
					llvm::Value *b = to_fp(rhs, dty);
					if (base_op == "+") rhs = builder.CreateFAdd(a, b);
					else if (base_op == "-") rhs = builder.CreateFSub(a, b);
					else if (base_op == "*") rhs = builder.CreateFMul(a, b);
					else if (base_op == "/") rhs = builder.CreateFDiv(a, b);
					else if (base_op == "%") rhs = builder.CreateFRem(a, b);
				}
				else
				{
					// 整数复合赋值：按左/右 C 类型做常用算术转换，据符号选指令
					IntPromo lp = int_promo_of(lhs_ct);
					IntPromo rp = int_promo_of(get_expr_ctype(e->rhs));
					bool lext = is_unsigned_int_type(lhs_ct);
					bool rext = is_unsigned_int_type(get_expr_ctype(e->rhs));
					bool is_shift = (base_op == "<<" || base_op == ">>");
					IntPromo res = is_shift ? lp : int_uac(lp, rp);
					unsigned cw = res.bits < 32 ? 32u : res.bits;
					bool u = res.is_unsigned;
					llvm::Value *a = widen_int(lhs_val, cw, lext);
					llvm::Value *b = widen_int(rhs, cw, rext);
					if (base_op == "+") rhs = builder.CreateAdd(a, b);
					else if (base_op == "-") rhs = builder.CreateSub(a, b);
					else if (base_op == "*") rhs = builder.CreateMul(a, b);
					else if (base_op == "/") rhs = u ? builder.CreateUDiv(a, b) : builder.CreateSDiv(a, b);
					else if (base_op == "%") rhs = u ? builder.CreateURem(a, b) : builder.CreateSRem(a, b);
					else if (base_op == "&") rhs = builder.CreateAnd(a, b);
					else if (base_op == "|") rhs = builder.CreateOr(a, b);
					else if (base_op == "^") rhs = builder.CreateXor(a, b);
					else if (base_op == "<<") rhs = builder.CreateShl(a, b);
					else if (base_op == ">>") rhs = u ? builder.CreateLShr(a, b) : builder.CreateAShr(a, b);
				}
			}
		}
		// 存储：根据 lvalue 类型转换
		//   优先按左值 C 类型定存储宽度——覆盖「经转型指针解引用标量」的位拼装：
		//   *(long long*)&(double 变量) 的 store 须按 long long/i64，不得退回 alloca 的声明类型 double
		//   （否则整数常量被 SIToFP、毁掉 IEEE 位模式——CTS 0099 NaN 族标量位拼装）。聚合类型
		//   （struct/union/array/func，多已由前面 memcpy/早返回处理）与 C 类型不可用时回退 alloca/global。
		llvm::Type *stor_ty = nullptr;
		CTypePtr lhs_st = get_expr_ctype(e->lhs);
		if (lhs_st && lhs_st->kind != TypeKind::Struct && lhs_st->kind != TypeKind::Union &&
			lhs_st->kind != TypeKind::Array && lhs_st->kind != TypeKind::Function)
			stor_ty = to_llvm_alloca_type(lhs_st);
		if (!stor_ty)
		{
			if (auto *al = llvm::dyn_cast<llvm::AllocaInst>(addr))
				stor_ty = al->getAllocatedType();
			else if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(addr))
				stor_ty = gv->getValueType();
		}
		if (stor_ty && stor_ty->isFloatingPointTy())
			rhs = convert_value(rhs, stor_ty, get_expr_ctype(e->rhs));
		else if (stor_ty && stor_ty->isPointerTy())
		{
			// 指针赋值
			if (rhs->getType()->isIntegerTy())
			{
				// 整数 0 → null ptr; 其他整数 → IntToPtr
				if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(rhs))
				{
					if (ci->isZero())
						rhs = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(stor_ty));
					else
						rhs = builder.CreateIntToPtr(rhs, stor_ty);
				}
				else
					rhs = builder.CreateIntToPtr(rhs, stor_ty);
			}
			// ptr→ptr 在 opaque pointer 下无需转换
		}
		else if (stor_ty && (stor_ty->isArrayTy() || stor_ty->isStructTy()))
			; // 不会走到这里（struct赋值已前面处理）
		else if (stor_ty && stor_ty->isIntegerTy() && stor_ty != llvm::Type::getInt32Ty(ctx))
			rhs = convert_value(rhs, stor_ty, get_expr_ctype(e->rhs));
		else
			rhs = to_i32(rhs);
		builder.CreateStore(rhs, addr);
		return rhs;
	}

	// 获取 lvalue 的地址（alloca pointer / GlobalVariable ptr）
	llvm::Value *gen_lvalue_addr(const ExprPtr &e)
	{
		if (e->kind == ExprKind::VarRef)
		{
			auto it = vars_.find(e->name);
			if (it != vars_.end())
				return it->second;
			auto git = globals_.find(e->name);
			if (git != globals_.end())
				return git->second;
		}
		// 解引用: *ptr -> 地址就是 ptr
		if (e->kind == ExprKind::Unary && e->op == "*")
			return gen_expr(e->lhs);
		// 数组下标: arr[idx] -> GEP
		if (e->kind == ExprKind::Binary && e->op == "[]")
		{
			llvm::Value *ptr = gen_expr(e->lhs);
			llvm::Value *idx = to_index(gen_expr(e->rhs));
			// 确定元素类型
			llvm::Type *elem_ty = llvm::Type::getInt32Ty(ctx);
			CTypePtr lhs_t = get_expr_ctype(e->lhs);
			if (lhs_t)
			{
				if (lhs_t->kind == TypeKind::Array || lhs_t->kind == TypeKind::Pointer)
					elem_ty = to_llvm_alloca_type(lhs_t->base);
			}
			return builder.CreateGEP(elem_ty, ptr, idx);
		}
		// 成员访问: obj.field / ptr->field
		if (e->kind == ExprKind::Member)
			return gen_member_addr(e);
		// fallback
		return gen_expr(e);
	}

	// ---- _Generic 类型选择支持（秩保持 + lvalue 转换 + 关联选择）----
	//   仅 _Generic 使用；不复用宽度塌缩的 resolve_expr_type（LLP64 下 long==int==32 会丢秩），
	//   以免扰动中央 codegen 路径。

	// 秩保持整型秩：int=1 < long=2 < long long=3（比 int 窄者先提升为 int）。
	static int gc_int_rank(const CTypePtr &t)
	{
		if (!t) return 1;
		switch (t->kind)
		{
		case TypeKind::Int: return 1;
		case TypeKind::Long: return 2;
		case TypeKind::LongLong: return 3;
		default: return 1;
		}
	}
	// C 整型提升：char/short/_Bool/wchar_t/enum 归 int（int 可表其全部值）。
	static CTypePtr gc_promote(const CTypePtr &t)
	{
		if (!t) return make_type(TypeKind::Int);
		switch (t->kind)
		{
		case TypeKind::Bool: case TypeKind::Char: case TypeKind::Short:
		case TypeKind::WChar: case TypeKind::Enum:
		case TypeKind::Char8: case TypeKind::Char16:
			return make_type(TypeKind::Int);
		case TypeKind::Char32:  // uint32 值域 int 表不下 → 提升为 unsigned int
			{ auto pt = make_type(TypeKind::Int); pt->is_unsigned = true; return pt; }
		default:
			return t;
		}
	}
	// §6.3.1.8 常用算术转换（整型，秩保持）：结果保 Long/LongLong 不塌 Int。
	static CTypePtr gc_usual_arith(const CTypePtr &a, const CTypePtr &b)
	{
		if (!a || !b) return make_type(TypeKind::Int);
		CTypePtr x = gc_promote(a), y = gc_promote(b);
		bool xfp = x->kind == TypeKind::Float || x->kind == TypeKind::Double;
		bool yfp = y->kind == TypeKind::Float || y->kind == TypeKind::Double;
		if (xfp || yfp)
		{
			if (x->kind == TypeKind::Double || y->kind == TypeKind::Double) return make_type(TypeKind::Double);
			return make_type(TypeKind::Float);
		}
		int xr = gc_int_rank(x), yr = gc_int_rank(y);
		bool xu = x->is_unsigned, yu = y->is_unsigned;
		int rmax = xr > yr ? xr : yr;
		TypeKind rk = rmax == 1 ? TypeKind::Int : rmax == 2 ? TypeKind::Long : TypeKind::LongLong;
		bool uns;
		if (xu == yu) uns = xu;
		else
		{
			int ur = xu ? xr : yr;
			int sr = xu ? yr : xr;
			uns = (ur >= sr);
		}
		auto t = make_type(rk);
		t->is_unsigned = uns;
		return t;
	}
	// lvalue 转换：数组→指针、函数→函数指针、剥顶层 const（不改动共享 CType，返回副本）。
	static CTypePtr gc_lvalue_convert(const CTypePtr &t)
	{
		if (!t) return nullptr;
		if (t->kind == TypeKind::Array) return make_pointer(t->base);
		if (t->kind == TypeKind::Function) return make_pointer(t);
		if (t->is_const)
		{
			auto c = std::make_shared<CType>(*t);
			c->is_const = false;
			return c;
		}
		return t;
	}
	// 控制表达式 → 经 lvalue 转换的真实 C 类型（整型按秩保 Long/LongLong）。
	CTypePtr generic_controlling_type(const ExprPtr &e)
	{
		if (!e) return nullptr;
		switch (e->kind)
		{
		case ExprKind::IntLiteral:
		case ExprKind::CharLiteral:
			if (e->type && (e->type->kind == TypeKind::LongLong || e->type->kind == TypeKind::Long ||
				e->type->kind == TypeKind::Int || e->type->kind == TypeKind::BitInt))
				return e->type;
			return make_type(TypeKind::Int);
		case ExprKind::FloatLiteral:
			if (e->type && e->type->kind == TypeKind::Float) return e->type;
			return make_type(TypeKind::Double);
		case ExprKind::StringLiteral:
			return make_pointer(make_type(TypeKind::Char)); // char[N] 经 lvalue → char*
		case ExprKind::VarRef:
		{
			auto vt = var_types_.find(e->name);
			if (vt != var_types_.end() && vt->second) return gc_lvalue_convert(vt->second);
			auto gt = global_types_.find(e->name);
			if (gt != global_types_.end() && gt->second) return gc_lvalue_convert(gt->second);
			auto ft = func_cftypes_.find(e->name);
			if (ft != func_cftypes_.end()) return make_pointer(ft->second); // 函数名衰减为函数指针
			return nullptr;
		}
		case ExprKind::Cast:
			return gc_lvalue_convert(e->type);
		case ExprKind::Unary:
		{
			if (e->op == "sizeof" || e->op == "alignof") { auto t = make_type(TypeKind::LongLong); t->is_unsigned = true; return t; }
			if (e->op == "!") return make_type(TypeKind::Int);
			if (e->op == "&") { auto t = generic_controlling_type(e->lhs); return t ? make_pointer(t) : nullptr; }
			if (e->op == "*") { auto t = generic_controlling_type(e->lhs); if (t && t->kind == TypeKind::Pointer) return gc_lvalue_convert(t->base); return nullptr; }
			if (e->op == "+" || e->op == "-" || e->op == "~") { auto t = generic_controlling_type(e->lhs); return t ? gc_promote(t) : nullptr; }
			return nullptr;
		}
		case ExprKind::Binary:
		{
			const std::string &op = e->op;
			if (op == ",") return generic_controlling_type(e->rhs);
			if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" ||
				op == "&&" || op == "||")
				return make_type(TypeKind::Int);
			CTypePtr lt = generic_controlling_type(e->lhs);
			CTypePtr rt = generic_controlling_type(e->rhs);
			if (op == "<<" || op == ">>") return lt ? gc_promote(lt) : nullptr;
			bool lp = lt && (lt->kind == TypeKind::Pointer || lt->kind == TypeKind::Array);
			bool rp = rt && (rt->kind == TypeKind::Pointer || rt->kind == TypeKind::Array);
			if (lp || rp)
			{
				if (op == "-" && lp && rp) { auto t = make_type(TypeKind::LongLong); t->is_unsigned = false; return t; } // ptrdiff_t
				return lp ? lt : rt; // ptr+int → ptr
			}
			return gc_usual_arith(lt, rt);
		}
		default:
			return nullptr;
		}
	}
	// 扫描关联：命中兼容项则返回，否则回 default 项（均无 → nullptr）。
	const ExprPtr *generic_select(const ExprPtr &g)
	{
		CTypePtr sel = generic_controlling_type(g->lhs);
		const ExprPtr *def = nullptr;
		for (const auto &assoc : g->args)
		{
			if (!assoc->type) { def = &assoc; continue; }
			if (sel && types_compatible(assoc->type, sel)) return &assoc;
		}
		return def;
	}

	// 推断表达式的 CType（用于嵌套成员/sizeof）
	//   P140 M1：算法已归一至共享解析器 type_resolver.h::resolve_expr_type（单一真值源）。
	//   此处仅保留薄封装——以本类符号表构造 CodegenTypeEnv 并委托之；47 调用点不变、行为逐字恒等。
	struct CodegenTypeEnv
	{
		CodeGenImpl *self;
		CTypePtr var_type(const std::string &n)
		{
			auto vt = self->var_types_.find(n);
			if (vt != self->var_types_.end()) return vt->second;
			auto gt = self->global_types_.find(n);
			if (gt != self->global_types_.end()) return gt->second;
			return nullptr;
		}
		CTypePtr struct_member(const std::string &tag, const std::string &field)
		{
			return struct_member_impl(tag, field, 0);
		}
		// 成员名提升：直查不中时递归穿过匿名 struct/union 成员（与 sema 侧一致）。
		CTypePtr struct_member_impl(const std::string &tag, const std::string &field, int depth)
		{
			if (depth > 16) return nullptr;
			auto sd = self->struct_defs_.find(tag);
			if (sd == self->struct_defs_.end()) return nullptr;
			for (const auto &m : sd->second->members)
				if (!m.name.empty() && m.name == field) return m.type;
			for (const auto &m : sd->second->members)
			{
				if (m.name.empty() && m.type
					&& (m.type->kind == TypeKind::Struct || m.type->kind == TypeKind::Union)
					&& !m.type->tag.empty())
				{
					if (CTypePtr r = struct_member_impl(m.type->tag, field, depth + 1))
						return r;
				}
			}
			return nullptr;
		}
		CTypePtr func_return(const std::string &n)
		{
			auto fit = self->func_types_.find(n);
			if (fit != self->func_types_.end()) return fit->second;
			return nullptr;
		}
	};
	CTypePtr get_expr_ctype(const ExprPtr &e)
	{
		// _Generic 结果类型：取命中关联项表达式的类型（经 lvalue 转换 + 衰减），使 _Generic(...)() 的间接 Call 能取到函数指针。
		if (e && e->kind == ExprKind::Generic)
		{
			const ExprPtr *a = generic_select(e);
			if (a && (*a)->rhs) return generic_controlling_type((*a)->rhs);
			return nullptr;
		}
		CodegenTypeEnv env{this};
		return resolve_expr_type(e, env);
	}

	// 成员访问解析结果：
	//   base_tag = 基表达式紧邻的容器 struct/union tag（GEP 从此出发）
	//   slot     = base_tag 中一级成员索引（命名成员本身，或含提升名的匿名 union 成员）
	//   decl_tag/decl_idx = 字段真正声明处（供类型回查与位域 key；提升时指向内部 union）
	struct MemberInfo {
		std::string base_tag;
		unsigned slot = 0;
		std::string decl_tag;
		unsigned decl_idx = 0;
	};
	// 递归定位字段：一级命中的命名成员 → slot=其索引；匿名 union 成员包裹→ slot=该匿名成员索引
	// （union 成员偏移恒 0，一次 GEP 到槽即达字段，无需逐层 GEP）。
	bool locate_member(const std::string &tag, const std::string &field, int depth,
	                   unsigned &slot, std::string &decl_tag, unsigned &decl_idx)
	{
		if (depth > 16) return false;
		auto sd = struct_defs_.find(tag);
		if (sd == struct_defs_.end()) return false;
		const auto &mem = sd->second->members;
		// 一级：直接命名成员
		for (unsigned i = 0; i < mem.size(); ++i)
			if (!mem[i].name.empty() && mem[i].name == field) {
				if (depth == 0) slot = i;
				decl_tag = tag; decl_idx = i; return true;
			}
		// 二级：穿过匿名 struct/union（union 保留嵌套，struct 已由 parser 平铺）
		for (unsigned i = 0; i < mem.size(); ++i)
		{
			if (mem[i].name.empty() && mem[i].type
				&& (mem[i].type->kind == TypeKind::Struct || mem[i].type->kind == TypeKind::Union)
				&& !mem[i].type->tag.empty())
			{
				unsigned sub_slot = 0; std::string dt; unsigned di = 0;
				if (locate_member(mem[i].type->tag, field, depth + 1, sub_slot, dt, di)) {
					if (depth == 0) slot = i;
					decl_tag = dt; decl_idx = di; return true;
				}
			}
		}
		return false;
	}

	// 解析成员访问的基础类型tag和字段索引
	MemberInfo resolve_member_info(const ExprPtr &e)
	{
		std::string tag;
		const ExprPtr &base = e->lhs;
		if (e->op == ".")
		{
			// obj.field: obj 是变量或另一个成员
			if (base->kind == ExprKind::VarRef)
			{
				auto vt = var_types_.find(base->name);
				if (vt != var_types_.end() && vt->second)
					tag = vt->second->tag;
				else
				{
					auto gt = global_types_.find(base->name);
					if (gt != global_types_.end() && gt->second)
						tag = gt->second->tag;
				}
			}
			else if (base->kind == ExprKind::Binary && base->op == "[]")
			{
				// arr[i].field 或 ptr[i].field
				CTypePtr at = get_expr_ctype(base);
				if (at) tag = at->tag;
			}
			else if (base->kind == ExprKind::Unary && base->op == "*")
			{
				// (*ptr).field
				if (base->lhs->kind == ExprKind::VarRef)
				{
					auto vt = var_types_.find(base->lhs->name);
					if (vt != var_types_.end() && vt->second && vt->second->base)
						tag = vt->second->base->tag;
				}
			}
			else if (base->kind == ExprKind::Member)
			{
				// 嵌套成员: a.b.c —— 递归解析 base 的类型
				CTypePtr bt = get_expr_ctype(base);
				if (bt) tag = bt->tag;
			}
			// 其它返回 struct 的右值基（如函数调用 fr_s().field、括号/逗号表达式）：
			//   统一回退到共享类型源取 struct tag（对齐下方 "->" 分支的兜底），避免误落空 tag 得 %struct. opaque。
			if (tag.empty())
			{
				CTypePtr bt = get_expr_ctype(base);
				if (bt) tag = bt->tag;
			}
		}
		else // "->" 
		{
			if (base->kind == ExprKind::VarRef)
			{
				auto vt = var_types_.find(base->name);
				if (vt != var_types_.end() && vt->second && vt->second->base)
					tag = vt->second->base->tag; // pointer->base = struct type
			}
			// 嵌套指针成员: a->b->c 或复杂表达式
			if (tag.empty())
			{
				CTypePtr bt = get_expr_ctype(base);
				if (bt && bt->kind == TypeKind::Pointer && bt->base)
					tag = bt->base->tag;
			}
		}
		// 定位字段（含穿过匿名 union 的名提升）
		MemberInfo mi;
		mi.base_tag = tag;
		mi.decl_tag = tag;
		unsigned slot = 0;
		if (locate_member(tag, e->name, 0, slot, mi.decl_tag, mi.decl_idx))
			mi.slot = slot;
		else
		{
			// 未命中：保持旧默认（idx 0）
			mi.slot = 0;
			mi.decl_tag = tag;
			mi.decl_idx = 0;
		}
		return mi;
	}

	// 生成成员地址 GEP
	llvm::Value *gen_member_addr(const ExprPtr &e)
	{
		MemberInfo mi = resolve_member_info(e);
		const std::string &tag = mi.base_tag;
		// union: 所有成员偏移为0，直接返回基址
		auto sd_it = struct_defs_.find(tag);
		bool is_union = (sd_it != struct_defs_.end() && sd_it->second->is_union);
		if (is_union)
		{
			if (e->op == ".")
				return gen_lvalue_addr(e->lhs);
			else
				return gen_expr(e->lhs);
		}
		// 确定实际 LLVM field index（位域成员用存储单元索引）
		llvm::StructType *st = get_struct_type(tag); // 确保布局已建立
		unsigned field_idx = mi.slot; // fallback = source order
		std::string key = tag + "." + e->name;
		auto bf_it = bitfield_access_.find(key);
		if (bf_it != bitfield_access_.end())
			field_idx = bf_it->second.storage_idx;
		else
		{
			auto fi_it = field_index_map_.find(key);
			if (fi_it != field_index_map_.end())
				field_idx = fi_it->second;
		}
		if (e->op == ".")
		{
			llvm::Value *obj_addr = gen_lvalue_addr(e->lhs);
			return builder.CreateStructGEP(st, obj_addr, field_idx, e->name.c_str());
		}
		else // "->" 
		{
			llvm::Value *ptr = gen_expr(e->lhs);
			return builder.CreateStructGEP(st, ptr, field_idx, e->name.c_str());
		}
	}

	// —— 短路求值 && / || ——
	llvm::Value *gen_short_circuit(const ExprPtr &e, bool is_and)
	{
		llvm::Function *F = builder.GetInsertBlock()->getParent();
		llvm::Value *lhs_val = gen_cond(e->lhs);
		// 分配结果变量
		llvm::AllocaInst *result = builder.CreateAlloca(
			llvm::Type::getInt1Ty(ctx), nullptr, "sc.result");
		llvm::BasicBlock *rhs_bb = llvm::BasicBlock::Create(ctx, is_and ? "sc.rhs" : "sc.rhs", F);
		llvm::BasicBlock *short_bb = llvm::BasicBlock::Create(ctx, is_and ? "sc.false" : "sc.true", F);
		llvm::BasicBlock *merge_bb = llvm::BasicBlock::Create(ctx, "sc.merge", F);
		if (is_and)
		{
			// a && b: a false → short(false), a true → rhs(b)
			builder.CreateCondBr(lhs_val, rhs_bb, short_bb);
			builder.SetInsertPoint(short_bb);
			builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), 0), result);
			builder.CreateBr(merge_bb);
			builder.SetInsertPoint(rhs_bb);
			llvm::Value *rhs_val = gen_cond(e->rhs);
			builder.CreateStore(rhs_val, result);
			builder.CreateBr(merge_bb);
		}
		else
		{
			// a || b: a true → short(true), a false → rhs(b)
			builder.CreateCondBr(lhs_val, short_bb, rhs_bb);
			builder.SetInsertPoint(short_bb);
			builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), 1), result);
			builder.CreateBr(merge_bb);
			builder.SetInsertPoint(rhs_bb);
			llvm::Value *rhs_val = gen_cond(e->rhs);
			builder.CreateStore(rhs_val, result);
			builder.CreateBr(merge_bb);
		}
		builder.SetInsertPoint(merge_bb);
		return builder.CreateLoad(llvm::Type::getInt1Ty(ctx), result);
	}

	// —— 三元 cond ? then : else ——（C §6.5.15：仅求值被选中的一支；结果可为 void）
	// 修正缺陷 #25：旧实现无条件用 select，既急切求值两臂（副作用重复）、又不支持 void 臂。
	// 改为与 gen_short_circuit 同构的控制流：CondBr 分入 then/else 块各只求值一臂，
	// 在各自“结束块”内完成类型统一后再 Br 到 merge，最后 phi 合流（void 则不合流）。
	llvm::Value *gen_ternary(const ExprPtr &e)
	{
		llvm::Function *F = builder.GetInsertBlock()->getParent();
		llvm::Value *cond = gen_cond(e->lhs); // 条件求值后的落点块即当前块
		llvm::BasicBlock *then_bb = llvm::BasicBlock::Create(ctx, "tern.then", F);
		llvm::BasicBlock *else_bb = llvm::BasicBlock::Create(ctx, "tern.else", F);
		llvm::BasicBlock *merge_bb = llvm::BasicBlock::Create(ctx, "tern.merge", F);
		builder.CreateCondBr(cond, then_bb, else_bb);

		// 分别求值两支（每支内部可能再劈块；结束块 = 求值后 builder 当前块，尚无终止符）
		builder.SetInsertPoint(then_bb);
		llvm::Value *tv = gen_expr(e->args[0]);
		llvm::BasicBlock *then_end = builder.GetInsertBlock();

		builder.SetInsertPoint(else_bb);
		llvm::Value *ev = gen_expr(e->args[1]);
		llvm::BasicBlock *else_end = builder.GetInsertBlock();

		const bool tvoid = tv->getType()->isVoidTy();
		const bool evoid = ev->getType()->isVoidTy();
		// 结果类型：任一臂为 void → 整体 void；否则同型即用、异型回退 i32（沿用旧策略）
		llvm::Type *rt = nullptr;
		if (!tvoid && !evoid)
			rt = (tv->getType() == ev->getType()) ? tv->getType() : llvm::Type::getInt32Ty(ctx);

		// 回到各自结束块，就地转换（不得跨终止符追加）后 Br
		builder.SetInsertPoint(then_end);
		if (rt && rt != llvm::Type::getInt32Ty(ctx))
			tv = convert_value(tv, rt, get_expr_ctype(e->args[0]));
		else if (rt)
			tv = to_i32(tv);
		builder.CreateBr(merge_bb);

		builder.SetInsertPoint(else_end);
		if (rt && rt != llvm::Type::getInt32Ty(ctx))
			ev = convert_value(ev, rt, get_expr_ctype(e->args[1]));
		else if (rt)
			ev = to_i32(ev);
		builder.CreateBr(merge_bb);

		builder.SetInsertPoint(merge_bb);
		if (!rt) // void 结果：两支已各自执行，无值可合流。
			// 注：void 三元仅合法于语句上下文（结果被丢弃），故直接回还某一 void 臂值作占位；
			//     不可用 Constant::getNullValue(void)（Release 无断言下会走 llvm_unreachable 崩溃）。
			return tv;
		llvm::PHINode *phi = builder.CreatePHI(rt, 2);
		phi->addIncoming(tv, then_end);
		phi->addIncoming(ev, else_end);
		return phi;
	}

	// —— 逗号表达式 (a, b, c) —— 顺序求值，结果为最后一个 ——
	llvm::Value *gen_comma(const ExprPtr &e)
	{
		llvm::Value *last = nullptr;
		for (const auto &sub : e->args)
			last = gen_expr(sub);
		return last ? last : llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
	}

	// —— 显式类型转换 (type)expr ——
	llvm::Value *gen_cast_expr(const ExprPtr &e)
	{
		llvm::Value *val = gen_expr(e->lhs);
		llvm::Type *target = to_llvm_type(e->type);
		// (void)expr：只求值以触发副作用并丢弃结果，不做任何类型转换。
		//   否则指针操作数会落入下方 ptr→int 分支，误发 PtrToInt 到 void 目标
		//   （LLVM IR 验证报 "PtrToInt result must be integral"）。
		if (target->isVoidTy())
			return val;
		// struct/union cast 取指针
		if ((e->type->kind == TypeKind::Struct || e->type->kind == TypeKind::Union) &&
			val->getType()->isPointerTy())
			return val;
		if (val->getType() == target)
			return val;
		// 指针 ↔ 整数
		if (val->getType()->isPointerTy() && !target->isPointerTy())
			return builder.CreatePtrToInt(val, target);
		if (!val->getType()->isPointerTy() && target->isPointerTy())
			return builder.CreateIntToPtr(val, target);
		// FP → FP (double↔float)
		if (val->getType()->isFloatingPointTy() && target->isFloatingPointTy())
		{
			if (val->getType()->isDoubleTy() && target->isFloatTy())
				return builder.CreateFPTrunc(val, target);
			return builder.CreateFPExt(val, target);
		}
		// FP → Int
		if (val->getType()->isFloatingPointTy() && target->isIntegerTy())
			return builder.CreateFPToSI(val, target);
		// Int → FP
		if (val->getType()->isIntegerTy() && target->isFloatingPointTy())
			return builder.CreateSIToFP(val, target);
		// 整数↔整数
		if (val->getType()->isIntegerTy() && target->isIntegerTy())
		{
			llvm::Instruction::CastOps op = get_cast_op(val->getType(), target);
			// 加宽时按源类型的符号性选扩展：无符号源（wchar_t / unsigned _BitInt）须零扩展，
			// 否则 (int)(wint_t)0xFFFF 会被符号扩展成 -1。get_cast_op 仅看位宽（i16 既可能 wchar_t
			// 也可能 signed short/_BitInt(16)），故此处用源 CType 判定无符号性。
			if (op == llvm::Instruction::SExt)
			{
				CTypePtr st = get_expr_ctype(e->lhs);
				if (st && is_unsigned_int_type(st))
					op = llvm::Instruction::ZExt;
			}
			return builder.CreateCast(op, val, target);
		}
		return val;
	}

	// 提升为 i32（char->sext, i1->zext, ptr->ptrtoint）
	// 提升为浮点类型（double）
	llvm::Value *to_fp(llvm::Value *v, llvm::Type *fp_ty)
	{
		llvm::Type *t = v->getType();
		if (t == fp_ty) return v;
		if (t == llvm::Type::getFloatTy(ctx)) return builder.CreateFPExt(v, fp_ty);
		if (t->isDoubleTy()) return builder.CreateFPTrunc(v, fp_ty); // float→double 不会到这里
		if (t == llvm::Type::getInt32Ty(ctx)) return builder.CreateSIToFP(v, fp_ty);
		if (t == llvm::Type::getInt8Ty(ctx)) return builder.CreateSIToFP(v, fp_ty);
		if (t == llvm::Type::getInt1Ty(ctx)) return builder.CreateZExt(v, llvm::Type::getInt32Ty(ctx)), builder.CreateSIToFP(builder.CreateZExt(v, llvm::Type::getInt32Ty(ctx)), fp_ty);
		if (t->isIntegerTy()) return builder.CreateSIToFP(v, fp_ty);
		return v;
	}

	llvm::Value *to_i32(llvm::Value *v)
	{
		llvm::Type *t = v->getType();
		if (t == llvm::Type::getInt32Ty(ctx)) return v;
		if (t->isPointerTy()) return builder.CreatePtrToInt(v, llvm::Type::getInt32Ty(ctx));
		if (t == llvm::Type::getInt1Ty(ctx)) return builder.CreateZExt(v, llvm::Type::getInt32Ty(ctx));
		if (t == llvm::Type::getInt8Ty(ctx)) return builder.CreateSExt(v, llvm::Type::getInt32Ty(ctx));
		if (t->isIntegerTy() && t->getIntegerBitWidth() < 32) return builder.CreateSExt(v, llvm::Type::getInt32Ty(ctx));
		if (t->isIntegerTy() && t->getIntegerBitWidth() > 32) return builder.CreateTrunc(v, llvm::Type::getInt32Ty(ctx));
		return v;
	}

	// 提升为 i64（用于 GEP 索引）
	llvm::Value *to_index(llvm::Value *v)
	{
		if (v->getType() == llvm::Type::getInt64Ty(ctx)) return v;
		return builder.CreateSExt(v, llvm::Type::getInt64Ty(ctx));
	}
};

}  // namespace

// ============================================================
// 对外接口
// ============================================================
bool generate(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors)
{
	if (!state.ast)
	{
		return false; // parse 已报错，跳过
	}

	// 模块名取源文件 stem
	std::string module_name = std::filesystem::path(opts.source_path).stem().string();
	if (module_name.empty())
	{
		module_name = "a";
	}

	CodeGenImpl cg(module_name, errors);
	cg.gen_tu(*state.ast);

	// DEBUG: 先写 .ll 再验证
	if (opts.emit_llvm)
	{
		std::string out_path = opts.output.empty() ? (module_name + ".ll") : opts.output;
		std::error_code ec;
		llvm::raw_fd_ostream os(out_path, ec);
		if (ec)
		{
			errors.push_back(Error{opts.source_path, Pos{1, 1}, "cannot write .ll: " + ec.message()});
			return false;
		}
		cg.module->print(os, nullptr);
		return true; // 跳过验证，方便调试
	}

	// 验证模块
	std::string verify_err;
	llvm::raw_string_ostream vs(verify_err);
	if (llvm::verifyModule(*cg.module, &vs))
	{
		errors.push_back(Error{opts.source_path, Pos{1, 1}, "module verification: " + verify_err});
		return false;
	}

	// -c 编译模式：写 .bc 到输出路径（默认 <stem>.bc）
	if (opts.compile_only)
	{
		std::string bc_out = opts.output.empty() ? (module_name + ".bc") : opts.output;
		std::error_code ec;
		llvm::raw_fd_ostream os(bc_out, ec);
		if (ec)
		{
			errors.push_back(Error{opts.source_path, Pos{1, 1}, "cannot write .bc: " + ec.message()});
			return false;
		}
		llvm::WriteBitcodeToFile(*cg.module, os);
		state.bc_path = bc_out; // 保留（不删除）
		return true;
	}

	// 链接模式：写 .bc 到临时目录
	if (opts.link_output)
	{
		std::error_code ec;
		std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
		if (ec)
		{
			tmp = std::filesystem::current_path(ec);
		}
		state.bc_path = (tmp / (module_name + ".bc")).string();
		llvm::raw_fd_ostream os(state.bc_path, ec);
		if (ec)
		{
			errors.push_back(Error{opts.source_path, Pos{1, 1}, "cannot write .bc: " + ec.message()});
			return false;
		}
		llvm::WriteBitcodeToFile(*cg.module, os);
		return true;
	}

	return true;
}

}  // namespace colang
