// ============================================================
// accelerators.cpp — ACCELERATORS 块 → ACCELERATOR（type 9）资源数据
//
// rc.exe 编码规律（真值探针 _tmp/accel 三组覆盖矩阵坐实，并经 A/B 逐字节修正）：
//   · 元素布局：每条 ACCEL 占 8 字节紧排 =
//       [0] fVirt(u8)  [1] 0x00(u8)  [2..3] key(u16 LE)  [4..5] cmd(u16 LE)  [6..7] 0x0000(u16)
//     即结构 {BYTE fVirt; WORD key; WORD cmd;} 每元素对齐补到 8 字节；无数组间额外填充、
//     无终止哨兵项，dataSize 恒 = 8 × 条数。
//   · fVirt：选项位或——VIRTKEY=0x01 / NOINVERT=0x02 / SHIFT=0x04 / CONTROL=0x08 / ALT=0x10。
//     **块内最后一条 fVirt 额外置 bit7(0x80)**（rc.exe 实测的结束标记，非标准 ACCEL 语义，
//     但 A/B 逐字节一致必须复现：单条块即该条带 0x80，多条块仅末条带）。
//   · key/cmd 的取值与合法性判定全在 parser（字符码/VK 码、cmd 数值），此处纯字节装配。
// ============================================================

#include <cstdint>
#include <vector>

#include "colang/common.h"
#include "colang/winres.h"
#include "rc_util.h"

namespace colang
{

bool rc_encode_accelerators(const std::vector<AccelEntry> &accels, ResItem &out,
                            std::vector<Error> &errors)
{
	(void)errors; // 合法性（选项名/VK 名/cmd 数值）均在 parser 判定，此处无新增诊断

	out.type_is_string = false;
	out.type_id = 9; // RT_ACCELERATOR

	std::vector<uint8_t> data;
	const std::size_t n = accels.size();
	for (std::size_t i = 0; i < n; ++i)
	{
		const AccelEntry &a = accels[i];
		uint8_t fVirt = a.fVirt;
		if (i + 1 == n)
			fVirt |= 0x80; // 末条结束标记（对齐 rc.exe 实测）
		rcutil::put_u16(data, static_cast<uint16_t>(fVirt)); // fVirt(u8) + 0(u8)（小端低字节）
		rcutil::put_u16(data, a.key);
		rcutil::put_u16(data, a.cmd);
		rcutil::put_u16(data, 0); // 每条补至 8 字节
	}
	out.data = std::move(data);
	return true;
}

} // namespace colang
