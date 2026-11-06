// ============================================================
// archive.cpp — P384 静态库归档产出（link_archive）
//
// 把编译期产出的目标文件打包为静态库：不链接、不链 CRT、无入口，
// 仅归档成员 + 写符号索引（SymtabWritingMode::NormalSymtab）。
// 走 LLVM 自带 object::writeArchive（Object 组件已在构建链入清单）。
//
// 归档格式随 host 构建分平台（与链接层铁律同口径）：
//   Windows 构建 → COFF 归档（.lib，object::Archive::K_COFF）；
//   Linux 构建   → GNU  归档（.a， object::Archive::K_GNU）。
// 扩展名分派与交叉格式拦截在调用方（main.cpp）完成，本函数只在匹配 host
// 的扩展名时被调用；Deterministic=true 保可复现（成员时间戳归零）。
// ============================================================

#include <string>
#include <utility>
#include <vector>

#include <llvm/Object/Archive.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/Error.h>

#include "colang/common.h"
#include "colang/link.h"

namespace colang
{

bool link_archive(const std::vector<std::string> &objs, const std::string &out_path,
                  std::vector<Error> &errors)
{
	const std::string file = objs.empty() ? out_path : objs.front();
	auto fail = [&](const std::string &msg) -> bool
	{
		errors.push_back(Error{file, Pos{1, 1}, "archiver: " + msg, false});
		return false;
	};

	if (objs.empty())
		return fail("no object files to archive");

	// 逐 obj 读入归档成员（getFile 自带 MemoryBuffer，Deterministic=true 归零时间戳）。
	std::vector<llvm::NewArchiveMember> members;
	members.reserve(objs.size());
	for (const std::string &o : objs)
	{
		llvm::Expected<llvm::NewArchiveMember> m =
			llvm::NewArchiveMember::getFile(o, /*Deterministic=*/true);
		if (!m)
			return fail("cannot read object file '" + o + "': " +
			                llvm::toString(m.takeError()));
		members.push_back(std::move(*m));
	}

	// 归档格式随 host：Windows 构建产 COFF .lib，Linux 构建产 GNU .a。
#ifdef _WIN32
	const llvm::object::Archive::Kind kind = llvm::object::Archive::K_COFF;
#else
	const llvm::object::Archive::Kind kind = llvm::object::Archive::K_GNU;
#endif

	llvm::Error err = llvm::writeArchive(out_path, members,
	                                     llvm::SymtabWritingMode::NormalSymtab,
	                                     kind, /*Deterministic=*/true, /*Thin=*/false);
	if (err)
		return fail("failed to write archive '" + out_path + "': " +
		                llvm::toString(std::move(err)));
	return true;
}

}  // namespace colang
