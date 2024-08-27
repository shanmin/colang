#include "colang.h"

std::vector<VARLIST> varlist;

//压入新的作用域
//	name	作用域名称
void scope::push(std::string zone)
{
	VARLIST vlist;
	vlist.zone = zone;
	varlist.push_back(vlist);
}

//弹出作用域
void scope::pop()
{
	varlist.pop_back();
}

//设置变量
void scope::set(VARINFO vi)
{
	varlist[varlist.size() - 1].info[vi.name] = vi;
}

//获取变量
VARINFO scope::get(TOKEN token)
{
	for (int vi = varlist.size() - 1; vi >= 0; vi--)
	{
		if (varlist[vi].info.find(token.Value) == varlist[vi].info.end())
			continue; //未找到指定变量名称
		VARINFO vinfo = varlist[vi].info[token.Value];
		if (vinfo.value)
			return vinfo;
		//如果当前是function定义，则下一步转到全局变量 20240411 shanmin
		//	当时为什么一步就到全局变量，这点忘了原因了 20240827 shanmin
		if (varlist[vi].zone == "function")
			vi = 0;
	}
	//报错
	std::vector<TOKEN> tmp;
	tmp.push_back(token);
	ErrorExit("ERROR: 变量不存在", tmp);
}

//	THE END