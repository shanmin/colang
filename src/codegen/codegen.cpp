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

// —— COFF .obj 发射（新 PM，供进程内 lld 链接）——
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/RuntimeLibcallInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/CodeGen/MachineFunctionAnalysis.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/MachinePassManager.h>
#include <llvm/CodeGen/TargetPassConfig.h> // registerCodeGenCallback
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/CGPassBuilderOption.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>

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
	// P211 Co 层入口模型：标志当前正在发射合成 main 体内顶层裸语句。
	//   Return 分支据此将无值的 `return;` 从 CreateRetVoid()（对 i32 返回不合法）
	//   改为 `ret i32 0`；有值则自然走现有 convert_value 到 i32。gen_function 中始终为 false。
	bool in_synthesized_main_body_ = false;
	// P229 Windows GUI 子系统 / WinMain 入口：由 gen_tu 扫描 AST 得出，经 generate() 回传 CompileState。
	//   use_winmain_=true → 入口取 WinMain、跳过合成 main、隐含 windows 子系统；
	//   windows_subsystem_=true → 任一函数标 [[subsystem("windows")]]（或 use_winmain_）。
	bool windows_subsystem_ = false;
	bool use_winmain_ = false;
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
						ba.is_signed = m.type && m.type->kind != TypeKind::Bool && m.type->kind != TypeKind::Enum && !is_unsigned_int_type(m.type); // Bool 恒0/1、Enum 沿用基线(无符号)均不参与符号扩展；覆盖 Char/Short/Int/Long/LongLong/_BitInt
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
		// 预注册 struct/union 定义（供 get_struct_type 查询）。空定义（`struct s {};`
		//   的 GNU 扩展）也要注册：否则被当作未定义 → opaque %struct. → 全局/局部
		//   对象 "unsized type"（CTS 0186 族）。
		for (const auto &sd : tu.structs)
		{
			if (!sd.tag.empty() && sd.is_definition)
				struct_defs_[sd.tag] = &sd;
		}
		// 同名函数的「规范声明」归并：无原型声明（`void out();`）与其带形参定义
		//   （`void out(int*, ...)`）在 tu.funcs 中是两条记录，LLVM 的 Function 类型
		//   由首次 getOrInsertFunction 固定、后续声明无法改写 → 定义体按错误签名绑定
		//   形参（形参无 alloca，引用退化为 `load i32, i32 0`），调用点亦与函数签名
		//   不符（verifier 报 Load/Store/Call 类错误）。故按名归并：优先带 body 的
		//   定义，其次首个声明，作为预声明/类型登记的唯一依据。
		std::unordered_map<std::string, const FuncDecl *> canon;
		for (const auto &fn : tu.funcs)
		{
			auto it = canon.find(fn.name);
			if (it == canon.end() || (!it->second->body && fn.body))
				canon[fn.name] = &fn;
		}
		// 预声明所有函数符号：使全局初始化器里的 &函数名 能折叠为函数地址
		//   （module->getFunction 可见）。getOrInsertFunction 幂等，下方第一遍仍据
		//   同一 Function 设 linkage/uwtable。修复 00089：s={&zero} 早于函数声明发射。
		for (const auto &fn : tu.funcs)
		{
			if (canon[fn.name] != &fn) continue;
			if (module->getFunction(fn.name)) continue;
			llvm::FunctionType *ft = to_func_type(fn);
			module->getOrInsertFunction(fn.name, ft);
		}
		// P219：顶层非恒定初始化的运行期 store 承载（仅 .co 新面 has_bare 生效，纯 C 档逐字不变）
		const bool co_has_bare = !tu.top_level_body.empty();
		struct TlPendingInit { llvm::GlobalVariable *gvar; ExprPtr init; CTypePtr ctype; };
		std::vector<TlPendingInit> tl_pending_inits;
		// P241：预登记全部全局变量的类型（名 → CType），使「文件作用域初始化器」折叠过程中
		//   经 CodegenTypeEnv::var_type 解析「声明在当前全局之后」的全局时能命中类型，
		//   sizeof/alignof 不再回退 int32(4)（P234 旁支② / P227 同族「按名查全局」）。
		//   仅登记类型、不建 LLVM 值：取址折叠（fold_addr_of_global）对未发射全局仍按
		//   「前向引用不误折」返回 null（P227 既定口径），故不影响地址常量语义。
		for (const auto &gv : tu.globals)
		{
			if (!gv.type || gv.type->kind == TypeKind::Void) continue;
			if (!global_types_.count(gv.name)) global_types_[gv.name] = gv.type;
		}
		// 第零遍：发射全局变量
		for (const auto &gv : tu.globals)
		{
			// void 型对象属不完整类型（parser 已报 error）：兜底跳过，避免以 void 建 GlobalVariable。
			if (!gv.type || gv.type->kind == TypeKind::Void)
				continue;
			// extern 纯声明（无定义）：创建 declaration（无 initializer）
			if (gv.is_extern && !gv.init)
			{
				llvm::Type *ty = to_llvm_alloca_type(gv.type);
				if (globals_.count(gv.name)) continue;
				auto *gvar = new llvm::GlobalVariable(*module, ty, false,
					llvm::GlobalValue::ExternalLinkage, nullptr, gv.name);
				// P211 B.3：public 仅叠加 DLL 导出 storage class，不改 linkage。
				if (gv.is_public)
					gvar->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
				globals_[gv.name] = gvar;
				global_types_[gv.name] = gv.type;
				continue;
			}
			llvm::Type *ty = to_llvm_alloca_type(gv.type);
			// P212 Strip：被导入模块的非 public 全局 → InternalLinkage（不注入外部可见 LLVM 值，
			//   同模块内引用仍可解析；链接器可自然 DCE）。主源自身、或 public、保持 P211 旧行为。
			const bool p212_strip_global = !gv.source_module_key.empty() && !gv.is_public;
			auto linkage = (gv.is_static || p212_strip_global)
				? llvm::GlobalValue::InternalLinkage
				: llvm::GlobalValue::ExternalLinkage;
			bool is_const = false; // P3: const 全局可考虑 constant
			// 先以零初值建全局并登记，再折叠初始化器：使「自引用 &本全局」
			//   （如 struct N a = { &a, ... } 循环哨兵、.st_3=&tg3_1）能在 fold_addr_of_global
			//   命中 globals_[gv.name]。旧路径 fold 早于登记 → 自引用 &a 落 forward-ref 分支折成
			//   null，运行期解引用越界（CTS 0010/0011/0048 崩族）。折叠永不返回 nullptr，故
			//   先建零初值再 setInitializer 与原「以 init_val 建」终态逐字一致（零回归）。
			auto *gvar = new llvm::GlobalVariable(*module, ty, is_const,
				linkage, llvm::Constant::getNullValue(ty), gv.name);
			if (auto a = obj_align(gv.type, ty, gv.align)) gvar->setAlignment(*a);
			// P211 B.3：public 仅叠加 DLL 导出 storage class，不改 linkage。
			// COFF 约束：内部链接（static/P212 strip）符号不得带 DLL 存储类
			//   （"symbol with local linkage cannot have a DLL storage class"）；故仅当
			//   最终 linkage 为 External 时才打 Export。static public 在 exe 场景本就无导出语义。
			if (gv.is_public && linkage == llvm::GlobalValue::ExternalLinkage)
				gvar->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
			globals_[gv.name] = gvar;
			global_types_[gv.name] = gv.type;
			bool tl_need_runtime = false; // P219：顶层非恒定初始化标记
			if (gv.init)
			{
				bool folded = true;
				llvm::Constant *c = fold_global_init(gv.init, ty, &folded);
				// P219 A 方案：仅 .co 新面（有顶层裸指令）时，标量/指针初值不可折叠
				//   不再静默折成 null，改占位 0 + 记待运行期 store（合成 main 头部按序写回）。
				//   纯 C 档（无顶层裸句）沿用旧折叠行为，CTS/.c 零影响。
				if (!folded && co_has_bare && ty->isSingleValueType())
					tl_need_runtime = true; // 保持零初值，运行期按序 store
				else
					gvar->setInitializer(c);
			}
			if (tl_need_runtime)
				tl_pending_inits.push_back({gvar, gv.init, gv.type});
		}
		// 第一遍：声明所有函数（保证调用可见）
		for (const auto &fn : tu.funcs)
		{
			if (canon[fn.name] != &fn) continue;
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
				// P212 Strip：被导入模块的非 public 函数定义 → InternalLinkage（不对外可见）。
				//   同模块内引用（如 public wrapper 调 private helper）仍能解析，LLVM 自然 DCE。
				// P213 winapi.co 探路修正：纯原型（body == nullptr）**不参与 strip**。
				//   它们本就没有本地定义可限内，强制保持 ExternalLinkage 交给链接器解析（典型场景：
				//   winapi.co 中的 Win32 API 声明，clang 默认链接的 kernel32.lib 会自然处理 __imp_ thunk）。
				const bool p212_strip_func = !fn.source_module_key.empty() && !fn.is_public && fn.body;
				const bool f_internal = fn.is_static || p212_strip_func;
				F->setLinkage(f_internal
					? llvm::GlobalValue::InternalLinkage
					: llvm::GlobalValue::ExternalLinkage);
				// P211 B.3：public 仅叠加 DLL 导出 storage class（COFF .drectve /EXPORT:），不改 linkage。
				// P213 winapi.co 探路修正：**仅对有函数体的 public 打 DLLExport**。
				//   纯原型 (body == nullptr) 本就不局部定义、不存在可导出的对象；强制打 Export 会误导
				//   lld 去当前 binary 中寻不存在的定义 → 静默默返 0 或 LNK4217。交给链接器默认处理。
				// COFF 约束：内部链接符号（static）不得带 DLL 存储类；static public 仅内部可调用、不导出。
				if (fn.is_public && fn.body && !f_internal)
					F->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
				// Win64 ABI 要求每个函数带 .pdata/.xdata 展开表；clang 从源码编译时默认
				// 加 "uwtable"，但 colang 手工建 IR 不会自动带。缺它则基于 RtlUnwind 的栈
				// 回溯（UCRT longjmp、异常处理、backtrace）找不到帧 → 崩溃。统一补上。
				F->addFnAttr("uwtable");
			}
		}
		// ============================================================
		// P214 [[lib("X")]] → COFF 目录段 .drectve 的 /DEFAULTLIB 指令
		//   实测通道（_tmp/libprobe/dt.ll）：
		//     private global [N x i8] c"/DEFAULTLIB:\22X.lib\22 ", section ".drectve"
		//   经 .bc → clang.exe → lld-link 穿透成立，无需命令行再给 -lX。
		//   按首现序去重（同库多次声明只发一条）；函数优先于全局变量（跟 AST 顺序）。
		//   注意：/DEFAULTLIB 只解决「拉哪个库」，不解决多库同名符号的归属 —— 后者已由
		//   Sema 的冲突检查（同名不同 lib → error）在编译期拦下。
		// ============================================================
		{
			std::vector<std::string> libs;
			auto add_lib = [&libs](const std::string &s)
			{
				if (s.empty()) return;
				for (const auto &x : libs)
					if (x == s) return;
				libs.push_back(s);
			};
			for (const auto &fn : tu.funcs) add_lib(fn.lib_name);
			for (const auto &gv : tu.globals) add_lib(gv.lib_name);
			for (std::size_t i = 0; i < libs.size(); ++i)
			{
				// 末尾留一个空格作分隔（与 MSVC CRT 自带的 .drectve 条目一致），再加 '\0' 收尾
				const std::string directive = "/DEFAULTLIB:\"" + libs[i] + ".lib\" ";
				llvm::Constant *data = llvm::ConstantDataArray::getString(ctx, directive, true);
				auto *dg = new llvm::GlobalVariable(*module, data->getType(), true,
					llvm::GlobalValue::PrivateLinkage, data, "co.drectve" + std::to_string(i));
				dg->setSection(".drectve");
				dg->setAlignment(llvm::MaybeAlign(2));
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
		// ============================================================
		// P229：入口子系统标志——扫定义函数定 WinMain 入口 / [[subsystem("windows")]]。
		//   WinMain 隐含 windows；标在任意函数的 subsystem("windows") 亦置 windows（可为 main 入口无控制台）。
		//   P231：入口属性仅主模块生效——导入模块（source_module_key 非空）里的
		//   WinMain/[[subsystem]] 不参与置位（sema 侧另有忽略警告）。
		// ============================================================
		for (const auto &f : tu.funcs)
		{
			if (!f.source_module_key.empty()) continue;
			if (f.body && f.name == "WinMain") use_winmain_ = true;
			if (f.subsystem_windows) windows_subsystem_ = true;
		}
		if (use_winmain_) windows_subsystem_ = true;
		// ============================================================
		// P211 A.4 入口合成（仅当非"纯 C 档"）
		//   has_bare = 顶层裸指令存在（.co 新面）
		//   user_main = 带 body 的 main 定义
		//   纯 C 档 = !has_bare && user_main → 逐字同 P210，不合成、不改名
		//   否则 →
		//     has_bare && user_main → 将 main 改名 __co_main__，合成 i32 @main(i32,ptr,ptr)
		//       内按顺序发裸句，末尾未终止则转调 __co_main__ 按其参数个数传参、以其返回为退出码
		//     has_bare && !user_main → 合成 main，末尾未终止则 ret 0
		//     !has_bare && !user_main → 合成空 main（ret 0）（sema 已射无入口警告）
		// ============================================================
		{
			const bool has_bare = !tu.top_level_body.empty();
			const FuncDecl *user_main = nullptr;
			for (const auto &f : tu.funcs)
				if (f.body && f.name == "main" && f.source_module_key.empty()) { user_main = &f; break; } // P231：仅主模块
			const bool pure_c = !has_bare && user_main != nullptr;
			if (!pure_c && !use_winmain_) // P229：WinMain 入口下不合成 main（GUI 逻辑在 WinMain）
			{
				llvm::Function *old_main = nullptr;
				if (user_main)
				{
					old_main = module->getFunction("main");
					if (old_main) old_main->setName("__co_main__");
				}
				llvm::FunctionType *mt = llvm::FunctionType::get(
					llvm::Type::getInt32Ty(ctx),
					{llvm::Type::getInt32Ty(ctx),
					 llvm::PointerType::getUnqual(ctx),
					 llvm::PointerType::getUnqual(ctx)},
					/*isVarArg=*/false);
				llvm::Function *m = llvm::Function::Create(mt,
					llvm::GlobalValue::ExternalLinkage, "main", module.get());
				m->addFnAttr("uwtable");
				auto *entry = llvm::BasicBlock::Create(ctx, "entry", m);
				builder.SetInsertPoint(entry);
				// 合成上下文需从“无局部变量/无循环”起步，同 gen_function 入口°样塑空状态；
				//   剩存外层 var 表拓后归回（无其它 gen_function 正在使用，但保守处理）。
				{
					auto saved_vars = vars_;
					auto saved_types = var_types_;
					vars_.clear();
					var_types_.clear();
					loop_stack_.clear();
					label_blocks_.clear();
					case_label_bb_.clear();
					pending_gotos_.clear();
					cur_func_name_ = "main";
					// 将 argc/argv/envp 入作域（argv 本批不向用户暴露，仅为下一批预留映射入口）。
					{
						auto ait = m->arg_begin();
						llvm::AllocaInst *ac = builder.CreateAlloca(
							llvm::Type::getInt32Ty(ctx), nullptr, "argc");
						builder.CreateStore(&*ait++, ac);
						vars_["argc"] = ac; var_types_["argc"] = nullptr;
						if (ait != m->arg_end())
						{
							llvm::AllocaInst *av = builder.CreateAlloca(
								llvm::PointerType::getUnqual(ctx), nullptr, "argv");
							builder.CreateStore(&*ait++, av);
							vars_["argv"] = av;
						}
						if (ait != m->arg_end())
						{
							llvm::AllocaInst *ev = builder.CreateAlloca(
								llvm::PointerType::getUnqual(ctx), nullptr, "envp");
							builder.CreateStore(&*ait, ev);
							vars_["envp"] = ev;
						}
					}
					in_synthesized_main_body_ = true;
					// P219：顶层非恒定初始化 → 合成 main 头部按声明序 store 运行期值
					//   （先于顶层裸句执行，贴近「顶层顺序执行」模型；此时 argc/argv/envp 已入作用域）
					for (auto &pi : tl_pending_inits)
					{
						llvm::Type *vt = pi.gvar->getValueType();
						llvm::Value *v = gen_expr(pi.init);
						if (v && v->getType() != vt)
							v = convert_value(v, vt, get_expr_ctype(pi.init));
						if (v) builder.CreateStore(v, pi.gvar);
					}
					for (const auto &s : tu.top_level_body)
						if (s) gen_stmt(s, m);
					in_synthesized_main_body_ = false;
					// 末尾未终止 → 转调 __co_main__ 或 ret 0（“return V;”已提前 ret 时跳过）
					llvm::BasicBlock *tail = builder.GetInsertBlock();
					if (!tail || tail->empty() || !tail->back().isTerminator())
					{
						if (old_main)
						{
							std::vector<llvm::Value*> cargs;
							std::size_t n = old_main->getFunctionType()->getNumParams();
							if (n >= 1) cargs.push_back(m->getArg(0));
							if (n >= 2) cargs.push_back(m->getArg(1));
							if (n >= 3) cargs.push_back(m->getArg(2));
							llvm::Value *r = builder.CreateCall(old_main, cargs);
							builder.CreateRet(r);
						}
						else
						{
							builder.CreateRet(llvm::ConstantInt::get(
								llvm::Type::getInt32Ty(ctx), 0));
						}
					}
					// 空块补 unreachable（与 gen_function 同构）：防 verifier 报无终止
					for (llvm::BasicBlock &bb : *m)
					{
						if (bb.empty())
						{
							builder.SetInsertPoint(&bb);
							builder.CreateUnreachable();
						}
					}
					vars_ = std::move(saved_vars);
					var_types_ = std::move(saved_types);
					cur_func_name_.clear();
				}
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
		// fty 应是指针到函数: Pointer{Function{base=ret, params=[...]}}；
		//   函数指示符（Ternary/条件表达式选中函数名、*fp、块作用域函数声明）本身即 Function。
		CTypePtr fn;
		if (fty && fty->kind == TypeKind::Function)
			fn = fty;
		else if (fty && fty->kind == TypeKind::Pointer && fty->base && fty->base->kind == TypeKind::Function)
			fn = fty->base;
		if (fn)
		{
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

	// 沿 Member/Subscript 链回溯到根「全局/静态」符号，累加编译期字节偏移。
	// 与 fold_null_offset 同构，唯根为具名对象（globals_/vars_）而非 (T*)0。
	// 链上出现非常量下标/未知类型/位域 → 回 nullptr，交回调用方按旧路径处理（零回归）。
	// 修 CTS 0010/0011/0048 崩族：全局聚合初始化器内 `&arr[ci]`、`&g.field` 折成 null。
	llvm::GlobalVariable *resolve_lvalue_root_offset(const ExprPtr &e, intmax_t &out_off)
	{
		if (!e) return nullptr;
		ExprPtr p = e;
		while (p && p->kind == ExprKind::Cast) p = p->lhs;   // 重解读 cast 不改地址偏移
		if (!p) return nullptr;
		const llvm::DataLayout &DL = module->getDataLayout();
		if (p->kind == ExprKind::VarRef)
		{
			out_off = 0;
			auto git = globals_.find(p->name);
			if (git != globals_.end()) return git->second;
			auto vit = vars_.find(p->name);   // 函数内 static：已物化为 GlobalVariable
			if (vit != vars_.end())
				if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(vit->second)) return gv;
			return nullptr;
		}
		if (p->kind == ExprKind::Member)
		{
			intmax_t base = 0;
			llvm::GlobalVariable *root = resolve_lvalue_root_offset(p->lhs, base);
			if (!root) return nullptr;
			MemberInfo mi = resolve_member_info(p);
			// 逐级静态偏移（含匿名聚合提升；union 级恒 0）；位域/未知 tag 拒折
			intmax_t moff = 0;
			if (!member_static_offset(mi, p->name, moff)) return nullptr;
			out_off = base + moff;
			return root;
		}
		if (p->kind == ExprKind::Binary && p->op == "[]")
		{
			intmax_t base = 0;
			llvm::GlobalVariable *root = resolve_lvalue_root_offset(p->lhs, base);
			if (!root) return nullptr;
			CTypePtr at = get_expr_ctype(p->lhs);
			if (!at || !at->base) return nullptr;
			llvm::Type *ety = to_llvm_alloca_type(at->base);
			if (!ety) return nullptr;
			intmax_t idx;
			if (!fold_int_const(p->rhs, idx)) return nullptr;
			out_off = base + idx * static_cast<intmax_t>(DL.getTypeAllocSize(ety).getFixedValue());
			return root;
		}
		return nullptr;
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
		// 子对象地址（&arr[ci] / &g.field，可嵌套）：偏移编译期常量 → i8 字节 GEP 常量。
		// 仅 has_amp（取址形态）走此路；裸数组名退化仍由下方 VarRef 分支处理。
		if (has_amp && p && (p->kind == ExprKind::Member ||
			(p->kind == ExprKind::Binary && p->op == "[]")))
		{
			intmax_t off = 0;
			if (llvm::GlobalVariable *root = resolve_lvalue_root_offset(p, off))
			{
				llvm::Constant *idx = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx),
					static_cast<uint64_t>(off), false);
				return llvm::ConstantExpr::getGetElementPtr(llvm::Type::getInt8Ty(ctx), root, {idx});
			}
			return nullptr;   // 链上出现非常量/未知 → 交回调用方（旧行为）
		}
		if (!p || p->kind != ExprKind::VarRef) return nullptr;
		const std::string nm = p->name;
		auto git = globals_.find(nm);
		if (git == globals_.end())
		{
			// 函数内 static 局部：已物化为 GlobalVariable 并登记在 vars_（名带 __static_ 前缀），
			// &x / 数组名退化同样应折叠为地址常量（旧代码只查 globals_ → 初始化器落 null，解引用崩）。
			auto vit = vars_.find(nm);
			if (vit != vars_.end() && llvm::isa<llvm::GlobalVariable>(vit->second))
			{
				if (!has_amp)
				{
					auto tit = var_types_.find(nm);
					if (tit == var_types_.end() || !tit->second ||
						tit->second->kind != TypeKind::Array)
						return nullptr;
				}
				return llvm::cast<llvm::Constant>(vit->second);
			}
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
	// offsetof 空基址形态折叠：把 `(char*)&((T*)0)->m`、`(int)(size_t)&((T*)0)->m`、`((T*)0)->m.field[idx]`
	// 等「以空指针为基的成员/下标取址」表达式，逐层累加字节偏移（llvm::StructLayout::getElementOffset +
	// DataLayout::getTypeAllocSize），对齐 padding/union/数组步长自动正确。镜像 gen_member_addr 的
	// tag/field_idx 解析。非该形态（基座为真实变量/调用/下标非常量）一律回 false，交回调用方按原处理（零回归）。
	// CTS 0053 offsetof 全局 static 折叠族（旧因 fold_int_const 不认成员/取址而落 0）。
	bool fold_null_offset(const ExprPtr &e, intmax_t &out)
	{
		if (!e) return false;
		ExprPtr p = e;
		while (p && p->kind == ExprKind::Cast) p = p->lhs;   // 指针/整型重解读 cast 不改偏移
		if (!p) return false;
		const llvm::DataLayout &DL = module->getDataLayout();
		if (p->kind == ExprKind::Unary && p->op == "&")
			return fold_null_offset(p->lhs, out);              // &lvalue == 该对象地址
		if (p->kind == ExprKind::Member)
		{
			intmax_t base;
			if (!fold_null_offset(p->lhs, base)) return false;
			MemberInfo mi = resolve_member_info(p);
			// 逐级静态偏移（含匿名聚合提升；union 级恒 0）；位域成员 offsetof 本身 UB → 不折
			intmax_t moff = 0;
			if (!member_static_offset(mi, p->name, moff)) return false;
			out = base + moff; return true;
		}
		if (p->kind == ExprKind::Binary && p->op == "[]")
		{
			intmax_t base;
			if (!fold_null_offset(p->lhs, base)) return false;
			CTypePtr at = get_expr_ctype(p->lhs);
			if (!at || !at->base) return false;
			llvm::Type *ety = to_llvm_alloca_type(at->base);
			if (!ety) return false;
			intmax_t idx;
			if (!fold_int_const(p->rhs, idx)) return false;
			out = base + idx * static_cast<intmax_t>(DL.getTypeAllocSize(ety).getFixedValue());
			return true;
		}
		if (p->kind == ExprKind::IntLiteral && p->int_val == 0) { out = 0; return true; }   // (T*)0 基座
		return false;
	}
	bool fold_offsetof_int(const ExprPtr &e, intmax_t &out)
	{
		return fold_null_offset(e, out);
	}

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
			// 空基址 offsetof 取址：& 的操作数常为成员/下标（非整型常量），
			// 须先于 fold_int_const(e->lhs) 尝试 offsetof 折叠，否则操作数折不动即整式回退 0。
			if (e->op == "&" && fold_offsetof_int(e, out)) return true;
			// sizeof / _Alignof 全局初始化器静态折叠（承 P227 同类「fold_global_init 折叠不完整」）：
			// 旧 fold_int_const 不认 sizeof/alignof，致文件作用域 `int sz = sizeof(T);` 折 0（
			// docs struct.md 探针暂露，10-24 K 矩阵）；函数体内正常（gen_expr Unary 分支已行）。
			// 与 L3843/L3873 处运行期 sizeof/alignof 保持同语义：类型式 e->type、表达式式 e->lhs 探型；
			// VLA 无法静态折、回 false 交上层走运行期路径（与 fold_global_init 非折叠路径同处理）。
			if (e->op == "sizeof" || e->op == "alignof")
			{
				const auto &DL = module->getDataLayout();
				if (e->op == "sizeof")
				{
					if (e->type)
					{
						out = static_cast<intmax_t>(DL.getTypeStoreSize(to_llvm_alloca_type(e->type)));
						return true;
					}
					if (e->lhs)
					{
						CTypePtr et = sizeof_operand_ctype(e->lhs);
						if (et && et->kind == TypeKind::Array && type_has_vla_dim(et)) return false;
						llvm::Type *ty = et ? to_llvm_alloca_type(et) : llvm::Type::getInt32Ty(ctx);
						out = static_cast<intmax_t>(DL.getTypeStoreSize(ty));
						return true;
					}
					out = 4; return true;
				}
				// alignof / _Alignof
				CTypePtr at = e->type ? e->type : (e->lhs ? get_expr_ctype(e->lhs) : nullptr);
				out = at ? static_cast<intmax_t>(type_effective_align(at))
				           : static_cast<intmax_t>(DL.getABITypeAlign(llvm::Type::getInt32Ty(ctx)).value());
				return true;
			}
			intmax_t v; if (!fold_int_const(e->lhs, v)) return false;
			if (e->op == "-") { out = -v; return true; }
			if (e->op == "+") { out = v; return true; }
			if (e->op == "~") { out = ~v; return true; }
			if (e->op == "!") { out = v ? 0 : 1; return true; }
			return false;
		}
		case ExprKind::Binary:
		{
			// 数组下标基址形态（如 (T*)0->arr[i]）：整型折叠不认 []，须委托 offsetof 空基址累加。
			if (e->op == "[]") return fold_offsetof_int(e, out);
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
			return fold_offsetof_int(e, out);   // 成员/下标等 offsetof 空基址形态（非该形态回 nullptr→仍 false）
		}
	}

	// 常量 → union 载荷字节（小端，按 DataLayout 布局）。
	// LLVM 禁止聚合→字节数组的 bitcast，故逐成员摊开：整型截位、浮点取位模式。
	// 含重定位（非空指针）时返回 false：对象写入器不支持 lshr/trunc 形式的
	// 重定位常量表达式（"unsupported constant expression of type lshr"），
	// 只能回退旧的全零行为。未覆盖的填充字节保持 0。
	bool const_to_bytes(llvm::Constant *c, uint64_t off, std::vector<llvm::Constant *> &bytes)
	{
		if (!c || off >= bytes.size()) return true;
		const auto &DL = module->getDataLayout();
		llvm::Type *i8 = llvm::Type::getInt8Ty(ctx);
		llvm::Type *t = c->getType();
		if (t->isIntegerTy())
		{
			auto *it = llvm::cast<llvm::IntegerType>(t);
			unsigned bw = it->getBitWidth();
			if (bw < 8)
			{
				bytes[off] = llvm::ConstantExpr::getCast(llvm::Instruction::ZExt, c, i8);
				return true;
			}
			if (bw == 8)
			{
				bytes[off] = c;
				return true;
			}
			for (unsigned k = 0; k < bw / 8 && off + k < bytes.size(); ++k)
			{
				// LLVM 23 已移除 ConstantExpr::getLShr，改用通用二元 get
				llvm::Constant *sh = llvm::ConstantExpr::get(llvm::Instruction::LShr, c,
					llvm::ConstantInt::get(it, k * 8));
				bytes[off + k] = llvm::ConstantExpr::getTrunc(sh, i8);
			}
			return true;
		}
		if (t->isFloatingPointTy())
		{
			auto *fp = llvm::cast<llvm::ConstantFP>(c);
			llvm::APInt v = fp->getValueAPF().bitcastToAPInt();
			for (unsigned k = 0; k < v.getBitWidth() / 8 && off + k < bytes.size(); ++k)
				bytes[off + k] = llvm::ConstantInt::get(i8, v.extractBitsAsZExtValue(8, k * 8));
			return true;
		}
		if (t->isPointerTy())
			return llvm::isa<llvm::ConstantPointerNull>(c); // 非空指针＝重定位，交回全零
		if (llvm::isa<llvm::ConstantAggregateZero>(c)) return true; // 全 0
		if (auto *cs = llvm::dyn_cast<llvm::ConstantStruct>(c))
		{
			const auto *sl = DL.getStructLayout(cs->getType());
			for (unsigned i = 0; i < cs->getNumOperands(); ++i)
				if (!const_to_bytes(cs->getOperand(i), off + sl->getElementOffset(i), bytes))
					return false;
			return true;
		}
		if (auto *ca = llvm::dyn_cast<llvm::ConstantArray>(c))
		{
			uint64_t esz = DL.getTypeAllocSize(ca->getType()->getElementType());
			for (unsigned i = 0; i < ca->getNumOperands(); ++i)
				if (!const_to_bytes(ca->getOperand(i), off + i * esz, bytes))
					return false;
			return true;
		}
		if (auto *cd = llvm::dyn_cast<llvm::ConstantDataSequential>(c))
		{
			uint64_t esz = DL.getTypeAllocSize(cd->getElementType());
			for (unsigned i = 0; i < cd->getNumElements(); ++i)
				if (!const_to_bytes(cd->getElementAsConstant(i), off + i * esz, bytes))
					return false;
			return true;
		}
		return true;
	}

	// union 首成员常量 → union 的 [N x i8] 载荷常量（成员小于 union 时余位补 0）。
	llvm::Constant *union_body_const(llvm::StructType *sty, llvm::Constant *mc)
	{
		llvm::Type *body = sty->getNumElements() == 1 ? sty->getElementType(0) : nullptr;
		auto *arr = body && body->isArrayTy() ? llvm::cast<llvm::ArrayType>(body) : nullptr;
		if (!arr || !mc)
			return llvm::ConstantStruct::get(sty, llvm::Constant::getNullValue(body ? body : arr));
		uint64_t N = arr->getNumElements();
		llvm::Type *i8 = llvm::Type::getInt8Ty(ctx);
		if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(mc))
		{
			llvm::APInt v = CI->getValue();
			if (v.getBitWidth() < N * 8) v = v.zext(N * 8);
			else if (v.getBitWidth() > N * 8) v = v.trunc(N * 8);
			std::vector<uint8_t> bytes(N, 0);
			for (uint64_t b = 0; b < N; ++b)
				bytes[b] = static_cast<uint8_t>(v.extractBitsAsZExtValue(8, static_cast<unsigned>(b * 8)));
			return llvm::ConstantStruct::get(sty, llvm::ConstantDataArray::get(ctx, bytes));
		}
		// 指针/浮点/聚合首成员：逐成员摊字节（旧实现只认 ConstantInt，其余一律全零）。
		// 含重定位（指针）时对象写入器无法表达 → 保持旧的全零。
		std::vector<llvm::Constant *> bytes(N, llvm::ConstantInt::get(i8, 0));
		if (!const_to_bytes(mc, 0, bytes))
			return llvm::ConstantStruct::get(sty, llvm::Constant::getNullValue(arr));
		return llvm::ConstantStruct::get(sty, llvm::ConstantArray::get(llvm::ArrayType::get(i8, N), bytes));
	}

	// union 常量槽：目标为具名 struct 且其 tag 是 union（colang union 表征 = struct{[N x i8]}）。
	// 标量或花括号初值均作用于首成员（§6.7.9p19）；取首成员常量按小端写入 [N x i8]。
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
		// 指定初始化器（P238）：union 宿主的成员指定符必须落到**被指定**的成员（§6.7.9p12：
		// 「用设计符指定成员时初始化该成员」）。旧实现无条件取 args[0] 折进 members[0] ——
		// `union { int i; long long q; } g = { .q = 0x1122334455667788 }` 被截成 32 位写进 i，
		// 全局静默误编译（探针 `_tmp/p238_b.co`）。链式（`.p.y` / `.a[1]`）复用 P237 物理路径。
		if (e->kind == ExprKind::InitList && !e->args.empty() && e->args[0]
			&& !e->args[0]->desig_chain.empty())
		{
			const ExprPtr &d0 = e->args[0];
			std::vector<PhysStep> path;
			std::size_t dcur = 0;
			if (flatten_desig(sty, d0->desig_chain, dcur, path, 0)
				&& dcur == d0->desig_chain.size() && !path.empty() && !path[0].is_index)
			{
				llvm::Type *lt = path_leaf_type(sty, path, 0);
				llvm::Constant *c0 = lt ? fold_global_init(d0, lt) : nullptr;
				llvm::Constant *cv = c0 ? wrap_path_const(sty, path, 0, c0) : nullptr;
				if (cv) return cv;
			}
			// 解析失败（非本 union 成员 / 越界）：回落下方首成员路径，不误写偏移
		}
		CTypePtr fm = sd->second->members[0].type; // 首成员类型
		llvm::Type *fmt = to_llvm_alloca_type(fm);
		// 初始化式：首项是带括号子初始化器（或首成员标量）→ 整体折叠；
		// 否则扁平项按 brace elision 全部归入首成员（§6.7.9p20），
		// 如 union{int *p[2];} u={&a,&a} 两项都属 p，旧实现只取首项 → 第 2 项丢失。
		llvm::Constant *mc = nullptr;
		if (fmt)
		{
			if (e->kind == ExprKind::InitList && !e->args.empty() && e->args[0]
				&& e->args[0]->kind != ExprKind::InitList && fmt->isAggregateType())
			{
				std::size_t cur = 0;
				mc = elide_fold(fmt, e->args, cur);
			}
			else
			{
				ExprPtr item = (e->kind == ExprKind::InitList && !e->args.empty()) ? e->args[0] : e;
				mc = fold_global_init(item, fmt);
			}
		}
		return union_body_const(sty, mc);
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

	// 由 LLVM 类型反查 union 的 CType（union 表征 = 具名 struct 包 [N x i8]）。
	// 嵌套初始化器递归时用于传递上下文：缺了它，union 载荷字节数组会被当成真实
	// [N x i8] 数组逐字节初始化（指针/聚合成员全写 0，CTS 0054_0008 运行期访问冲突）。
	CTypePtr union_ctype_of(llvm::Type *ty)
	{
		if (!ty || !ty->isStructTy()) return nullptr;
		auto *sty = llvm::cast<llvm::StructType>(ty);
		if (!is_union_struct(sty)) return nullptr;
		auto t = make_type(TypeKind::Union);
		t->tag = tag_for_struct(sty);
		return t;
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
		// union 包装：首成员为聚合时扁平项按 brace elision 依次折叠进该成员（§6.7.9p20），
		// 否则取一项（标量/括号）作用于首成员（顶层 fold_global_init 会检测 union→fold_union_slot）
		if (ty->isStructTy())
		{
			auto *sty = llvm::cast<llvm::StructType>(ty);
			if (is_union_struct(sty))
			{
				if (pos >= args.size()) return llvm::Constant::getNullValue(ty);
				std::string utag = std::string(llvm::StringRef(sty->getName()).drop_front(7));
				auto usd = struct_defs_.find(utag);
				llvm::Type *fmt = (usd != struct_defs_.end() && !usd->second->members.empty())
					? to_llvm_alloca_type(usd->second->members[0].type) : nullptr;
				if (fmt && fmt->isAggregateType() && args[pos] && args[pos]->kind != ExprKind::InitList)
					return union_body_const(sty, elide_fold(fmt, args, pos));
				ExprPtr it = args[pos++];
				return fold_global_init(it, sty);
			}
		}
		// brace elision：逐元素/槽共享游标
		if (ty->isArrayTy())
		{
			auto *aty = llvm::cast<llvm::ArrayType>(ty);
			llvm::Type *ety = aty->getElementType();
			// 字符串字面量整体初始化字符数组（§6.7.9p14）：在扁平游标中只占一项，
			// 不能按元素逐个消费（旧代码使 struct{char s[20];} x={"str",...} 的后续项全错位）。
			if (pos < args.size() && args[pos] && args[pos]->kind == ExprKind::StringLiteral && ety->isIntegerTy())
			{
				ExprPtr it = args[pos++];
				return fold_global_init(it, ty);
			}
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

	// 指定符链展开后的物理路径步（P237）：成员步按 C 成员索引（手工布局另经
	// struct_member_ll_index 换算 LLVM 槽），下标步按数组元素序号。
	struct PhysStep
	{
		bool is_index = false;
		unsigned idx = 0;
	};

	// 链式指定符落地 helper（P237）。均定义在 fold_global_init / emit_init_list_store 之前，
	// 以「叶子类型/叶子常量/叶子地址」反转递归，避开类内先声明后定义同一成员的 C2535 限制。
	// 指定初始化器成员名 → C 成员索引链（穿过匿名聚合做成员名提升，与 sema/codegen 同口径）。
	// 尾部即目标字段所在聚合；索引链逐层记全（union 层由使用方按「偏移恒 0」跳过）。
	bool find_member_cpath(const std::string &tag, const std::string &fname,
		std::vector<unsigned> &cpath, int depth = 0)
	{
		if (depth > 16 || tag.empty()) return false;
		auto dit = struct_defs_.find(tag);
		if (dit == struct_defs_.end()) return false;
		const StructDef *sd = dit->second;
		for (std::size_t i = 0; i < sd->members.size(); ++i)
			if (!sd->members[i].name.empty() && sd->members[i].name == fname)
			{
				cpath.push_back(static_cast<unsigned>(i));
				return true;
			}
		for (std::size_t i = 0; i < sd->members.size(); ++i)
		{
			const auto &m = sd->members[i];
			if (!m.name.empty() || !m.type || m.type->tag.empty()) continue;
			if (m.type->kind != TypeKind::Struct && m.type->kind != TypeKind::Union) continue;
			cpath.push_back(static_cast<unsigned>(i));
			if (find_member_cpath(m.type->tag, fname, cpath, depth + 1)) return true;
			cpath.pop_back();
		}
		return false;
	}

	// 指定符链 → 物理路径（P237）：成员步经 find_member_cpath 穿匿名聚合展开成多级成员步，
	// 下标步要求当前为数组。成功要求链全部消费（i 走到末尾）、out 即完整路径；
	// 失败返回 false，调用方整块丢弃后回退旧路径。
	bool flatten_desig(llvm::Type *ty, const std::vector<DesigStep> &ds, std::size_t &i,
		std::vector<PhysStep> &out, int depth = 0)
	{
		if (i >= ds.size()) return true;
		if (depth > 16 || !ty) return false;
		const DesigStep &s = ds[i];
		if (s.is_index)
		{
			auto *aty = llvm::dyn_cast<llvm::ArrayType>(ty);
			if (!aty || s.index < 0) return false;
			out.push_back({true, static_cast<unsigned>(s.index)});
			++i;
			return flatten_desig(aty->getElementType(), ds, i, out, depth + 1);
		}
		auto *sty = llvm::dyn_cast<llvm::StructType>(ty);
		if (!sty) return false;
		std::string tag = tag_for_struct(sty);
		std::vector<unsigned> cpath;
		if (tag.empty() || !find_member_cpath(tag, s.name, cpath)) return false;
		for (unsigned ci : cpath)
		{
			auto dit = struct_defs_.find(tag);
			if (dit == struct_defs_.end() || ci >= dit->second->members.size()) return false;
			const StructMember &m = dit->second->members[ci];
			out.push_back({false, ci});
			ty = to_llvm_alloca_type(m.type);
			tag = m.type ? m.type->tag : std::string();
		}
		++i;
		return flatten_desig(ty, ds, i, out, depth + 1);
	}

	// 沿物理路径求叶子对象的 LLVM 类型（P237）：struct 层取成员类型、数组层取元素类型。
	llvm::Type *path_leaf_type(llvm::Type *ty, const std::vector<PhysStep> &path, std::size_t lvl = 0)
	{
		if (!ty) return nullptr;
		if (lvl >= path.size()) return ty;
		const PhysStep &st = path[lvl];
		if (st.is_index)
		{
			auto *aty = llvm::dyn_cast<llvm::ArrayType>(ty);
			if (!aty || st.idx >= aty->getNumElements()) return nullptr;
			return path_leaf_type(aty->getElementType(), path, lvl + 1);
		}
		auto *sty = llvm::dyn_cast<llvm::StructType>(ty);
		if (!sty) return nullptr;
		std::string tag = tag_for_struct(sty);
		auto dit = tag.empty() ? struct_defs_.end() : struct_defs_.find(tag);
		if (dit == struct_defs_.end() || st.idx >= dit->second->members.size()) return nullptr;
		return path_leaf_type(to_llvm_alloca_type(dit->second->members[st.idx].type), path, lvl + 1);
	}

	// 把已折叠好的叶子常量沿物理路径自内向外包成整对象常量（P237）：struct 层填
	// ConstantStruct 对应槽，union 层交 union_body_const 摊成 [N x i8] 载荷（LLVM 禁止
	// 聚合→字节数组的 bitcast），数组层填 ConstantArray 对应元素；其余位置清零。
	llvm::Constant *wrap_path_const(llvm::Type *ty, const std::vector<PhysStep> &path,
		std::size_t lvl, llvm::Constant *leaf)
	{
		if (!ty || !leaf) return nullptr;
		if (lvl >= path.size()) return leaf;
		const PhysStep &st = path[lvl];
		if (st.is_index)
		{
			auto *aty = llvm::dyn_cast<llvm::ArrayType>(ty);
			if (!aty || st.idx >= aty->getNumElements()) return nullptr;
			llvm::Constant *inner = wrap_path_const(aty->getElementType(), path, lvl + 1, leaf);
			if (!inner) return nullptr;
			std::vector<llvm::Constant *> elems(aty->getNumElements(),
				llvm::Constant::getNullValue(aty->getElementType()));
			elems[st.idx] = inner;
			return llvm::ConstantArray::get(aty, elems);
		}
		auto *sty = llvm::dyn_cast<llvm::StructType>(ty);
		if (!sty) return nullptr;
		std::string tag = tag_for_struct(sty);
		auto dit = tag.empty() ? struct_defs_.end() : struct_defs_.find(tag);
		if (dit == struct_defs_.end() || st.idx >= dit->second->members.size()) return nullptr;
		llvm::Constant *inner = wrap_path_const(
			to_llvm_alloca_type(dit->second->members[st.idx].type), path, lvl + 1, leaf);
		if (!inner) return nullptr;
		// union 层：先按成员类型把内层包好，再摊成 [N x i8] 载荷（成员偏移恒 0）
		if (dit->second->is_union) return union_body_const(sty, inner);
		unsigned slot = static_cast<unsigned>(struct_member_ll_index(sty, st.idx));
		if (slot >= sty->getNumElements()) return nullptr;
		std::vector<llvm::Constant *> elems(sty->getNumElements(), nullptr);
		for (std::size_t k = 0; k < sty->getNumElements(); ++k)
			elems[k] = llvm::Constant::getNullValue(sty->getElementType(k));
		elems[slot] = inner;
		return llvm::ConstantStruct::get(sty, elems);
	}

	// 沿物理路径下钻到叶子地址（P237，局部初始化）：struct 层按 LLVM 槽 GEP，
	// union 层成员偏移恒 0（地址不变），数组层按元素下标 GEP。
	bool path_leaf_addr(llvm::Value *&addr, llvm::Type *&ty, CTypePtr &ct,
		const std::vector<PhysStep> &path, std::size_t lvl = 0)
	{
		if (!addr || !ty) return false;
		if (lvl >= path.size()) return true;
		const PhysStep &st = path[lvl];
		if (st.is_index)
		{
			auto *aty = llvm::dyn_cast<llvm::ArrayType>(ty);
			if (!aty || st.idx >= aty->getNumElements()) return false;
			addr = builder.CreateGEP(aty, addr,
				{llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0),
				 llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), st.idx)});
			ct = (ct && ct->kind == TypeKind::Array && ct->base) ? ct->base : union_ctype_of(aty->getElementType());
			ty = aty->getElementType();
			return path_leaf_addr(addr, ty, ct, path, lvl + 1);
		}
		auto *sty = llvm::dyn_cast<llvm::StructType>(ty);
		if (!sty) return false;
		std::string tag = tag_for_struct(sty);
		auto dit = tag.empty() ? struct_defs_.end() : struct_defs_.find(tag);
		if (dit == struct_defs_.end() || st.idx >= dit->second->members.size()) return false;
		const StructMember &m = dit->second->members[st.idx];
		if (!dit->second->is_union)
		{
			unsigned slot = static_cast<unsigned>(struct_member_ll_index(sty, st.idx));
			if (slot >= sty->getNumElements()) return false;
			addr = builder.CreateStructGEP(sty, addr, slot);
		}
		ct = m.type;
		ty = to_llvm_alloca_type(m.type);
		return path_leaf_addr(addr, ty, ct, path, lvl + 1);
	}

	// ── P239：指定符之后位置项的 DFS 续位（C11 §6.7.9p17） ────────────────
	// 设计符之后的位置项须从「被指定子对象的下一个子对象」起按 DFS 序续位（本级无兄弟则
	// 逐层上溯）。旧实现一律把「顶层序号 +1」当续位点：`{ [1].x = 5, 6 }` 把 6 落 arr[2].x
	// （clang 为 arr[1].y）、`.ps[0].x = 1, 2, 3, 4` 只留 ps[0].x、其余静默丢。故把续位
	// 游标由序号升级为**物理路径**（与 P237 同一表征，成员步记 C 成员索引）。
	llvm::Type *phys_child_type(llvm::Type *ty, const PhysStep &st)
	{
		if (!ty) return nullptr;
		if (st.is_index)
		{
			auto *aty = llvm::dyn_cast<llvm::ArrayType>(ty);
			if (!aty || st.idx >= aty->getNumElements()) return nullptr;
			return aty->getElementType();
		}
		auto *sty = llvm::dyn_cast<llvm::StructType>(ty);
		if (!sty) return nullptr;
		std::string tag = tag_for_struct(sty);
		auto dit = tag.empty() ? struct_defs_.end() : struct_defs_.find(tag);
		if (dit == struct_defs_.end() || st.idx >= dit->second->members.size()) return nullptr;
		return to_llvm_alloca_type(dit->second->members[st.idx].type);
	}

	// 同一父对象内 st 的下一兄弟：数组→下一元素、struct→下一 C 成员、union→无
	// （union 同一时刻只有一个活跃成员，续位回到宿主层找兄弟）。
	bool phys_sibling_after(llvm::Type *parentTy, const PhysStep &st, PhysStep &out)
	{
		if (!parentTy) return false;
		if (st.is_index)
		{
			auto *aty = llvm::dyn_cast<llvm::ArrayType>(parentTy);
			if (!aty || st.idx + 1 >= aty->getNumElements()) return false;
			out.is_index = true;
			out.idx = st.idx + 1;
			return true;
		}
		auto *sty = llvm::dyn_cast<llvm::StructType>(parentTy);
		if (!sty) return false;
		std::string tag = tag_for_struct(sty);
		auto dit = tag.empty() ? struct_defs_.end() : struct_defs_.find(tag);
		if (dit == struct_defs_.end() || dit->second->is_union) return false;
		if (st.idx + 1 >= dit->second->members.size()) return false;
		out.is_index = false;
		out.idx = st.idx + 1;
		return true;
	}

	// 物理路径之后的下一个子对象（DFS 序）；无则 false —— 调用方据此把后续位置项判为 excess 丢弃。
	bool phys_next_path(llvm::Type *ty, const std::vector<PhysStep> &path, std::vector<PhysStep> &out)
	{
		std::vector<llvm::Type *> parents;
		llvm::Type *cur = ty;
		for (const auto &st : path)
		{
			parents.push_back(cur);
			llvm::Type *child = phys_child_type(cur, st);
			if (!child) return false;
			cur = child;
		}
		out.clear();
		for (std::size_t l = path.size(); l-- > 0;)
		{
			PhysStep sib;
			if (phys_sibling_after(parents[l], path[l], sib))
			{
				out.assign(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(l));
				out.push_back(sib);
				return true;
			}
		}
		return false;
	}

	// 把路径末端的聚合展开到「第一个叶子」：位置项落在聚合子对象上时，brace elision
	// 使其实际初始化的是该聚合的首个叶子（clang `{ .ps[0] = {1,2}, 3 }` 的 3 → ps[1].x）。
	void phys_descend_first(llvm::Type *ty, std::vector<PhysStep> &path)
	{
		for (int guard = 0; guard < 32; ++guard)
		{
			llvm::Type *leaf = path_leaf_type(ty, path, 0);
			if (!leaf) return;
			PhysStep s;
			if (auto *aty = llvm::dyn_cast<llvm::ArrayType>(leaf))
			{
				if (aty->getNumElements() == 0) return;
				s.is_index = true;
			}
			else
			{
				auto *sty = llvm::dyn_cast<llvm::StructType>(leaf);
				if (!sty) return;
				std::string tag = tag_for_struct(sty);
				auto dit = tag.empty() ? struct_defs_.end() : struct_defs_.find(tag);
				if (dit == struct_defs_.end() || dit->second->members.empty()) return;
				s.is_index = false;
			}
			s.idx = 0;
			path.push_back(s);
		}
	}

	// 把叶子常量按物理路径**合并**进既有整对象常量（P239）：同层可能已有其它指定项，
	// 不能像 wrap_path_const 那样整块重建。union 层按本次写入重建载荷（同一 union 逐成员
	// 交错覆盖属未定义区，取后写者，与旧行为一致）。
	llvm::Constant *merge_path_const(llvm::Type *ty, const std::vector<PhysStep> &path,
		std::size_t lvl, llvm::Constant *base, llvm::Constant *leaf)
	{
		if (!ty || !leaf) return base;
		if (lvl >= path.size()) return leaf;
		const PhysStep &st = path[lvl];
		if (st.is_index)
		{
			auto *aty = llvm::dyn_cast<llvm::ArrayType>(ty);
			if (!aty || st.idx >= aty->getNumElements()) return base;
			std::vector<llvm::Constant *> elems;
			for (unsigned k = 0; k < aty->getNumElements(); ++k)
			{
				llvm::Constant *ce = base ? base->getAggregateElement(k) : nullptr;
				elems.push_back(ce ? ce : llvm::Constant::getNullValue(aty->getElementType()));
			}
			llvm::Constant *nested = merge_path_const(aty->getElementType(), path, lvl + 1, elems[st.idx], leaf);
			if (!nested) return base;
			elems[st.idx] = nested;
			return llvm::ConstantArray::get(aty, elems);
		}
		auto *sty = llvm::dyn_cast<llvm::StructType>(ty);
		if (!sty) return base;
		std::string tag = tag_for_struct(sty);
		auto dit = tag.empty() ? struct_defs_.end() : struct_defs_.find(tag);
		if (dit == struct_defs_.end() || st.idx >= dit->second->members.size()) return base;
		llvm::Type *mt = to_llvm_alloca_type(dit->second->members[st.idx].type);
		if (!mt) return base;
		if (dit->second->is_union)
		{
			llvm::Constant *inner = merge_path_const(mt, path, lvl + 1, nullptr, leaf);
			return inner ? union_body_const(sty, inner) : base;
		}
		unsigned slot = static_cast<unsigned>(struct_member_ll_index(sty, st.idx));
		if (slot >= sty->getNumElements()) return base;
		std::vector<llvm::Constant *> elems;
		for (unsigned k = 0; k < sty->getNumElements(); ++k)
		{
			llvm::Constant *ce = base ? base->getAggregateElement(k) : nullptr;
			elems.push_back(ce ? ce : llvm::Constant::getNullValue(sty->getElementType(k)));
		}
		llvm::Constant *nested = merge_path_const(mt, path, lvl + 1, elems[slot], leaf);
		if (!nested) return base;
		elems[slot] = nested;
		return llvm::ConstantStruct::get(sty, elems);
	}

	// 将初始化表达式折叠为 LLVM 常量（全局变量初始化器用）
	llvm::Constant *fold_global_init(const ExprPtr &e, llvm::Type *ty, bool *out_folded = nullptr)
	{
		if (out_folded) *out_folded = true; // P219：默认视为可折叠，仅于末尾 fallback 置 false
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
				// C §6.3.1.4：无符号常量表达式须按值转浮点。fold_int_const 以 intmax_t 回绕，
				// `18446744073709551615ULL` 得 -1，不重解释就会折成 -1.0（局部路径经 uitofp 正确）。
				CTypePtr vt = get_expr_ctype(e);
				if (!vt) vt = e->type;
				if (vt && is_unsigned_int_type(vt))
					return llvm::ConstantFP::get(ty, static_cast<double>(static_cast<uint64_t>(v)));
				return llvm::ConstantFP::get(ty, static_cast<double>(v));
			}
		}
		// 浮点字面量（含一元 ±/cast 包裹）：-1.5f 在 AST 中为 Unary("-") 包 FloatLiteral，
		// 旧代码只认裸 FloatLiteral，负浮点常量初始化一律折成 0（CTS 0054 族）。
		if (ty->isFloatTy() || ty->isDoubleTy())
		{
			const Expr *fe = e.get();
			bool neg = false;
			while (fe)
			{
				if (fe->kind == ExprKind::Cast) { fe = fe->lhs.get(); continue; }
				if (fe->kind == ExprKind::Unary && fe->op == "-") { neg = !neg; fe = fe->lhs.get(); continue; }
				if (fe->kind == ExprKind::Unary && fe->op == "+") { fe = fe->lhs.get(); continue; }
				break;
			}
			if (fe && fe->kind == ExprKind::FloatLiteral)
				return llvm::ConstantFP::get(ty, neg ? -fe->float_val : fe->float_val);
		}
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
				// §6.7.9p14/p19：单字符串元素的花括号 char s[] = {"str"} ≡ = "str"，整串填入数组。
				// 否则下方位置循环会把整串折进单个 char 元素→全零（CTS 0011 空输出族）。
				if (!has_desig && e->args.size() == 1 && e->args[0] &&
					e->args[0]->kind == ExprKind::StringLiteral && elem_ty->isIntegerTy())
					return fold_global_init(e->args[0], ty);
				if (!has_desig && elem_ty->isAggregateType()
					&& !e->args.empty() && e->args[0] && e->args[0]->kind != ExprKind::InitList)
				{
					std::size_t cur = 0;
					for (std::size_t k = 0; k < aty->getNumElements(); ++k)
						elems[k] = elide_fold(elem_ty, e->args, cur);
					return llvm::ConstantArray::get(aty, elems);
				}
				std::size_t pos = 0;
				// P239：链式/单层指定符之后，续位游标改记物理路径（cur_on=false 时逐字走旧序号语义）
				std::vector<PhysStep> ppath;
				bool cur_on = false;
				for (const auto &ae : e->args)
				{
					// 链式指定符（P237）：`[1][2] = 4` 走物理路径常量（首级必为下标）。
					// 旧路径只看首级 desig_index 并把值整块折进 elems[首级]（`[1][2]` 当 `[1]`）。
					if (ae && ae->desig_chain.size() >= 2)
					{
						std::vector<PhysStep> path;
						std::size_t dcur = 0;
						if (flatten_desig(aty, ae->desig_chain, dcur, path, 0)
							&& dcur == ae->desig_chain.size() && !path.empty()
							&& path[0].is_index && path[0].idx < aty->getNumElements())
						{
							llvm::Type *lt = path_leaf_type(elem_ty, path, 1);
							llvm::Constant *c0 = lt ? fold_global_init(ae, lt) : nullptr;
							if (llvm::Constant *cv = c0 ? wrap_path_const(elem_ty, path, 1, c0) : nullptr)
							{
								elems[path[0].idx] = cv;
								pos = path[0].idx + 1;
								std::vector<PhysStep> nx;
								cur_on = phys_next_path(aty, path, nx); // P239：游标接在该路径之后
								ppath.swap(nx);
								continue;
							}
						}
					}
					// 单层指定符：写入后把游标设在「该元素之后」（P239，使后续位置项精确续位）
					if (ae->desig_index >= 0)
					{
						std::size_t idx = static_cast<std::size_t>(ae->desig_index);
						pos = idx + 1;
						if (idx < aty->getNumElements())
						{
							elems[idx] = fold_global_init(ae, elem_ty);
							std::vector<PhysStep> one(1);
							one[0].is_index = true;
							one[0].idx = static_cast<unsigned>(idx);
							std::vector<PhysStep> nx;
							cur_on = phys_next_path(aty, one, nx);
							ppath.swap(nx);
						}
						continue;
					}
					// 位置项（P239）：有物理游标则按 DFS 续位；无障碍时回落旧序号语义（行为逐字不变）
					if (cur_on)
					{
						std::vector<PhysStep> tgt = ppath;
						if (ae->kind != ExprKind::InitList) phys_descend_first(aty, tgt);
						llvm::Type *lt = path_leaf_type(aty, tgt, 0);
						llvm::Constant *c0 = lt ? fold_global_init(ae, lt) : nullptr;
						if (c0 && !tgt.empty() && tgt[0].is_index && tgt[0].idx < aty->getNumElements())
							elems[tgt[0].idx] = merge_path_const(elem_ty, tgt, 1, elems[tgt[0].idx], c0);
						if (!tgt.empty() && tgt[0].is_index) pos = tgt[0].idx + 1;
						std::vector<PhysStep> nx;
						cur_on = phys_next_path(aty, tgt, nx);
						ppath.swap(nx);
						continue;
					}
					std::size_t idx = pos;
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
					// 链式指定符（P237）：位域 struct 也走通用物理路径（首级为本层成员）。
					// 旧分支只按首级名查 bitfield_access_/field_index_map_，会把 `.a[1]` 当 `.a`。
					auto apply_desig_chain = [&](const ExprPtr &se, std::size_t &root) -> bool
					{
						if (!se || se->desig_chain.size() < 2) return false;
						std::vector<PhysStep> path;
						std::size_t dcur = 0;
						if (!flatten_desig(sty, se->desig_chain, dcur, path, 0)
							|| dcur != se->desig_chain.size() || path.empty() || path[0].is_index)
							return false;
						if (gsd == struct_defs_.end() || path[0].idx >= gm.size()) return false;
						std::size_t slot = gsd->second->is_union ? 0 : struct_member_ll_index(sty, path[0].idx);
						if (slot >= elems.size()) return false;
						llvm::Type *mt = to_llvm_alloca_type(gm[path[0].idx].type);
						llvm::Type *lt = path_leaf_type(mt, path, 1);
						llvm::Constant *c0 = lt ? fold_global_init(se, lt) : nullptr;
						llvm::Constant *cv = c0 ? wrap_path_const(mt, path, 1, c0) : nullptr;
						if (!cv) return false;
						elems[slot] = cv;
						root = path[0].idx;
						return true;
					};
					for (const auto &se : e->args)
					{
						std::size_t root = 0;
						if (apply_desig_chain(se, root)) { cpos = root + 1; continue; }
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
				// P239：指定符之后位置项的物理续位游标
				std::vector<PhysStep> ppath;
				bool cur_on = false;
				for (std::size_t ai = 0; ai < e->args.size(); ++ai)
				{
					ExprPtr se = e->args[ai];
					std::size_t idx;
					// 位置项（P239）：指定符之后按物理路径 DFS 续位；聚合作子对象时展开到首叶
					if (cur_on && se->desig_chain.empty() && se->desig_field.empty()
						&& se->desig_index < 0 && gsd != struct_defs_.end())
					{
						std::vector<PhysStep> tgt = ppath;
						if (se->kind != ExprKind::InitList) phys_descend_first(sty, tgt);
						llvm::Type *lt = path_leaf_type(sty, tgt, 0);
						llvm::Constant *c0 = lt ? fold_global_init(se, lt) : nullptr;
						if (c0 && !tgt.empty() && !tgt[0].is_index
							&& tgt[0].idx < gsd->second->members.size())
						{
							std::size_t slot = gsd->second->is_union ? 0 : struct_member_ll_index(sty, tgt[0].idx);
							llvm::Type *mt = to_llvm_alloca_type(gsd->second->members[tgt[0].idx].type);
							if (mt && slot < elems.size())
								elems[slot] = merge_path_const(mt, tgt, 1, elems[slot], c0);
							pos = tgt[0].idx + 1;
						}
						std::vector<PhysStep> nx;
						cur_on = phys_next_path(sty, tgt, nx);
						ppath.swap(nx);
						continue;
					}
					// 链式指定符（P237）：`.ps[1].y` / `.a.b` 走通用物理路径（成员名先穿匿名聚合展开，
					// 首级为本层成员）。旧路径只看首级 desig_field，把 `.ps[1].y` 当 `.ps`。
					{
						std::vector<PhysStep> path;
						std::size_t dcur = 0;
						if (se->desig_chain.size() >= 2 && gsd != struct_defs_.end()
							&& flatten_desig(sty, se->desig_chain, dcur, path, 0)
							&& dcur == se->desig_chain.size() && !path.empty() && !path[0].is_index
							&& path[0].idx < gsd->second->members.size())
						{
							std::size_t slot = gsd->second->is_union ? 0 : struct_member_ll_index(sty, path[0].idx);
							llvm::Type *mt = to_llvm_alloca_type(gsd->second->members[path[0].idx].type);
							llvm::Type *lt = (slot < elems.size()) ? path_leaf_type(mt, path, 1) : nullptr;
							llvm::Constant *c0 = lt ? fold_global_init(se, lt) : nullptr;
							llvm::Constant *cv = c0 ? wrap_path_const(mt, path, 1, c0) : nullptr;
							if (cv)
							{
								elems[slot] = cv;
								pos = path[0].idx + 1;
								std::vector<PhysStep> nx;
								cur_on = phys_next_path(sty, path, nx); // P239：游标接在该路径之后
								ppath.swap(nx);
								continue;
							}
						}
					}
					// 指定初始化器名提升（P236）：`.提升名` 折叠到匿名聚合内的真实字段。
					// 旧路径只认直接成员、失配回退 pos（此处恒 0）→ 值静默写进内层偏移 0（§6.7.9p17）。
					std::vector<unsigned> cpath;
					if (!se->desig_field.empty() && gsd != struct_defs_.end()
						&& find_member_cpath(gtag, se->desig_field, cpath) && cpath.size() > 1)
					{
						const StructDef *psd = gsd->second;
						CTypePtr mt = (cpath[0] < psd->members.size()) ? psd->members[cpath[0]].type : nullptr;
						std::size_t slot = psd->is_union ? 0 : struct_member_ll_index(sty, cpath[0]);
						llvm::Constant *cv = mt ? build_desig_const(mt->tag, cpath, 1, se) : nullptr;
						if (cv && slot < elems.size()) elems[slot] = cv;
						pos = cpath[0] + 1;
						// P239：游标接在「匿名聚合全链之后」；`.hi` 之后无可续成员时逐层上溯
						{
							std::vector<PhysStep> full;
							for (unsigned ci : cpath)
							{
								PhysStep s;
								s.is_index = false;
								s.idx = ci;
								full.push_back(s);
							}
							std::vector<PhysStep> nx;
							cur_on = phys_next_path(sty, full, nx);
							ppath.swap(nx);
						}
						continue;
					}
					if (!se->desig_field.empty())
					{
						idx = find_struct_field_idx(sty, se->desig_field, pos);
						pos = idx + 1;
						// P239：游标接在该成员之后（记 C 成员索引，穿匿名聚合的多级情形已由上方 continue）
						std::vector<unsigned> dcp;
						if (gsd != struct_defs_.end() && find_member_cpath(gtag, se->desig_field, dcp)
							&& !dcp.empty())
						{
							std::vector<PhysStep> full;
							for (unsigned ci : dcp)
							{
								PhysStep s;
								s.is_index = false;
								s.idx = ci;
								full.push_back(s);
							}
							std::vector<PhysStep> nx;
							cur_on = phys_next_path(sty, full, nx);
							ppath.swap(nx);
						}
					}
					else
					{
						idx = struct_member_ll_index(sty, pos); // 手工布局: C 成员序→LLVM 索引
						++pos;
					}
					if (idx >= sty->getNumElements()) break;
					llvm::Type *ft = sty->getElementType(idx);
					// 聚合成员 + 非括号项：按 brace elision 从当前游标递归折叠（§6.7.9p20）。
					// 旧代码直接 fold_global_init(标量, 聚合类型) → getNullValue，使
					// `struct{struct{int a;} inst;} x = {1}` 的嵌套成员恒为 0（CTS 0149 族）。
					if (ft->isAggregateType())
					{
						if (se->kind == ExprKind::InitList) { elems[idx] = fold_global_init(se, ft); continue; }
						CTypePtr et = get_expr_ctype(se);
						bool agg_value = et && (et->kind == TypeKind::Struct || et->kind == TypeKind::Union);
						if (!agg_value)
						{
							std::size_t cur = ai;
							if (llvm::Constant *c = elide_fold(ft, e->args, cur)) elems[idx] = c;
							if (cur > ai) ai = cur - 1;
							continue;
						}
					}
					elems[idx] = fold_global_init(se, ft);
				}
				return llvm::ConstantStruct::get(sty, elems);
				}
				// 标量整数/浮点（含 enum）的花括号初始化 `T x = {v}`（旧代码 InitList 仅处理 array/struct，标量 ty 两分支都不命中
				// →落到末尾 getNullValue → 0。CTS 0150 族全局 enum en={1}、int g={1} 全 0）：按 C §6.7.9 取首元素
				// 递归折叠（可嵌套 {{v}}）；空 {} 保持零初始化。**仅限整数/浮点标量**：指针类型不 return，
				// 留给下方 pointer 块（保持复合字面量 (T){...} 取址/字符串数组退化等旧行为，否则 p41 会误取 args[0] 崩）。
				if (ty->isIntegerTy() || ty->isFloatingPointTy())
				{
					if (!e->args.empty())
						return fold_global_init(e->args[0], ty, out_folded);
					return llvm::Constant::getNullValue(ty);
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
				const int w = co_str_lit_width(e->str_val);
				llvm::ArrayType *saty = nullptr;
				llvm::Constant *data = nullptr;
				if (w == 1)
				{
					std::string decoded = co_decode_string(e->str_val);
					data = llvm::ConstantDataArray::getString(ctx, llvm::StringRef(decoded), /*AddNull*/ true);
					saty = llvm::cast<llvm::ArrayType>(data->getType());
				}
				else
				{
					// 全局宽字符指针初值（wchar_t *p = L"…" / char32_t *q = U"…"）：按前缀三路降
					// UTF-16/UTF-32（与 gen_expr 右值发射、fold 数组分支同口径）；旧路径恒发 i8 会与 wchar_t* 点位宽不符。
					llvm::Type *ety = (w == 2) ? static_cast<llvm::Type *>(llvm::Type::getInt16Ty(ctx))
					                           : static_cast<llvm::Type *>(llvm::Type::getInt32Ty(ctx));
					std::vector<llvm::Constant *> elems;
					if (w == 2)
						for (uint16_t c : co_decode_utf16(e->str_val)) elems.push_back(llvm::ConstantInt::get(ety, c));
					else
						for (uint32_t c : co_decode_utf32(e->str_val)) elems.push_back(llvm::ConstantInt::get(ety, c));
					elems.push_back(llvm::ConstantInt::get(ety, 0));
					saty = llvm::ArrayType::get(ety, elems.size());
					data = llvm::ConstantArray::get(saty, elems);
				}
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
			if (out_folded) *out_folded = false; // P219：指针初值不可折叠（如函数调用）→ 交运行期 store
			return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty));
		}
		if (out_folded) *out_folded = false; // P219：标量初值不可折叠（如函数调用/读运行时变量）→ 交运行期 store
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
				// void 形参属不完整类型（parser 已报 error）：兜底不建 alloca（void 类型会崩）。
				if (!pty || pty->kind == TypeKind::Void)
					continue;
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
		// 注：用 getNullValue 而非 ConstantInt::get——返回类型为 ptr（struct 值建模）
		// 或浮点时 ConstantInt::get 会造出非法常量（`ret i0 0`）触发 verifier 报错；
		// 整数返回仍是 0，行为不变。
		if (builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())
		{
			if (F->getReturnType()->isVoidTy())
				builder.CreateRetVoid();
			else
				builder.CreateRet(llvm::Constant::getNullValue(F->getReturnType()));
		}
		// 空且无终止符的死块（如 return 之后只含无指令可发的声明、延续块最终未被写入）：
		// 补 unreachable，否则 verifier 报 "Basic Block does not have terminator"。
		// 仅扫空块——非空块缺终止符仍按真实缺陷交给 verifier 报出。
		for (llvm::BasicBlock &bb : *F)
		{
			if (bb.empty())
			{
				builder.SetInsertPoint(&bb);
				builder.CreateUnreachable();
			}
		}
		std::string verify_err;
		llvm::raw_string_ostream vs(verify_err);
		if (llvm::verifyFunction(*F, &vs))
		{
			errs.push_back(Error{fn.loc.file, Pos{fn.loc.line, fn.loc.column},
				"IR verification: " + verify_err});
		}
	}

	// 位域成员的掩码写入（局部，P242）：读存储槽 → 清 (mask<<off) 位 → 并入 (v&mask)<<off → 写回。
	//   与 P191 全局位域分支同口径（存储槽 = ba.storage_idx、掩码宽 = ba.bit_width、位移 = ba.bit_offset）。
	//   整槽 store 会覆盖同存储单元的其它位域（如 {int a:4;int b:4;} 写 a 清掉 b），故必须按位合并。
	void store_bitfield_at(llvm::StructType *sty, llvm::Value *addr, const ExprPtr &val, const BitfieldAccess &ba)
	{
		if (!sty || ba.storage_idx >= sty->getNumElements()) return;
		llvm::Type *stor_ty = get_int_ty_for_bits(ba.unit_bits);
		unsigned ub = stor_ty->getIntegerBitWidth();
		uint64_t mask = (ba.bit_width >= ub) ? ~0ULL : ((1ULL << ba.bit_width) - 1);
		ExprPtr it = val;
		if (it && it->kind == ExprKind::InitList && it->args.size() == 1) it = it->args[0];
		if (!it) return;
		llvm::Value *v = gen_expr(it);
		if (!v) return;
		if (v->getType() != stor_ty) v = convert_value(v, stor_ty, get_expr_ctype(it));
		v = builder.CreateAnd(v, llvm::ConstantInt::get(stor_ty, mask));
		if (ba.bit_offset > 0)
			v = builder.CreateShl(v, llvm::ConstantInt::get(stor_ty, ba.bit_offset));
		uint64_t clr = ~(mask << ba.bit_offset);
		llvm::Value *stor_addr = builder.CreateStructGEP(sty, addr, ba.storage_idx);
		llvm::Value *cur = builder.CreateLoad(stor_ty, stor_addr);
		cur = builder.CreateAnd(cur, llvm::ConstantInt::get(stor_ty, clr));
		cur = builder.CreateOr(cur, v);
		builder.CreateStore(cur, stor_addr);
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
		// union 包装：首成员为聚合时扁平项按 brace elision 依次写入该成员（§6.7.9p20），
		// 否则取一项作用于首成员（store 到偏移 0）。
		if (ty->isStructTy())
		{
			auto *sty = llvm::cast<llvm::StructType>(ty);
			if (is_union_struct(sty))
			{
				if (pos >= args.size()) return;
				std::string utag = std::string(llvm::StringRef(sty->getName()).drop_front(7));
				auto usd = struct_defs_.find(utag);
				llvm::Type *fmt = (usd != struct_defs_.end() && !usd->second->members.empty())
					? to_llvm_alloca_type(usd->second->members[0].type) : nullptr;
				if (fmt && fmt->isAggregateType())
				{
					elide_store(fmt, addr, args, pos);
					return;
				}
				ExprPtr it = args[pos++];
				if (it && it->kind == ExprKind::InitList && !it->args.empty()) it = it->args[0];
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
			// 字符串字面量整体初始化字符数组（§6.7.9p14）：在扁平游标中只占一项
			if (pos < args.size() && args[pos] && args[pos]->kind == ExprKind::StringLiteral && ety->isIntegerTy())
			{
				ExprPtr it = args[pos++];
				if (llvm::Constant *c = fold_global_init(it, ty))
					builder.CreateStore(c, addr);
				return;
			}
			for (std::size_t k = 0; k < aty->getNumElements(); ++k)
			{
				llvm::Value *ii = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), k);
				llvm::Value *ptr = builder.CreateGEP(aty, addr, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), ii});
				elide_store(ety, ptr, args, pos);
			}
			return;
		}
		auto *sty = llvm::cast<llvm::StructType>(ty);
		// 具名 struct：按 C 成员序共享游标——位域成员掩码/移位合并进存储单元、聚合成员递归 elision。
		// 旧实现按 LLVM 槽序消费，位域折叠使槽数 < C 成员数（如 {u8 m1; u8 m2:3; u8 m3:3} 只有 2 槽），
		// 第 3 项被丢（CTS 0013/0093 位域族）。
		std::string tag = tag_for_struct(sty);
		auto sd = struct_defs_.find(tag);
		if (!tag.empty() && sd != struct_defs_.end() && !sd->second->members.empty())
		{
			const auto &ms = sd->second->members;
			unsigned next_idx = 0;
			for (std::size_t i = 0; i < ms.size() && pos < args.size(); ++i)
			{
				const auto &m = ms[i];
				if (m.name.empty()) continue; // 匿名位域/匿名填充不占初始化项
				std::string key = tag + "." + m.name;
				auto bf = bitfield_access_.find(key);
				if (bf != bitfield_access_.end())
				{
					store_bitfield_at(sty, addr, args[pos++], bf->second);
					continue;
				}
				auto fi = field_index_map_.find(key);
				unsigned idx = next_idx;
				if (fi != field_index_map_.end()) idx = fi->second;
				if (idx >= sty->getNumElements()) break;
				next_idx = idx + 1;
				llvm::Value *ptr = builder.CreateStructGEP(sty, addr, idx);
				elide_store(sty->getElementType(idx), ptr, args, pos);
			}
			return;
		}
		for (std::size_t idx = 0; idx < sty->getNumElements(); ++idx)
		{
			llvm::Value *ptr = builder.CreateStructGEP(sty, addr, static_cast<unsigned>(idx));
			elide_store(sty->getElementType(idx), ptr, args, pos);
		}
	}

	// 局部复合初始化器: 逐元素 store 到 alloca/GlobalVariable
	void emit_init_list_store(const ExprPtr &e, llvm::Value *addr, llvm::Type *ty, const CTypePtr &ct = nullptr)
	{
		if (e->kind != ExprKind::InitList) return;
		// union：LLVM 表示是「首成员大小的字节数组」（get_struct_type 的 is_union 分支），
		//   初始化列表只初始化首个具名成员（C11 §6.7.9p12）。旧实现落到 struct 分支按
		//   [N x i8] 槽处理 → convert_value(ptr → [N x i8]) 造出 ptrtoint 到数组（非法 IR，
		//   CTS 0059 族）。此处按 CType 判定 union 后直接写首成员类型。
		if (ct && ct->kind == TypeKind::Union)
		{
			if (e->args.empty()) return;
			// 指定初始化器（P238）：union 宿主的 `.成员 = v` 须落到**被指定**的成员（§6.7.9p12）。
			// 旧实现无条件按首成员类型写入 —— `union { int i; long long q; } l = { .q = 0x1122334455667788 }`
			// 被截成 32 位写进 i，局部同样静默误编译（探针 `_tmp/p238_b.co`）。
			// 链式（`.p.y` / `.a[1]`）复用 P237 物理路径：union 层偏移恒 0、`path_leaf_addr` 已跳过 GEP。
			if (e->args[0] && !e->args[0]->desig_chain.empty())
			{
				const ExprPtr &d0 = e->args[0];
				std::vector<PhysStep> path;
				std::size_t dcur = 0;
				if (flatten_desig(ty, d0->desig_chain, dcur, path, 0)
					&& dcur == d0->desig_chain.size() && !path.empty() && !path[0].is_index)
				{
					llvm::Value *lp = addr;
					llvm::Type *lt = ty;
					CTypePtr lct = ct;
					if (path_leaf_addr(lp, lt, lct, path))
					{
						if (lt->isAggregateType() && d0->kind == ExprKind::InitList)
							emit_init_list_store(d0, lp, lt, lct);
						else
						{
							llvm::Value *v = gen_expr(d0);
							if (v->getType() != lt) v = convert_value(v, lt, get_expr_ctype(d0));
							builder.CreateStore(v, lp);
						}
						return;
					}
				}
				// 解析失败（非本 union 成员 / 越界）：回落下方首成员路径，不误写
			}
			const ExprPtr &first = e->args[0];
			CTypePtr mt;
			auto sd = struct_defs_.find(ct->tag);
			if (sd != struct_defs_.end() && !sd->second->members.empty())
				mt = sd->second->members[0].type;
			llvm::Type *fty = mt ? to_llvm_alloca_type(mt) : llvm::Type::getInt32Ty(ctx);
			// 存储体 { [N x i8] } 的元素 0 即成员地址（union 全部成员偏移 0）
			llvm::Value *ptr = builder.CreateStructGEP(ty, addr, 0);
			if (fty->isAggregateType())
			{
				if (first->kind == ExprKind::InitList)
					emit_init_list_store(first, ptr, fty, mt);
				else
				{
					// 扁平列表：首成员为聚合时按 brace elision 依次写入该成员（§6.7.9p20），
					// 旧实现只取首项 → union{struct{8 位域}} x={0x00,0xff,...} 只初始化第 1 个位域。
					std::size_t cur = 0;
					elide_store(fty, ptr, e->args, cur);
				}
				return;
			}
			// 标量成员的括号包裹初值 `{ { 5 } }`（§6.7.9p11：标量初始化式可选加括号）：
			// 全局 fold_global_init 对 InitList + 标量会递归取首项，局部旧实现把内层 InitList
			// 直接交 gen_expr → 值恒 0（静默）。此处同口径取首项（P238 顺手修）。
			ExprPtr item = first;
			while (item && item->kind == ExprKind::InitList && !item->args.empty()) item = item->args[0];
			if (!item) return;
			llvm::Value *v = gen_expr(item);
			if (v->getType() != fty) v = convert_value(v, fty, get_expr_ctype(item));
			builder.CreateStore(v, ptr);
			return;
		}
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
			// P239：指定符之后位置项的物理续位游标
			std::vector<PhysStep> ppath;
			bool cur_on = false;
			for (const auto &elem : e->args)
			{
				// 链式指定符（P237）：`[1][2] = 4` / `[1].x = 5` 逐级 GEP 后写入（首级必为下标）
				if (elem->desig_chain.size() >= 2)
				{
					std::vector<PhysStep> path;
					std::size_t dcur = 0;
					if (flatten_desig(aty, elem->desig_chain, dcur, path, 0)
						&& dcur == elem->desig_chain.size() && !path.empty()
						&& path[0].is_index && path[0].idx < aty->getNumElements())
					{
						llvm::Value *lp = addr;
						llvm::Type *lt = aty;
						CTypePtr lct = ct;
						if (path_leaf_addr(lp, lt, lct, path))
						{
							if (lt->isAggregateType() && elem->kind == ExprKind::InitList)
								emit_init_list_store(elem, lp, lt, lct);
							else
							{
								llvm::Value *v = gen_expr(elem);
								if (v->getType() != lt) v = convert_value(v, lt, get_expr_ctype(elem));
								builder.CreateStore(v, lp);
							}
							pos = path[0].idx + 1;
							std::vector<PhysStep> nx;
							cur_on = phys_next_path(aty, path, nx); // P239：游标接在该路径之后
							ppath.swap(nx);
							continue;
						}
					}
				}
				// 位置项（P239）：有物理游标则按 DFS 续位；无障碍时回落旧序号语义（行为逐字不变）
				if (cur_on)
				{
					std::vector<PhysStep> tgt = ppath;
					if (elem->kind != ExprKind::InitList) phys_descend_first(aty, tgt);
					llvm::Value *lp = addr;
					llvm::Type *lt = aty;
					CTypePtr lct = ct;
					if (!tgt.empty() && tgt[0].is_index && path_leaf_addr(lp, lt, lct, tgt))
					{
						if (lt->isAggregateType() && elem->kind == ExprKind::InitList)
							emit_init_list_store(elem, lp, lt, lct);
						else
						{
							llvm::Value *v = gen_expr(elem);
							if (v->getType() != lt) v = convert_value(v, lt, get_expr_ctype(elem));
							builder.CreateStore(v, lp);
						}
						pos = tgt[0].idx + 1;
					}
					std::vector<PhysStep> nx;
					cur_on = phys_next_path(aty, tgt, nx);
					ppath.swap(nx);
					continue;
				}
				// 指定索引或顺序
				if (elem->desig_index >= 0)
				{
					std::size_t idx = static_cast<std::size_t>(elem->desig_index);
					pos = idx + 1;
					if (idx < aty->getNumElements())
					{
						llvm::Value *ii = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), idx);
						llvm::Value *ptr = builder.CreateGEP(aty, addr, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), ii});
						CTypePtr ect = (ct && ct->kind == TypeKind::Array && ct->base) ? ct->base : union_ctype_of(elem_ty);
						if (elem_ty->isAggregateType() && elem->kind == ExprKind::InitList)
							emit_init_list_store(elem, ptr, elem_ty, ect);
						else
						{
							llvm::Value *v = gen_expr(elem);
							if (v->getType() != elem_ty) v = convert_value(v, elem_ty, get_expr_ctype(elem));
							builder.CreateStore(v, ptr);
						}
						std::vector<PhysStep> one(1);
						one[0].is_index = true;
						one[0].idx = static_cast<unsigned>(idx);
						std::vector<PhysStep> nx;
						cur_on = phys_next_path(aty, one, nx);
						ppath.swap(nx);
					}
					continue;
				}
				std::size_t idx = pos;
				pos = idx + 1;
				if (idx >= aty->getNumElements()) break;
				llvm::Value *ii = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), idx);
				llvm::Value *ptr = builder.CreateGEP(aty, addr, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), ii});
				// 元素 C 类型下传：union 元素必须走 ct==Union 分支，不能把载荷 [N x i8] 当数组
				CTypePtr ect = (ct && ct->kind == TypeKind::Array && ct->base) ? ct->base : union_ctype_of(elem_ty);
				if (elem_ty->isAggregateType() && elem->kind == ExprKind::InitList)
					emit_init_list_store(elem, ptr, elem_ty, ect);
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
					// 交 elide_store 的 struct 分支：按 C 成员序消费（位域合并、聚合成员递归），
					// 不按 LLVM 槽序（位域折叠后槽数 < C 成员数会丢项）。
					elide_store(sty, addr, e->args, cur);
					return;
				}
			}
			std::size_t pos = 0;
			// P239：指定符之后位置项的物理续位游标。含位域的 struct 不启用 —— 位域与同居成员共享
			// 存储单元，按物理路径整槽 store 会覆盖同槽其它位域（与全局位域分支同口径的保守回退）。
			bool lbf = false;
			{
				std::string btag = tag_for_struct(sty);
				auto bd = btag.empty() ? struct_defs_.end() : struct_defs_.find(btag);
				if (bd != struct_defs_.end())
					for (const auto &mm : bd->second->members)
						if (mm.bit_width >= 0) { lbf = true; break; }
			}
			std::vector<PhysStep> ppath;
			bool cur_on = false;
			for (std::size_t ai = 0; ai < e->args.size(); ++ai)
			{
				ExprPtr elem = e->args[ai];
				llvm::Value *ptr = nullptr;
				llvm::Type *ft = nullptr;
				// 位置项（P239）：指定符之后按物理路径 DFS 续位；聚合作子对象时展开到首叶
				if (cur_on && elem->desig_chain.empty() && elem->desig_field.empty()
					&& elem->desig_index < 0)
				{
					std::vector<PhysStep> tgt = ppath;
					if (elem->kind != ExprKind::InitList) phys_descend_first(sty, tgt);
					llvm::Value *lp = addr;
					llvm::Type *lt = sty;
					CTypePtr lct = ct;
					if (!tgt.empty() && !tgt[0].is_index && path_leaf_addr(lp, lt, lct, tgt))
					{
						if (lt->isAggregateType() && elem->kind == ExprKind::InitList)
							emit_init_list_store(elem, lp, lt, lct);
						else
						{
							llvm::Value *v = gen_expr(elem);
							if (v->getType() != lt) v = convert_value(v, lt, get_expr_ctype(elem));
							builder.CreateStore(v, lp);
						}
						pos = tgt[0].idx + 1;
					}
					std::vector<PhysStep> nx;
					cur_on = !lbf && phys_next_path(sty, tgt, nx);
					ppath.swap(nx);
					continue;
				}
				// 链式指定符（P237）：`.ps[1].y` / `.a.b` 逐级 GEP 后写入（成员名先穿匿名聚合展开）
				if (elem->desig_chain.size() >= 2)
				{
					std::vector<PhysStep> path;
					std::size_t dcur = 0;
					std::string tg = tag_for_struct(sty);
					auto d = tg.empty() ? struct_defs_.end() : struct_defs_.find(tg);
					if (d != struct_defs_.end()
						&& flatten_desig(sty, elem->desig_chain, dcur, path, 0)
						&& dcur == elem->desig_chain.size() && !path.empty() && !path[0].is_index
						&& path[0].idx < d->second->members.size())
					{
						llvm::Value *lp = addr;
						llvm::Type *lt = sty;
						CTypePtr lct = ct;
						if (path_leaf_addr(lp, lt, lct, path))
						{
							if (lt->isAggregateType() && elem->kind == ExprKind::InitList)
								emit_init_list_store(elem, lp, lt, lct);
							else
							{
								llvm::Value *v = gen_expr(elem);
								if (v->getType() != lt) v = convert_value(v, lt, get_expr_ctype(elem));
								builder.CreateStore(v, lp);
							}
							pos = path[0].idx + 1;
							std::vector<PhysStep> nx;
							cur_on = !lbf && phys_next_path(sty, path, nx); // P239：游标接在该路径之后
							ppath.swap(nx);
							continue;
						}
					}
				}
				std::vector<unsigned> cpath;
				// 指定初始化器名提升（P236）：`.提升名` 须下钻到匿名聚合内的真实字段；
				// 旧路径 find_struct_field_idx 只认直接成员、失配回退 pos=0 → 静默写进内层偏移 0。
				if (!elem->desig_field.empty()
					&& find_member_cpath(tag_for_struct(sty), elem->desig_field, cpath)
					&& cpath.size() > 1)
				{
					// 逐层下钻：struct 层按 LLVM 槽 GEP；union 层成员偏移恒 0、地址不变
					// （与 gen_member_addr 的链式寻址同口径）。
					llvm::Value *p = addr;
					llvm::StructType *cs = sty;
					std::string ctag = tag_for_struct(sty);
					for (std::size_t k = 0; k < cpath.size(); ++k)
					{
						if (!cs || ctag.empty()) break;
						auto dit = struct_defs_.find(ctag);
						if (dit == struct_defs_.end()) break;
						const StructDef *sd = dit->second;
						unsigned ci = cpath[k];
						if (ci >= sd->members.size()) break;
						if (!sd->is_union)
						{
							unsigned slot = static_cast<unsigned>(struct_member_ll_index(cs, ci));
							if (slot >= cs->getNumElements()) break;
							p = builder.CreateStructGEP(cs, p, slot);
						}
						if (k + 1 == cpath.size())
						{
							ptr = p;
							ft = to_llvm_alloca_type(sd->members[ci].type);
							break;
						}
						ctag = sd->members[ci].type ? sd->members[ci].type->tag : std::string();
						cs = ctag.empty() ? nullptr : get_struct_type(ctag);
					}
					pos = cpath[0] + 1;
					// P239：游标接在「匿名聚合全链之后」
					{
						std::vector<PhysStep> full;
						for (unsigned ci : cpath)
						{
							PhysStep s;
							s.is_index = false;
							s.idx = ci;
							full.push_back(s);
						}
						std::vector<PhysStep> nx;
						cur_on = !lbf && phys_next_path(sty, full, nx);
						ppath.swap(nx);
					}
				}
				else
				{
					std::size_t idx;
					// P242：含位域 struct —— 位置项按 C 成员序定位（auto-layout 槽序≠C 序号）、位域成员掩码写入。
					//   lbf 时 cur_on 恒 false（上方已禁游标），无需维护 P239 物理游标。
					if (lbf)
					{
						std::string btag = tag_for_struct(sty);
						auto bsd = btag.empty() ? struct_defs_.end() : struct_defs_.find(btag);
						const auto *bms = (bsd != struct_defs_.end()) ? &bsd->second->members : nullptr;
						std::size_t cmi = static_cast<std::size_t>(-1);
						if (!elem->desig_field.empty())
						{
							if (bms)
								for (std::size_t mi = 0; mi < bms->size(); ++mi)
									if ((*bms)[mi].name == elem->desig_field) { cmi = mi; break; }
						}
						else if (bms)
						{
							while (pos < bms->size() && (*bms)[pos].name.empty()) ++pos; // 匿名位域不占初始化位
							cmi = (pos < bms->size()) ? pos : static_cast<std::size_t>(-1);
						}
						if (bms && cmi < bms->size())
						{
							auto bf = bitfield_access_.find(btag + "." + (*bms)[cmi].name);
							if (bf != bitfield_access_.end())
							{
								store_bitfield_at(sty, addr, elem, bf->second);
								pos = cmi + 1;
								continue;
							}
							auto fi = field_index_map_.find(btag + "." + (*bms)[cmi].name);
							idx = (fi != field_index_map_.end()) ? fi->second : cmi;
							pos = cmi + 1;
						}
						else
						{
							idx = struct_member_ll_index(sty, pos);
							++pos;
						}
					}
					// 通过 .name 查找字段索引
					else if (!elem->desig_field.empty())
					{
						// 从 struct_defs_ 查成员名列表
						idx = find_struct_field_idx(sty, elem->desig_field, pos);
						pos = idx + 1;
						// P239：游标接在该成员之后（记 C 成员索引）
						std::vector<unsigned> dcp;
						if (find_member_cpath(tag_for_struct(sty), elem->desig_field, dcp) && !dcp.empty())
						{
							std::vector<PhysStep> full;
							for (unsigned ci : dcp)
							{
								PhysStep s;
								s.is_index = false;
								s.idx = ci;
								full.push_back(s);
							}
							std::vector<PhysStep> nx;
							cur_on = !lbf && phys_next_path(sty, full, nx);
							ppath.swap(nx);
						}
					}
					else
					{
						idx = struct_member_ll_index(sty, pos); // 手工布局: C 成员序→LLVM 索引
						++pos;
					}
					if (idx >= sty->getNumElements()) break;
					ptr = builder.CreateStructGEP(sty, addr, static_cast<unsigned>(idx));
					ft = sty->getElementType(static_cast<unsigned>(idx));
				}
				if (!ptr || !ft) break; // 成员名解析失败：放弃该分支（不误写偏移 0）
				// union 槽 + 括号列表：交 emit_init_list_store 的 ct==Union 分支（首成员可以是聚合，
				// 旧特判只取首项 gen_expr → 把 InitList 当值生成，指针成员全丢）。
				if (elem->kind == ExprKind::InitList)
				{
					if (CTypePtr mct = union_ctype_of(ft))
					{
						emit_init_list_store(elem, ptr, ft, mct);
						continue;
					}
				}
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
				if (ft->isAggregateType())
				{
					if (elem->kind == ExprKind::InitList)
					{
						emit_init_list_store(elem, ptr, ft);
						continue;
					}
					// 聚合成员 + 非括号项：按 brace elision 从当前游标递归消费（§6.7.9p20）。
					// 该项本身若是 struct/union 值则须整体拷贝，不参与 elision。
					CTypePtr et = get_expr_ctype(elem);
					bool agg_value = et && (et->kind == TypeKind::Struct || et->kind == TypeKind::Union);
					if (!agg_value)
					{
						std::size_t cur = ai;
						elide_store(ft, ptr, e->args, cur);
						if (cur > ai) ai = cur - 1;
						continue;
					}
				}
				{
					llvm::Value *v = gen_expr(elem);
					if (v->getType() != ft) v = convert_value(v, ft, get_expr_ctype(elem));
					builder.CreateStore(v, ptr);
				}
			}
		}
	}

	// 指定初始化器提升名 → 「仅目标字段非零」的嵌套常量（自内向外逐层包）。
	// struct 层填 ConstantStruct 对应槽；union 层交 union_body_const 摊成 [N x i8] 载荷
	// （LLVM 禁止聚合→字节数组的 bitcast，故不能直接叠进 union 类型）。
	// 返回类型 = tag 该层的 LLVM struct 类型；未知 tag / 越界 / 无法折算一律 nullptr。
	llvm::Constant *build_desig_const(const std::string &tag, const std::vector<unsigned> &cpath,
		std::size_t lvl, const ExprPtr &e)
	{
		if (tag.empty() || lvl >= cpath.size()) return nullptr;
		auto dit = struct_defs_.find(tag);
		if (dit == struct_defs_.end()) return nullptr;
		const StructDef *sd = dit->second;
		unsigned ci = cpath[lvl];
		if (ci >= sd->members.size()) return nullptr;
		const StructMember &m = sd->members[ci];
		llvm::Constant *val = (lvl + 1 == cpath.size())
			? fold_global_init(e, to_llvm_alloca_type(m.type))
			: build_desig_const(m.type ? m.type->tag : std::string(), cpath, lvl + 1, e);
		if (!val) return nullptr;
		llvm::StructType *sty = get_struct_type(tag);
		if (sd->is_union) return union_body_const(sty, val);
		std::vector<llvm::Constant *> elems(sty->getNumElements(), nullptr);
		for (std::size_t i = 0; i < sty->getNumElements(); ++i)
			elems[i] = llvm::Constant::getNullValue(sty->getElementType(i));
		unsigned slot = static_cast<unsigned>(struct_member_ll_index(sty, ci));
		if (slot >= sty->getNumElements()) return nullptr;
		elems[slot] = val;
		return llvm::ConstantStruct::get(sty, elems);
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
		// 当前块已被上一终止符结束（return/goto/break/continue 之后紧跟的不可达语句）：
		// 若直接续发，指令会被追加到含 terminator 的块里，LLVM verifier 报
		// "Terminator found in the middle of a basic block"（`{ return 1; x = 2; }`）。
		// 统一在此开一个延续块承接后续不可达语句（与既有 after.ret/after.break/after.goto
		// 死块约定一致）；Label/Case/Default 自带落点块、不向当前块发射，故排除。
		if (s->kind != StmtKind::Label && s->kind != StmtKind::Case && s->kind != StmtKind::Default)
		{
			llvm::BasicBlock *cur = builder.GetInsertBlock();
			if (cur && !cur->empty() && cur->back().isTerminator())
				builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "after.term", F));
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
			// 已终止块的延续由 gen_stmt 入口统一处理（见上方 after.term 护栏）。
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
			else if (in_synthesized_main_body_)
			{
				// P211：顶层裸 `return;` 无回返値 → 合成 main 以退出码 0 返回（i32 ret void 会 verifier 失败）
				builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0));
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
			// 但类型须登记到 var_types_：块内声明的函数（`struct t1 f21(void)`）在
			// resolve_expr_type 中按 var_type 查类型，缺失会让 (cond?f21:f22)() 落到 i32() 回退。
			if (s->var_type && s->var_type->kind == TypeKind::Function)
			{
				var_types_[s->var_name] = s->var_type;
				break;
			}
			// void 对象属不完整类型（parser 已报 error）：兜底不分配存储，避免 alloca void。
			if (!s->var_type || s->var_type->kind == TypeKind::Void)
				break;
			bool is_arr = (s->var_type && s->var_type->kind == TypeKind::Array);
			bool is_struct = (s->var_type && (s->var_type->kind == TypeKind::Struct || s->var_type->kind == TypeKind::Union));
			llvm::Type *ty = (is_arr || is_struct) ? to_llvm_alloca_type(s->var_type) : to_llvm_type(s->var_type);
			// VLA（P208/P209，C99 §6.7.2.1）：数组链含任一运行期维（外/中/内皆可）。
			// 旧路径 eval_const_int 把 `[len]` 折成 0 → `alloca [0 x double]` 零存储 → 写入即越界 AV
			// （CTS 0066/0106/0062/0090/0189 族 clang 可运行而 colang 崩）。
			// 多维线性化（P209）与 clang 同形：从内层剥「连续常量维」构成静态元素类型 S（遇首个
			// 运行期维停止），alloca S, count（count=外→该运行期维各维之积）：
			//   a[m][3]->alloca [3 x i32], m；b[3][m]->alloca i32, 3*m；a[m][n]->alloca i32, m*n。
			// static/extern 分支不适用（VLA 非法），保持旧 `[0 x elem]` 行为不变。
			bool is_vla = is_arr && !s->is_static && !s->is_extern &&
				type_has_vla_dim(s->var_type);
			llvm::Type *vla_elem_ty = nullptr;
			llvm::Value *vla_n = nullptr;
			if (is_vla)
			{
				// 各运行期维的尺寸在**声明点**求值：变量可能晚于函数入口才被赋值，且 VLA 作用域
				// 随块结束 → 不得参与下方 entry 提升（否则尺寸错且生命周期拉全函数）。
				std::vector<const CType *> dims;
				for (const CType *a = s->var_type.get(); a && a->kind == TypeKind::Array; a = a->base.get())
					dims.push_back(a);   // 外 -> 内
				int p = static_cast<int>(dims.size()) - 1;
				while (p >= 0 && !(dims[p]->is_vla && static_cast<bool>(dims[p]->vla_dim))) --p;   // 停在最内层运行期维
				// p>=0 恒成立（is_vla 蕴含至少一个运行期维）；dims[p]->base 之内层维全常量 → 可物化为 S。
				vla_elem_ty = to_llvm_alloca_type(dims[p]->base);
				vla_n = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 1);
				for (int i = 0; i <= p; ++i)
				{
					llvm::Value *di = (dims[i]->is_vla && static_cast<bool>(dims[i]->vla_dim))
						? to_index(gen_expr(dims[i]->vla_dim))
						: llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), dims[i]->array_size);
					vla_n = builder.CreateMul(vla_n, di);
				}
			}
			llvm::Value *addr = nullptr;
			// 块作用域 extern 声明：不分配存储，直接引用文件作用域对象（尚未声明则建外部声明）。
			if (s->is_extern)
			{
				auto git = globals_.find(s->var_name);
				if (git == globals_.end())
				{
					auto *gvar = new llvm::GlobalVariable(*module, ty, false,
						llvm::GlobalValue::ExternalLinkage, nullptr, s->var_name);
					globals_[s->var_name] = gvar;
					global_types_[s->var_name] = s->var_type;
					git = globals_.find(s->var_name);
				}
				vars_[s->var_name] = git->second;
				var_types_[s->var_name] = s->var_type;
				break;
			}
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
				if (is_vla)
				{
					// VLA 留在声明点：count 操作数必须支配使用点，提升 entry 会错化尺寸与生命周期。
					addr = builder.CreateAlloca(vla_elem_ty, vla_n, s->var_name.c_str());
					if (auto a = obj_align(s->var_type, vla_elem_ty, s->var_align))
						llvm::cast<llvm::AllocaInst>(addr)->setAlignment(*a);
				}
				else
				{
					llvm::BasicBlock &EB = F->getEntryBlock();
					builder.SetInsertPoint(&EB, EB.begin());
					addr = builder.CreateAlloca(ty, nullptr, s->var_name.c_str());
					if (auto a = obj_align(s->var_type, ty, s->var_align))
						llvm::cast<llvm::AllocaInst>(addr)->setAlignment(*a);
				}
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
				else if (is_vla && (s->var_init->kind == ExprKind::InitList ||
					s->var_init->kind == ExprKind::StringLiteral))
				{
					// VLA 初始化（§6.7.9p21：未列出的元素置零）：先按运行期总字节数 memset，
					// 再以 `[k x elem]` 视图逐项写入——addr 指向连续 n 个元素，视图只描述前 k 个，
					// 两者存储布局逐字相同（n < k 的输入属 UB，与 C 同口径）。
					llvm::Value *bytes = builder.CreateMul(vla_n,
						llvm::ConstantInt::get(vla_n->getType(),
							module->getDataLayout().getTypeAllocSize(vla_elem_ty)));
					builder.CreateMemSet(addr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0),
						bytes, llvm::MaybeAlign(1));
					if (s->var_init->kind == ExprKind::InitList)
						emit_init_list_store(s->var_init, addr,
							llvm::ArrayType::get(vla_elem_ty, s->var_init->args.size()), s->var_type);
					else if (llvm::Constant *c = fold_global_init(s->var_init,
						llvm::ArrayType::get(vla_elem_ty, co_decode_string(s->var_init->str_val).size() + 1)))
						builder.CreateStore(c, addr);
				}
				else if (s->var_init->kind == ExprKind::InitList && (is_arr || is_struct))
				{
					// 先零填 alloca（指定初始化器可能跳过某些位置）
					uint64_t sz = module->getDataLayout().getTypeAllocSize(ty);
					builder.CreateMemSet(addr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0), sz, llvm::MaybeAlign(1));
					emit_init_list_store(s->var_init, addr, ty, s->var_type);
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
					// 标量（含 enum/typedef 标量）的花括号初始化 `T x = {v}`：旧代码直接 gen_expr(InitList)，
					// 而 gen_expr 对 InitList 不产标量值→alloca 保持 0（CTS 0150 族 enum en={1}、int d={1} 全 0）。
					// 按 C §6.7.9p2/p16：标量以单层（可嵌套）花括号包裹的单个表达式初始化，取首元素递归解包；空 {} → 零初始化。
					// **仅限 LLVM 整数/浮点标量**：指针（复合字面量 (T){...} 赋值、数组退化）保持原 gen_expr 路径，否则 p41 会误取 args[0] 当标量存→解引用崩。
					bool scalar_ty = ty && (ty->isIntegerTy() || ty->isFloatingPointTy());
					ExprPtr init = s->var_init;
					if (scalar_ty)
						while (init && init->kind == ExprKind::InitList && !init->args.empty())
							init = init->args[0];
					// 解包后仍为 InitList ⇒ 必为空 {} ⇒ 零初始化；否则为普通表达式（或原本非 InitList）。
					bool is_empty_brace = scalar_ty && init && init->kind == ExprKind::InitList;
					llvm::Value *v = is_empty_brace ? llvm::Constant::getNullValue(ty) : gen_expr(init);
					if (v->getType() != ty)
						v = convert_value(v, ty, get_expr_ctype(is_empty_brace ? nullptr : init));
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

		// break 目标 = switch_end（C 语义：switch 内 break 跳出 switch）；
		// continue 不是迭代语句（§6.8.6.2p2），switch 帧须继承最近外层循环的 continue 目标，
		// 否则 switch 内的 continue 无处跳转，当前块会缺终止符触发 verifier 报错。
		llvm::BasicBlock *cont_target = nullptr;
		for (auto it = loop_stack_.rbegin(); it != loop_stack_.rend(); ++it)
		{
			if (it->continue_target)
			{
				cont_target = it->continue_target;
				break;
			}
		}
		loop_stack_.push_back({switch_end, cont_target});
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
		// 浮点非零判定：C §6.3.2.1p2「值不等于 0」用 fcmp（icmp 对浮点非法）。
		// 用 UNE（无序或不等）：NaN 视为真，与 C 真值语义一致。
		if (vt->isFloatingPointTy())
			return builder.CreateFCmpUNE(v, llvm::ConstantFP::get(vt, 0.0));
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
		{
			// i1（比较/布尔结果）转 FP 须按无符号 0/1：sitofp i1 true(有符号 1-bit = -1) 会得 -1.0，
			// 而 C 关系运算 `b==a` 产 int 0/1 再转 FP 应为 0.0/1.0（CTS 0114 long double 族）。
			if (from->getIntegerBitWidth() == 1) return builder.CreateUIToFP(v, target);
			// C §6.3.1.4：无符号整型转浮点须按值（uitofp）。源位宽相同但 signed/unsigned
			// 共用同一 LLVM 整型（i32 既可能 int 也可能 unsigned int），符号性只能取自 CType；
			// 缺判会把 `unsigned u=0x80000000; (double)u` 转成 -2147483648.0。
			if (src_t && is_unsigned_int_type(src_t)) return builder.CreateUIToFP(v, target);
			return builder.CreateSIToFP(v, target);
		}
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
			// 后缀定型：f/F 字面量是 float，须经 float 舍入（LLVM 按 FloatTy 构造 APFloat 时
			// 将 double 值就近舍到 float），否则 `float 变量 == f-字面量` 因字面量保留 double
			// 精度而不等（CTS 0048/0150 浮点后缀比较族）。无后缀/l/L → double。
			if (e->type && e->type->kind == TypeKind::Float)
				return llvm::ConstantFP::get(llvm::Type::getFloatTy(ctx), e->float_val);
			return llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx), e->float_val);
		case ExprKind::CharLiteral:
		{
			// int_val 已由 parser 解码为真实码值（跨层单一真值源）。带前缀者按 §6.4.4.4 在
			//   字面量自身宽度发射（L/u→i16、U→i32、u8/无前缀→i8）；旧实现恒发 i8，
			//   非 ASCII 宽字符值被截到首字节、宽度信息丢失。
			llvm::Type *cty = llvm::Type::getInt8Ty(ctx);
			if (e->type)
			{
				if (e->type->kind == TypeKind::WChar || e->type->kind == TypeKind::Char16)
					cty = llvm::Type::getInt16Ty(ctx);
				else if (e->type->kind == TypeKind::Char32)
					cty = llvm::Type::getInt32Ty(ctx);
			}
			return llvm::ConstantInt::get(cty, static_cast<uint64_t>(e->int_val));
		}
		case ExprKind::StringLiteral:
		{
			const int w = co_str_lit_width(e->str_val);
			if (w == 1)
			{
				std::string decoded = decode_string(e->str_val);
				return builder.CreateGlobalString(llvm::StringRef(decoded), ".str");
			}
			// 右值宽字符串字面量（L"…"/u"…"→i16、U"…"→i32）：按前缀三路降 UTF-16/UTF-32，
			// 物化为 private constant 全局数组再退化为首元素指针（复用 fold_global_init 同款解码器、
			// 同口径；C §6.4.5 前缀决定字面量元素类型）。旧路径恒发 i8、忽略前缀（§3.1 后置项）。
			llvm::Type *ety = (w == 2) ? static_cast<llvm::Type *>(llvm::Type::getInt16Ty(ctx))
			                           : static_cast<llvm::Type *>(llvm::Type::getInt32Ty(ctx));
			std::vector<llvm::Constant *> elems;
			if (w == 2)
			{
				const std::vector<uint16_t> u = co_decode_utf16(e->str_val);
				for (uint16_t c : u) elems.push_back(llvm::ConstantInt::get(ety, c));
			}
			else
			{
				const std::vector<uint32_t> u = co_decode_utf32(e->str_val);
				for (uint32_t c : u) elems.push_back(llvm::ConstantInt::get(ety, c));
			}
			elems.push_back(llvm::ConstantInt::get(ety, 0)); // 终止 null 码元
			llvm::ArrayType *aty = llvm::ArrayType::get(ety, elems.size());
			llvm::Constant *init = llvm::ConstantArray::get(aty, elems);
			auto *gv = new llvm::GlobalVariable(*module, aty, /*isConstant*/ true,
				llvm::GlobalValue::PrivateLinkage, init, ".str");
			gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
			llvm::Constant *z32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
			llvm::Constant *idxs[] = { z32, z32 };
			return llvm::ConstantExpr::getGetElementPtr(aty, gv, llvm::ArrayRef<llvm::Constant *>(idxs));
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
				// VLA（P208/P209）：存储形态 `alloca S, n`（S=静态元素后缀），当 S 为叶标量
				// （多维全运行期，如 a[m][n]/b[3][m]）时 getAllocatedType 返回标量而非数组
				// → 仅按 LLVM 类型判退化会 load 元素值再 inttoptr（非法 IR）。按「含运行期维」
				// 精确补这一类（形参的 Array-over-ptr 仍走下方旧路径，行为不变）。
				{
					auto vtit = var_types_.find(e->name);
					if (vtit != var_types_.end() && vtit->second &&
						vtit->second->kind == TypeKind::Array && type_has_vla_dim(vtit->second))
						return it->second;
				}
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
			// alloca/__builtin_alloca：POSIX/GNU 栈分配，按名 lowering 为 LLVM alloca 指令
			//   （符号不进入链接；对齐取 16 字节，满足 max_align_t）。
			if (!e->lhs && (e->name == "alloca" || e->name == "__builtin_alloca") && e->args.size() == 1)
			{
				llvm::Type *I64 = llvm::Type::getInt64Ty(ctx);
				llvm::Value *n = gen_expr(e->args[0]);
				if (n->getType() != I64 && n->getType()->isIntegerTy())
					n = n->getType()->getIntegerBitWidth() < 64 ? builder.CreateZExt(n, I64)
																: builder.CreateTrunc(n, I64);
				llvm::AllocaInst *ai = builder.CreateAlloca(llvm::Type::getInt8Ty(ctx), n, "alloca");
				ai->setAlignment(llvm::Align(16));
				return ai;
			}
			// math.h 内建（C §7.12 务实子集）：宿主 UCRT 不导出部分 ISO 符号（如 fabsf），
			// 且分类/关系函数（isnan/isgreater 等）在标准里本是宏；colang 按名 lowering 到
			// LLVM intrinsic / fcmp，避免链接期未解析符号。
			if (!e->lhs && !e->args.empty())
			{
				const std::string nm = e->name;
				const bool is_fabs = (nm == "fabs" || nm == "fabsf");
				const bool is_cls = (nm == "isnan" || nm == "isinf" || nm == "isfinite" || nm == "signbit");
				const bool is_rel = (nm == "isgreater" || nm == "isgreaterequal" || nm == "isless" ||
					nm == "islessequal" || nm == "islessgreater" || nm == "isunordered");
				const bool is_exp10 = (nm == "exp10" || nm == "exp10f");
				if (is_fabs || is_cls || is_rel || is_exp10)
				{
					llvm::Type *I32 = llvm::Type::getInt32Ty(ctx);
					llvm::Type *I64 = llvm::Type::getInt64Ty(ctx);
					llvm::Type *F32 = llvm::Type::getFloatTy(ctx);
					llvm::Type *F64 = llvm::Type::getDoubleTy(ctx);
					auto as_bool = [&](llvm::Value *b) -> llvm::Value * {
						return b->getType() == I32 ? b : builder.CreateZExt(b, I32);
					};
					auto to_fp = [&](llvm::Value *v, llvm::Type *want, const CTypePtr &src = nullptr) -> llvm::Value * {
						if (v->getType() == want) return v;
						if (v->getType()->isFloatingPointTy())
							return want->isFloatTy() ? builder.CreateFPTrunc(v, want) : builder.CreateFPExt(v, want);
						if (v->getType()->isIntegerTy())
						{
							// C §6.3.1.4：无符号/i1 源按值转（uitofp），否则大值变负数
							if (v->getType()->getIntegerBitWidth() == 1 ||
								(src && is_unsigned_int_type(src)))
								return builder.CreateUIToFP(v, want);
							return builder.CreateSIToFP(v, want);
						}
						return v;
					};
					if (is_fabs)
					{
						llvm::Type *want = (nm == "fabsf") ? F32 : F64;
						llvm::Value *a = to_fp(gen_expr(e->args[0]), want, get_expr_ctype(e->args[0]));
						if (!a->getType()->isFloatingPointTy()) return a;
						return builder.CreateIntrinsic(want, llvm::Intrinsic::fabs, {a});
					}
					if (is_exp10)
					{
						bool f32 = (nm == "exp10f");
						llvm::Type *want = f32 ? F32 : F64;
						llvm::Value *a = to_fp(gen_expr(e->args[0]), want, get_expr_ctype(e->args[0]));
						llvm::FunctionCallee pc = module->getOrInsertFunction(f32 ? "powf" : "pow",
							llvm::FunctionType::get(want, {want, want}, false));
						llvm::Value *ten = llvm::ConstantFP::get(want, 10.0);
						return builder.CreateCall(pc, {ten, a});
					}
					// 分类/关系：按 double 求值（float 实参安全提升，语义等价）
					llvm::Value *a = to_fp(gen_expr(e->args[0]), F64, get_expr_ctype(e->args[0]));
					if (!a->getType()->isFloatingPointTy()) return as_bool(llvm::ConstantInt::getFalse(ctx));
					auto is_inf = [&]() -> llvm::Value * {
						return builder.CreateFCmpOEQ(
							builder.CreateIntrinsic(F64, llvm::Intrinsic::fabs, {a}),
							llvm::ConstantFP::getInfinity(F64));
					};
					if (nm == "isnan") return as_bool(builder.CreateFCmpUNO(a, a));
					if (nm == "isinf") return as_bool(is_inf());
					if (nm == "isfinite")
						return as_bool(builder.CreateAnd(
							builder.CreateNot(builder.CreateFCmpUNO(a, a)),
							builder.CreateNot(is_inf())));
					if (nm == "signbit")
					{
						llvm::Value *bits = builder.CreateBitCast(a, I64);
						return builder.CreateTrunc(builder.CreateLShr(bits, 63), I32);
					}
					if (e->args.size() < 2) return as_bool(llvm::ConstantInt::getFalse(ctx));
					llvm::Value *b = to_fp(gen_expr(e->args[1]), F64, get_expr_ctype(e->args[1]));
					if (!b->getType()->isFloatingPointTy()) return as_bool(llvm::ConstantInt::getFalse(ctx));
					if (nm == "isgreater") return as_bool(builder.CreateFCmpOGT(a, b));
					if (nm == "isgreaterequal") return as_bool(builder.CreateFCmpOGE(a, b));
					if (nm == "isless") return as_bool(builder.CreateFCmpOLT(a, b));
					if (nm == "islessequal") return as_bool(builder.CreateFCmpOLE(a, b));
					if (nm == "islessgreater") return as_bool(builder.CreateFCmpONE(a, b));
					return as_bool(builder.CreateFCmpUNO(a, b)); /* isunordered */
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
					llvm::FunctionType *ft = callee->getFunctionType();
					coerce_call_args(ft, args, arg_cts);
					// 无原型（`int f();`）声明与带形参定义并存、或 K&R 式 `void out()` 定义被带实参调用：
					//   声明处 LLVM 函数类型与实参数量/类型对不上，直接 CreateCall 会触发 verifier
					//   「Incorrect number of arguments / Call parameter type does not match」。
					//   C 对这种「未知形参」调用按 §6.5.2.2p6 做默认实参提升，故以实参实际签名单建
					//   FunctionType 调用（opaque pointer 下 callee 本身即 ptr，无需 bitcast）。
					bool sig_ok = ft->isVarArg() ? (args.size() >= ft->getNumParams())
												 : (args.size() == ft->getNumParams());
					if (sig_ok)
					{
						for (size_t i = 0; i < ft->getNumParams(); ++i)
						{
							if (ft->getParamType(i) != args[i]->getType())
							{
								sig_ok = false;
								break;
							}
						}
					}
					if (!sig_ok)
					{
						for (size_t i = 0; i < args.size(); ++i)
							args[i] = default_arg_promote(args[i], i < arg_cts.size() ? arg_cts[i] : nullptr);
						std::vector<llvm::Type *> atys;
						for (auto *a : args) atys.push_back(a->getType());
						llvm::FunctionType *cft = llvm::FunctionType::get(ft->getReturnType(), atys, false);
						return builder.CreateCall(cft, callee, args);
					}
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
				// 提取位域 [bit_offset, bit_offset+bit_width)：先左移把域顶位送到存储单元最高位（顺带丢弃更高位、低位
				// 补 0），再右移对齐到底——有符号域用算术右移(AShr)从 bit_width-1 位恢复符号、无符号域用逻辑右移(LShr)
				// 零扩展。（旧法 LShr+掩码后仅从存储单元顶位 SExt，窄有符号域符号位落错位：signed char b:1 存 1 回读得
				//   +1 而非 -1、signed int c:1（i32 单元、不触发 <32 提升）直接返回掩码值 1；配合布局处 is_signed 判定补全
				//   Char/Long/LongLong，CTS 0119 BSTORE 族。）
				unsigned stor_bits = stor_ty->getIntegerBitWidth();
				unsigned lshift = stor_bits - ba.bit_offset - ba.bit_width; // 域顶位 → 存储单元顶位
				unsigned rshift = stor_bits - ba.bit_width;                 // 域 → 存储单元底位
				if (lshift > 0)
					sv = builder.CreateShl(sv, llvm::ConstantInt::get(stor_ty, lshift));
				sv = ba.is_signed ? builder.CreateAShr(sv, llvm::ConstantInt::get(stor_ty, rshift))
								  : builder.CreateLShr(sv, llvm::ConstantInt::get(stor_ty, rshift));
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
			emit_init_list_store(e, addr, ty, e->type);
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
			llvm::Value *ptr = nullptr;
			llvm::Value *idx = nullptr;
			CTypePtr lhs_t = get_expr_ctype(e->lhs);
			CTypePtr rhs_t = get_expr_ctype(e->rhs);
			bool lhs_ptr = lhs_t && (lhs_t->kind == TypeKind::Array || lhs_t->kind == TypeKind::Pointer);
			bool rhs_ptr = rhs_t && (rhs_t->kind == TypeKind::Array || rhs_t->kind == TypeKind::Pointer);
			if (!lhs_ptr && rhs_ptr)
			{
				// C §6.5.2.1p2：E1[E2] 等价于 *((E1)+(E2))，整数写在前面（`4[intb]`、`b[a]`）
				// 也合法。旧实现一律把 lhs 当指针 → GEP/SExt 作用在 i32 上
				// （CTS 0048/0053/0151 族 "SExt only operates on integer"）。
				ptr = gen_expr(e->rhs);
				idx = gen_expr(e->lhs);
				lhs_t = rhs_t;
			}
			else
			{
				ptr = gen_expr(e->lhs);
				idx = gen_expr(e->rhs);
			}
			idx = to_index(idx);
			// 多维 VLA / VLA 形参（P209）：行元素含运行期维→叶类型+运行期跨距线性寻址。
			bool rt_row = subscript_row_is_runtime(lhs_t);
			llvm::Value *addr = gen_subscript_addr(lhs_t, ptr, idx);
			if (rt_row)
				return addr;   // 行指针（数组退化为指向首元素）
			// 数组/结构体元素不 load，返回地址（数组退化为指针）
			llvm::Type *elem_ty = llvm::Type::getInt32Ty(ctx);
			if (lhs_t && (lhs_t->kind == TypeKind::Array || lhs_t->kind == TypeKind::Pointer))
				elem_ty = to_llvm_alloca_type(lhs_t->base);
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
			llvm::Value *l = to_fp(l_raw, dty, get_expr_ctype(e->lhs));
			llvm::Value *r = to_fp(r_raw, dty, get_expr_ctype(e->rhs));
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
			// 数组操作数先按 §6.5.3.2p3 衰减为「指向首元素的指针」再解引用：
			//   *arr 即首元素左值。元素仍是数组（多维）或 struct/union 时按地址建模，
			//   直接返回指针；元素为标量时须 load（否则 *v 得到地址而非 v[0]）。
			if (pt && pt->kind == TypeKind::Array)
			{
				CTypePtr et = pt->base;
				if (et && (et->kind == TypeKind::Array || et->kind == TypeKind::Struct || et->kind == TypeKind::Union))
					return ptr;
				llvm::Type *load_ty = et ? to_llvm_alloca_type(et) : llvm::Type::getInt32Ty(ctx);
				return builder.CreateLoad(load_ty, ptr);
			}
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
				CTypePtr et = sizeof_operand_ctype(e->lhs);
				// VLA 的 sizeof 是运行期表达式（§6.5.3.4p2：结果为元素个数乘元素大小）。
				// 旧路径 to_llvm_alloca_type 得 [0 x elem] → 返 0。
				if (et && et->kind == TypeKind::Array && type_has_vla_dim(et))
				{
					// sizeof(VLA) 运行期求值（§6.5.3.4p2）= 叶元素个数 × 叶存储大小；
					// 多维 VLA（P209）各维之积皆计入。
					llvm::Value *cnt = vla_leaf_count(et);
					llvm::Value *bytes = builder.CreateMul(cnt,
						llvm::ConstantInt::get(cnt->getType(),
							DL.getTypeStoreSize(vla_leaf_llvm_type(et))));
					return builder.CreateTrunc(bytes, llvm::Type::getInt32Ty(ctx));
				}
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
			llvm::Value *pold;   // 指针分支：自增前旧地址
			llvm::Value *pnv;    // 指针分支：自增后新地址
			if (is_ptr)
			{
				// 指针自增: load ptr → GEP ±1
				llvm::Type *ptr_ty = llvm::PointerType::getUnqual(ctx);
				pold = builder.CreateLoad(ptr_ty, addr);
				llvm::Value *one = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1);
				if (e->op == "--")
					one = builder.CreateSub(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), one);
				llvm::Type *ety = ct->base ? to_llvm_alloca_type(ct->base) : llvm::Type::getInt32Ty(ctx);
				pnv = builder.CreateGEP(ety, pold, to_index(one));
				builder.CreateStore(pnv, addr);
			}
			else
			{
				// 按左值**实际内存类型** load/store：此前一律 i32，对 char(i8)/short(i16)
				// 越界读写、对 long long(i64) 欠读、对 float/double 误当整数加——副作用虽
				// 写回，但取回的返回值是垃圾（CTS 0053 `++/--` char 族：i=--(a) 得 2304）。
				llvm::Type *mem_ty = ct ? to_llvm_alloca_type(ct) : llvm::Type::getInt32Ty(ctx);
				if (mem_ty->isFloatingPointTy())
				{
					llvm::Value *oldf = builder.CreateLoad(mem_ty, addr);
					llvm::Value *onef = llvm::ConstantFP::get(mem_ty, 1.0);
					llvm::Value *nvf = (e->op == "++") ? builder.CreateFAdd(oldf, onef)
													  : builder.CreateFSub(oldf, onef);
					builder.CreateStore(nvf, addr);
					return e->postfix ? oldf : nvf;
				}
				IntPromo p = int_promo_of(ct);
				unsigned tgt = p.bits < 32 ? 32u : p.bits;   // C 整型提升：<int→int，long long 保 64
				bool uns = is_unsigned_int_type(ct);         // 扩展按**原类型**符号性（unsigned char 零扩展）
				llvm::Value *raw = builder.CreateLoad(mem_ty, addr);
				llvm::Value *old = widen_int(raw, tgt, uns);  // 提升后的旧值（后缀返回）
				llvm::Value *one = llvm::ConstantInt::get(old->getType(), 1);
				llvm::Value *nvar = (e->op == "++") ? builder.CreateAdd(old, one)
													: builder.CreateSub(old, one);
				llvm::Value *to_store = (nvar->getType() == mem_ty) ? nvar
																: builder.CreateTrunc(nvar, mem_ty);
				builder.CreateStore(to_store, addr);
				// 前缀返回=存回值再提升（反映溢出回绕，如 signed char 127→-128）
				llvm::Value *nvp = (to_store->getType()->getIntegerBitWidth() == tgt)
					? to_store : widen_int(to_store, tgt, uns);
				return e->postfix ? old : nvp;
			}
			return e->postfix ? pold : pnv;
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
		if (e->op == "!")
		{
			llvm::Value *c = gen_cond(e->lhs);
			return builder.CreateXor(c, llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), 1));
		}
		if (e->op == "~")
		{
			// 一元 ~ 按操作数**实际整数类型**取反（§6.3.1.1 整型提升）：窄整型提升到 int（i32），
			// long/long long 保持其位宽（i64）。此前一律 to_i32 把 64 位操作数截到 i32 取反，
			// 且赋值回 i64 时符号扩展，致 ~(long long)0x8000000000000000 得 0xffffffffffffffff
			// （应 0x7fffffffffffffff，CTS 0132）。
			llvm::Value *raw = gen_expr(e->lhs);
			CTypePtr ot = get_expr_ctype(e->lhs);
			IntPromo p = int_promo_of(ot);
			unsigned tgt = p.bits < 32 ? 32u : p.bits;
			llvm::Value *iv = widen_int(raw, tgt, is_unsigned_int_type(ot));
			return builder.CreateXor(iv, llvm::ConstantInt::get(iv->getType(), static_cast<uint64_t>(-1), true));
		}
		llvm::Value *v = to_i32(gen_expr(e->lhs));
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
				{
					rhs_val = to_i32(rhs_val);
				}
				// 归一 到存储单元宽度：不能一律降 i32——i64 存储单元（signed long long b:7）会
				// 走到 trunc i32→i64（DestTy too big，CTS 0093/0095/0117/0129/0137 族）。
				if (rhs_val->getType() != stor_ty)
					rhs_val = convert_value(rhs_val, stor_ty);
				// 截断 rhs 到位域宽度（掩码在存储单元宽度上做）
				if (ba.bit_width < ba.unit_bits)
				{
					uint64_t mask = (ba.bit_width >= 64) ? ~0ULL : ((1ULL << ba.bit_width) - 1);
					rhs_val = builder.CreateAnd(rhs_val,
						llvm::ConstantInt::get(stor_ty, mask));
				}
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
				else if (ld_ty->isFloatingPointTy() || rhs->getType()->isFloatingPointTy())
				{
					// 浮点复合赋值：按 double 运算（与 gen_binary 一致），右侧 double 操作数不再被误截到 float；
					// 左值为整型而右值为浮点（C §6.3.2.8 常用算术转换）同样走此路：整型先转 double，
					// 存回由函数尾部 convert_value(rhs, stor_ty) 截回左值真实存储类型（修 IR 校验「add i32, double」）。
					llvm::Type *dty = llvm::Type::getDoubleTy(ctx);
					llvm::Value *a = to_fp(lhs_val, dty, lhs_ct);
					llvm::Value *b = to_fp(rhs, dty, get_expr_ctype(e->rhs));
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
		else if (stor_ty && stor_ty->isIntegerTy())
			// 整型目标（含普通 int/i32）统一走 convert_value：float/double→int 才能正确 fptosi。
			// 旧法对 i32 目标落 `else to_i32`，而 to_i32 不识别浮点、原样返回，随后 store 把浮点值
			// **裸位重interpret** 进整型槽（float 变量→其 IEEE 位如 5.0f→1084227584；double 表达式→8 字节
			// 覆盖 4 字节槽、取低 32 位常为 0），致 `int ti = i*2.0f` 得 0（CTS 0011 float 运算矩阵族）。
			// convert_value 对 int→int 与 to_i32 结果等价（from==target 直接返回），仅补上 float→int 的 FPToSI。
			rhs = convert_value(rhs, stor_ty, get_expr_ctype(e->rhs));
		else
			rhs = to_i32(rhs); // stor_ty 不可用时的兜底（正常整型赋值已由上一分支含 float→int 处理）
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
		// 数组下标: arr[idx] -> GEP（E1[E2] ≡ *((E1)+(E2))，整数写在前面同样合法）
		if (e->kind == ExprKind::Binary && e->op == "[]")
		{
			CTypePtr lhs_t = get_expr_ctype(e->lhs);
			CTypePtr rhs_t = get_expr_ctype(e->rhs);
			bool lhs_ptr = lhs_t && (lhs_t->kind == TypeKind::Array || lhs_t->kind == TypeKind::Pointer);
			bool rhs_ptr = rhs_t && (rhs_t->kind == TypeKind::Array || rhs_t->kind == TypeKind::Pointer);
			llvm::Value *ptr = nullptr;
			llvm::Value *idx = nullptr;
			if (!lhs_ptr && rhs_ptr)
			{
				ptr = gen_expr(e->rhs);
				idx = to_index(gen_expr(e->lhs));
				lhs_t = rhs_t;
			}
			else
			{
				ptr = gen_expr(e->lhs);
				idx = to_index(gen_expr(e->rhs));
			}
			// 下标取址：元素类型 / 运行期行跨距（多维 VLA，P209）由 gen_subscript_addr 统一处理；
			// 本路径始终返回地址（不 load），load 与否由调用方（gen_expr）决定。
			return gen_subscript_addr(lhs_t, ptr, idx);
		}
		// 成员访问: obj.field / ptr->field
		if (e->kind == ExprKind::Member)
			return gen_member_addr(e);
		// _Generic 左值：取命中关联项表达式的地址（C11 DR 4xx：_Generic 结果可为左值）
		if (e->kind == ExprKind::Generic)
		{
			const ExprPtr *assoc = generic_select(e);
			if (assoc && (*assoc)->rhs)
				return gen_lvalue_addr((*assoc)->rhs);
			return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
		}
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
			// 带前缀字符常量按其前缀类型参与 _Generic 匹配（§6.4.4.4，控制表达式不做整型提升）：
			//   _Generic(L'A', wchar_t: …, int: …) 应命中 wchar_t。
			if (e->type && (e->type->kind == TypeKind::LongLong || e->type->kind == TypeKind::Long ||
				e->type->kind == TypeKind::Int || e->type->kind == TypeKind::BitInt ||
				e->type->kind == TypeKind::WChar || e->type->kind == TypeKind::Char8 ||
				e->type->kind == TypeKind::Char16 || e->type->kind == TypeKind::Char32))
				return e->type;
			return make_type(TypeKind::Int);
		case ExprKind::FloatLiteral:
			if (e->type && e->type->kind == TypeKind::Float) return e->type;
			return make_type(TypeKind::Double);
		case ExprKind::StringLiteral:
			// 前缀决定字面量元素类型（C §6.4.5）：L/u→wchar_t*(i16)、U→char32_t*(i32)、其余→char*。
			// 与 gen_expr 右值三路编码发射同口径（均据 co_str_lit_width）；使 wchar_t *p = L"…" 与
			// MessageBoxW(L"…", …) 实参在 Sema 通过。旧行为恒报 char*（与右值发射不一致）。
		{
			const int w = co_str_lit_width(e->str_val);
			if (w == 2) return make_pointer(make_type(TypeKind::WChar));
			if (w == 4) return make_pointer(make_type(TypeKind::Char32));
			return make_pointer(make_type(TypeKind::Char)); // char[N] 经 lvalue → char*
		}
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

	// sizeof 操作数的类型：字符串字面量须按**数组对象**计（§6.3.2.1：sizeof 是不发生数组→指针
	//   退化的语境），而 get_expr_ctype 按左值转换后的元素指针建模 → 直接用它会把 sizeof("abc") 错得 8。
	//   其余表达式一律沿用 get_expr_ctype（变量/成员/下标的数组类型本就不退化）。
	CTypePtr sizeof_operand_ctype(const ExprPtr &e)
	{
		if (e && e->kind == ExprKind::StringLiteral) return ctype_of_string_literal(e->str_val);
		return get_expr_ctype(e);
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
		// 自 base_tag 到字段声明处的逐级「(所在 tag, 成员序号)」链，首项即一级成员：
		// 直命名成员链长 1；穿过匿名聚合时逐级追加（末项即 decl_tag 一级）。
		// 用于逐级下钻定位——union 级偏移恒 0、struct 级按其布局取槽偏移。
		std::vector<std::pair<std::string, unsigned>> chain;
	};
	// 递归定位字段：一级命中的命名成员 → slot=其索引；匿名聚合包裹→ slot=该匿名成员索引，
	// 并在 chain 中记录逐级路径（union 级无偏移、struct 级偏移非 0：匿名 struct 嵌入 union 时
	// 其第二字段偏移不能丢弃，旧「一次 GEP 到槽」的假设只对匿名 union 嵌套成立）。
	bool locate_member(const std::string &tag, const std::string &field, int depth,
	                   unsigned &slot, std::string &decl_tag, unsigned &decl_idx,
	                   std::vector<std::pair<std::string, unsigned>> &chain)
	{
		if (depth > 16) return false;
		auto sd = struct_defs_.find(tag);
		if (sd == struct_defs_.end()) return false;
		const auto &mem = sd->second->members;
		// 一级：直接命名成员
		for (unsigned i = 0; i < mem.size(); ++i)
			if (!mem[i].name.empty() && mem[i].name == field) {
				if (depth == 0) slot = i;
				decl_tag = tag; decl_idx = i;
				chain.push_back({tag, i});
				return true;
			}
		// 二级：穿过匿名 struct/union（union 保留嵌套，struct 已由 parser 平铺）
		for (unsigned i = 0; i < mem.size(); ++i)
		{
			if (mem[i].name.empty() && mem[i].type
				&& (mem[i].type->kind == TypeKind::Struct || mem[i].type->kind == TypeKind::Union)
				&& !mem[i].type->tag.empty())
			{
				unsigned sub_slot = 0; std::string dt; unsigned di = 0;
				if (locate_member(mem[i].type->tag, field, depth + 1, sub_slot, dt, di, chain)) {
					if (depth == 0) slot = i;
					decl_tag = dt; decl_idx = di;
					chain.insert(chain.begin(), {tag, i});
					return true;
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
		if (locate_member(tag, e->name, 0, slot, mi.decl_tag, mi.decl_idx, mi.chain))
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

	// 成员相对基址的静态字节偏移：按 chain 逐级累加（union 级 0、struct 级取槽偏移）。
	// 位域成员（offsetof 本身 UB）或未知 tag 返回 false，交调用方拒折。
	bool member_static_offset(const MemberInfo &mi, const std::string &name, intmax_t &off)
	{
		const llvm::DataLayout &DL = module->getDataLayout();
		off = 0;
		if (mi.chain.empty()) return true;   // 未命中：维持旧默认（一级偏移 0）
		for (std::size_t k = 0; k < mi.chain.size(); ++k)
		{
			const std::string &ctag = mi.chain[k].first;
			auto sd_it = struct_defs_.find(ctag);
			if (sd_it == struct_defs_.end()) return false;
			if (sd_it->second->is_union) continue;   // union 成员偏移恒 0
			llvm::StructType *st = get_struct_type(ctag);
			if (!st) return false;
			unsigned field_idx = mi.chain[k].second;
			if (k + 1 == mi.chain.size())
			{
				// 末级：位域用存储单元索引、普通成员查 field_index_map_
				std::string key = ctag + "." + name;
				if (bitfield_access_.find(key) != bitfield_access_.end()) return false;   // 位域拒折
				auto fi = field_index_map_.find(key);
				if (fi != field_index_map_.end()) field_idx = fi->second;
			}
			else
				field_idx = static_cast<unsigned>(struct_member_ll_index(st, field_idx));
			if (field_idx >= st->getNumElements()) return false;
			off += static_cast<intmax_t>(DL.getStructLayout(st)->getElementOffset(field_idx).getFixedValue());
		}
		return true;
	}

	// 生成成员地址 GEP：按 chain 逐级下钻（struct 级 GEP 到槽、union 级不产生指令）。
	// 旧实现对 union 基址一律返回基址，仅「匿名 union 嵌套」时正确；匿名 struct 嵌入 union
	// 时其非首字段偏移被丢弃（LARGE_INTEGER.HighPart 读到 LowPart），故按链逐级走。
	llvm::Value *gen_member_addr(const ExprPtr &e)
	{
		MemberInfo mi = resolve_member_info(e);
		llvm::Value *addr = (e->op == ".") ? gen_lvalue_addr(e->lhs) : gen_expr(e->lhs);
		if (mi.chain.empty())
		{
			// 未命中：维持旧默认（struct 按 0 号槽 GEP、union 直接基址）
			auto sd_it = struct_defs_.find(mi.base_tag);
			if (sd_it != struct_defs_.end() && sd_it->second->is_union)
				return addr;
			llvm::StructType *st = get_struct_type(mi.base_tag);
			if (!st) return addr;
			return builder.CreateStructGEP(st, addr, 0, e->name.c_str());
		}
		for (std::size_t k = 0; k < mi.chain.size(); ++k)
		{
			const std::string &ctag = mi.chain[k].first;
			auto sd_it = struct_defs_.find(ctag);
			if (sd_it != struct_defs_.end() && sd_it->second->is_union) continue;   // union 成员偏移恒 0
			llvm::StructType *st = get_struct_type(ctag);   // 确保布局已建立
			unsigned field_idx = mi.chain[k].second;        // fallback = source order
			bool last = (k + 1 == mi.chain.size());
			if (last)
			{
				// 末级：位域用存储单元索引、普通成员查 field_index_map_
				std::string key = ctag + "." + e->name;
				auto bf_it = bitfield_access_.find(key);
				if (bf_it != bitfield_access_.end())
					field_idx = bf_it->second.storage_idx;
				else
				{
					auto fi_it = field_index_map_.find(key);
					if (fi_it != field_index_map_.end())
						field_idx = fi_it->second;
				}
			}
			else
				field_idx = static_cast<unsigned>(struct_member_ll_index(st, field_idx));   // 中间匿名聚合成员
			addr = builder.CreateStructGEP(st, addr, field_idx, last ? e->name.c_str() : "");
		}
		return addr;
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
		// 结果类型：任一臂为 void → 整体 void；否则按 C §6.5.15 统一：
		//   同型即用；任一臂为浮点 → 取 double（整型臂 SIToFP 提升）；其余异型回退 i32（沿用旧策略）
		llvm::Type *rt = nullptr;
		if (!tvoid && !evoid)
		{
			llvm::Type *tt = tv->getType();
			llvm::Type *et = ev->getType();
			if (tt == et)
				rt = tt;
			else if (tt->isFloatingPointTy() || et->isFloatingPointTy())
				rt = llvm::Type::getDoubleTy(ctx);
			else
				rt = llvm::Type::getInt32Ty(ctx);
		}

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
		{
			// 同 convert_value：i1 按无符号 0/1 转 FP，避免 sitofp i1 true = -1.0；
			// 无符号源按值转（uitofp），否则 `(double)0x80000000u` 得负数（C §6.3.1.4）。
			if (val->getType()->getIntegerBitWidth() == 1) return builder.CreateUIToFP(val, target);
			if (is_unsigned_int_type(get_expr_ctype(e->lhs))) return builder.CreateUIToFP(val, target);
			return builder.CreateSIToFP(val, target);
		}
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
	// 提升为浮点类型（double）。src_t 为源表达式 CType（可空），仅用于判定无符号性
	// （同 convert_value：i32 既可能 int 也可能 unsigned int，只能取自 CType）。
	llvm::Value *to_fp(llvm::Value *v, llvm::Type *fp_ty, const CTypePtr &src_t = nullptr)
	{
		llvm::Type *t = v->getType();
		if (t == fp_ty) return v;
		if (t == llvm::Type::getFloatTy(ctx)) return builder.CreateFPExt(v, fp_ty);
		if (t->isDoubleTy()) return builder.CreateFPTrunc(v, fp_ty); // float→double 不会到这里
		if (t == llvm::Type::getInt1Ty(ctx)) return builder.CreateZExt(v, llvm::Type::getInt32Ty(ctx)), builder.CreateUIToFP(builder.CreateZExt(v, llvm::Type::getInt32Ty(ctx)), fp_ty);
		if (t->isIntegerTy())
		{
			// C §6.3.1.4：无符号源按值转（uitofp），否则大值变负数
			if (src_t && is_unsigned_int_type(src_t)) return builder.CreateUIToFP(v, fp_ty);
			return builder.CreateSIToFP(v, fp_ty);
		}
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
		// i1（&&/||/! 等逻辑/关系运算结果，C 里是 int 0/1）须零扩展：
		// sext(i1 true)= -1 会令 arr[1&&1] 变 arr[-1] 越界（CTS 0150 下标族）。
		if (v->getType() == llvm::Type::getInt1Ty(ctx))
			return builder.CreateZExt(v, llvm::Type::getInt64Ty(ctx));
		return builder.CreateSExt(v, llvm::Type::getInt64Ty(ctx));
	}

	// —— VLA 多维支持（P209）——
	// 数组类型链是否含任一运行期维（外/中/内任一维为 VLA 即真）。
	bool type_has_vla_dim(const CTypePtr &t)
	{
		for (const CType *a = t.get(); a && a->kind == TypeKind::Array; a = a->base.get())
			if (a->is_vla && static_cast<bool>(a->vla_dim)) return true;
		return false;
	}
	// 数组链底部叶类型（剥尽所有 Array 维后的类型）之 alloca LLVM 形态。
	llvm::Type *vla_leaf_llvm_type(const CTypePtr &t)
	{
		CTypePtr a = t;
		while (a && a->kind == TypeKind::Array) a = a->base;
		return a ? to_llvm_alloca_type(a) : llvm::Type::getInt32Ty(ctx);
	}
	// 一个数组值所含叶元素个数（各维之积：常量维=array_size、运行期维=gen_expr(vla_dim)）；非数组→1。
	llvm::Value *vla_leaf_count(const CTypePtr &t)
	{
		if (!t || t->kind != TypeKind::Array)
			return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 1);
		llvm::Value *d = (t->is_vla && static_cast<bool>(t->vla_dim))
			? to_index(gen_expr(t->vla_dim))
			: llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), t->array_size);
		return builder.CreateMul(d, vla_leaf_count(t->base));
	}
	// 被下标对象（数组，或 VLA 形参调整后「指向运行期数组的指针」）的
	//   元素（行）是否含运行期维——需线性寻址（运行期行跨距）。
	bool subscript_row_is_runtime(const CTypePtr &lhs_t)
	{
		return lhs_t && (lhs_t->kind == TypeKind::Array || lhs_t->kind == TypeKind::Pointer) &&
			lhs_t->base && lhs_t->base->kind == TypeKind::Array && type_has_vla_dim(lhs_t->base);
	}
	// 下标取址：常量元素走静态 GEP；元素含运行期维（多维 VLA 的行、或 VLA 形参的行）则线性化，
	//   addr = GEP(leafTy, ptr, idx * 行叶元素数)——与 clang `mul + getelementptr <leaf>` 同形。
	llvm::Value *gen_subscript_addr(const CTypePtr &lhs_t, llvm::Value *ptr, llvm::Value *idx)
	{
		if (subscript_row_is_runtime(lhs_t))
		{
			llvm::Type *leaf = vla_leaf_llvm_type(lhs_t->base);
			llvm::Value *cnt = vla_leaf_count(lhs_t->base);
			return builder.CreateGEP(leaf, ptr, builder.CreateMul(idx, cnt));
		}
		llvm::Type *elem_ty = llvm::Type::getInt32Ty(ctx);
		if (lhs_t && (lhs_t->kind == TypeKind::Array || lhs_t->kind == TypeKind::Pointer))
			elem_ty = to_llvm_alloca_type(lhs_t->base);
		return builder.CreateGEP(elem_ty, ptr, idx);
	}
};

}  // namespace

// ============================================================
// 对外接口
// ============================================================

// 用 LLVM 23 新 PM 把模块下 Lower 成 COFF .obj（对标 llc NewPMDriver）。
// 关键点（Phase 0 spike 实测）：
//   - 模块 DataLayout 必须与目标机一致，否则 buildCodeGenPipeline fatal-error；
//     故此处用 TM->createDataLayout() 覆写模块 DL（与 clang 喂 .bc 时行为一致）。
//   - X86 TargetMachine 构造器由 LLVMInitializeX86Target() 注册（非 ...TargetMC）。
static bool emit_object_file(llvm::Module *M, const std::string &obj_path,
                             const std::string &src, std::vector<Error> &errors)
{
	auto fail = [&](const std::string &msg) -> bool {
		errors.push_back(Error{src, Pos{1, 1}, msg, false});
		return false;
	};

	static bool target_inited = false;
	if (!target_inited)
	{
		// LLVMInitializeX86* 由 TargetSelect.h 声明于全局 extern "C"（非 llvm 命名空间）。
		LLVMInitializeX86Target();
		LLVMInitializeX86TargetInfo();
		LLVMInitializeX86TargetMC();
		LLVMInitializeX86AsmPrinter();
		LLVMInitializeX86AsmParser();
		target_inited = true;
	}

	const llvm::Triple TT(M->getTargetTriple());
	std::string lookup_err;
	const llvm::Target *TheTarget = llvm::TargetRegistry::lookupTarget(TT, lookup_err);
	if (!TheTarget)
		return fail("target lookup: " + lookup_err);

	llvm::TargetOptions Options;
	std::unique_ptr<llvm::TargetMachine> TM(TheTarget->createTargetMachine(
		TT, "x86-64", "", Options, std::nullopt));
	if (!TM)
		return fail("createTargetMachine failed");

	// 目标机权威 DataLayout 覆写模块，保证与新 PM 下降一致。
	M->setDataLayout(TM->createDataLayout());

	std::error_code ec;
	llvm::raw_fd_ostream Dest(obj_path, ec, llvm::sys::fs::OF_None);
	if (ec)
		return fail("cannot write .obj: " + ec.message());
	llvm::raw_pwrite_stream &OS = Dest; // 常规文件流可 seek，无需 buffer_ostream

	llvm::TargetLibraryInfoImpl TLII(TT);
	llvm::CGPassBuilderOption Opt;
	Opt.DisableVerify = true;

	llvm::MachineModuleInfo MMI(TM.get());
	llvm::PassInstrumentationCallbacks PIC;
	llvm::StandardInstrumentations SI(M->getContext(), /*DebugLogging=*/false,
								  /*DebugPassManager=*/false);
	llvm::registerCodeGenCallback(PIC, *TM);

	llvm::MachineFunctionAnalysisManager MFAM;
	llvm::LoopAnalysisManager LAM;
	llvm::FunctionAnalysisManager FAM;
	llvm::CGSCCAnalysisManager CGAM;
	llvm::ModuleAnalysisManager MAM;

	FAM.registerPass([&] { return llvm::TargetLibraryAnalysis(TLII); });
	MAM.registerPass([&] {
		const llvm::TargetOptions &TO = TM->Options;
		return llvm::RuntimeLibraryAnalysis(
			M->getTargetTriple(), TM->Options.ExceptionModel,
			TM->Options.FloatABIType, TM->Options.EABIVersion,
			TO.MCOptions.ABIName, TM->Options.VecLib);
	});
	MAM.registerPass([&] { return llvm::MachineModuleAnalysis(MMI); });

	llvm::PassBuilder PB(TM.get(), llvm::PipelineTuningOptions(), std::nullopt, &PIC);
	PB.registerModuleAnalyses(MAM);
	PB.registerCGSCCAnalyses(CGAM);
	PB.registerFunctionAnalyses(FAM);
	PB.registerLoopAnalyses(LAM);
	PB.registerMachineFunctionAnalyses(MFAM);
	PB.crossRegisterProxies(LAM, FAM, CGAM, MAM, &MFAM);
	SI.registerCallbacks(PIC, &MAM);

	llvm::ModulePassManager MPM;
	if (auto Err = TM->buildCodeGenPipeline(
			MPM, MAM, OS, /*DwoOut=*/nullptr, llvm::CodeGenFileType::ObjectFile,
			Opt, MMI.getContext(), &PIC))
	{
		std::string msg;
		llvm::raw_string_ostream ms(msg);
		ms << "buildCodeGenPipeline: " << llvm::toString(std::move(Err));
		return fail(msg);
	}
	MPM.run(*M, MAM);
	Dest.flush();
	return true;
}

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

	// P229：回传入口子系统标志（供链接层依 -subsystem/-entry 分岔）。
	state.windows_subsystem = cg.windows_subsystem_;
	state.use_winmain = cg.use_winmain_;

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

	// -c 编译模式：经 X86 TargetMachine 下 Lower 为 COFF .obj，写到输出路径
	// （默认 <stem>.obj）；不链接、不删除（区别于链接模式的临时 .obj）。
	if (opts.compile_only)
	{
		const std::string obj_out = opts.output.empty() ? (module_name + ".obj") : opts.output;
		state.obj_path = obj_out; // 保留（不删除）
		return emit_object_file(cg.module.get(), obj_out, opts.source_path, errors);
	}

	// 链接模式：经 X86 TargetMachine 下 Lower 为 COFF .obj（写临时目录，进程内 lld 链接后删除）
	if (opts.link_output)
	{
		std::error_code ec;
		std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
		if (ec)
		{
			tmp = std::filesystem::current_path(ec);
		}
		state.obj_path = (tmp / (module_name + ".obj")).string();
		return emit_object_file(cg.module.get(), state.obj_path, opts.source_path, errors);
	}

	return true;
}

}  // namespace colang
