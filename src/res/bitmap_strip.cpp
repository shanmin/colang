// ============================================================
// bitmap_strip.cpp — .bmp 去 14 字节 BITMAPFILEHEADER → DIB（RT_BITMAP 数据）
//
// BITMAPFILEHEADER = u16 type('BM'=0x4D42) + u32 size + u16 rsv1 + u16 rsv2
//                    + u32 offbits  →  共 14 字节。
// RT_BITMAP 存的是纯 DIB（BITMAPINFOHEADER 起），即去掉这 14 字节文件头。
// 与 icon 同「文件裁剪」族：只搬运、不重编码。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

#include "colang/common.h"
#include "colang/winres.h"
#include "rc_util.h"

namespace colang
{

bool rc_strip_bitmap(const std::string &bmp_path, std::vector<uint8_t> &dib,
                     std::vector<Error> &errors, const SourceLoc &loc)
{
	std::vector<uint8_t> bmp;
	if (!rcutil::read_file_bytes(bmp_path, bmp))
	{
		errors.push_back(Error{loc.file, Pos{loc.line, loc.column},
		                       "cannot open bitmap file: " + bmp_path});
		return false;
	}
	if (bmp.size() < 14)
	{
		errors.push_back(Error{loc.file, Pos{loc.line, loc.column},
		                       "bitmap file too small: " + bmp_path});
		return false;
	}
	// 校 'BM' 魔数（宽松：不符仅告警级错误，交调用方判致命与否）
	const uint16_t magic = static_cast<uint16_t>(bmp[0]) | (static_cast<uint16_t>(bmp[1]) << 8);
	if (magic != 0x4D42)
	{
		errors.push_back(Error{loc.file, Pos{loc.line, loc.column},
		                       "not a BMP (missing 'BM' signature): " + bmp_path});
		return false;
	}
	dib.assign(bmp.begin() + 14, bmp.end()); // 去 BITMAPFILEHEADER
	return true;
}

}  // namespace colang
