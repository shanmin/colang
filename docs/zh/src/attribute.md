# 属性（`[[...]]`）

## 语法

Co 采用 C23 的属性拼写：一对方括号 `[[` `]]` 包住若干属性，属性之间用逗号分隔，写在声明之前：

```
[[ ATTRIBUTE1, ATTRIBUTE2(...) ]]  声明
```

Co 当前赋予实际意义的属性有两个：

| 属性 | 作用于 | 效果 |
| --- | --- | --- |
| `[[lib("X")]]` | 顶层函数声明、全局变量声明 | 声明该符号由外部库 `X` 供给，链接期自动拉入 `X` |
| `[[deprecated]]`、`[[deprecated("消息")]]` | 顶层函数、全局变量 | 引用该符号时发一条告警 |

属性只用 `[[` 这一种拼写；C 里等价的 `<:` `::>` 替代拼写 Co 不支持。

## `[[lib("X")]]`——声明库归属

外部函数（如操作系统 API）由哪个库实现，过去无法表达：只写原型而不告诉链接器库名，链接就会报未定义符号。`[[lib("X")]]` 补上这个归属：

```
[[lib("user32")]] int GetSystemMetrics(int nIndex);
[[lib("user32")]] unsigned int GetDoubleClickTime(void);

public int screen_width()
{
    return GetSystemMetrics(0);   // 无需命令行给 -luser32，链接器自动拉入 user32
}
```

`X` 是**链接名**（不含扩展名、路径、空白、引号、冒号），由链接器按目标平台补全，写法与命令行 `-l` 同形。因此 `"user32"` 合法，`"user32.lib"`、`""`、`"sub/user32"`、`"C:x64/user32"` 都会被拒绝。

约束：

- **只能挂在声明上**：函数须是纯原型（无函数体），全局变量须是纯外部声明（无初始化式）。声称来自外部库却又在本模块写出定义，是矛盾，编译器拒绝。
- **与 `public`、`static` 互斥**：`public` 是导出、`lib` 是导入，不能落在同一符号上；`static` 表示内部链接，也不可能由外部库供给。
- **同名符号的库归属必须一致**：同一名字重复声明且库名相同是合法的；同名却标了两个不同的库，编译器报错（不做静默取舍）。
- 一个声明上写两个 `[[lib]]` 报错。

## `[[deprecated]]`——标记弃用

给函数或全局变量标 `[[deprecated]]`，此后每次引用都会发一条告警，提醒改用别的接口；带消息可写明替代方案：

```
[[deprecated]] int old_add(int x);
[[deprecated("use new_api instead")]] int new_api(int x);

int use(int x)
{
    return new_api(x);   // warning: 'new_api' is deprecated: use new_api instead
}
```

- 属性挂在前置声明、定义写在另一处时，按**同一实体**合并——调用点照样告警。
- 告警**不改变程序退出码**，也不阻断编译。
- 局部作用域里的声明上的 `deprecated` 当前不生效。

## 探测与未识别属性

预处理指令 `__has_c_attribute(名字)` 返回 `1` 或 `0`，可用于条件编译：

```
#if __has_c_attribute(lib)
[[lib("user32")]] int GetSystemMetrics(int nIndex);
#endif
```

- 已知但当前批次尚无实际效果的属性（如 `nodiscard`、`maybe_unused`、`noreturn`、`fallthrough` 等）**静默接受**——不报错也不刷警，为的是不打扰从 C/C++ 迁来的代码。
- 未识别的裸名属性发一条 `unknown attribute 'x' ignored` 告警并跳过，不阻断编译。
- 带命名空间的属性名（如 `[[colang::lib]]`）**一律报错拒收**：Co 不假装支持自己兑现不了的厂商属性。

## 与 C / C++ 的关系

| 方面 | Co | 相对语言 |
| --- | --- | --- |
| 属性拼写 `[[...]]` | 沿用 C23 | 与 C23 相同 |
| `[[deprecated]]` | 支持（诊断层，不进产物） | 与 C23 语义一致 |
| `[[nodiscard]]` / `[[maybe_unused]]` / `[[noreturn]]` 等 | 接受但不产生效果 | 与 C23 不同（C23 有实际语义） |
| `[[lib("X")]]` | Co 新定义，C 标准无此属性 | Co 独有 |
| `<:` `::>` 替代拼写 | 不支持 | 与 C23 不同 |

> **兼容性告知**：
> 1. `[[lib("X")]]` 是 Co 自定义属性。把 `.co`/`.c` 交给真正的 clang/clangd 看时，它会按未知属性 `-Wunknown-attributes` 警告并忽略——**这是必然如此、不是缺陷**；外部工具不会校验库归属，只有 Co 编译器消费它。
> 2. 从 C/C++ 迁移时，`#pragma comment(lib, "X")` 与 `__declspec(dllimport)` 本编译器**既不实现、也不模拟**，库归属一律须显式改写为 `[[lib("X")]]`。
> 3. `[[deprecated]]` 是纯前端诊断，Co 与 clang 一样**不把它编进产物 IR**，因此不会传导到依赖该产物的下游模块。

## 当前边界（编译器实装状态）

- `[[lib("X")]]` 落地为 COFF `.drectve` 段的 `/DEFAULTLIB` 链接指令，按声明首现序去重；这依赖链接步能消费该指令，**尚不能**做到「同名符号按具体 dll 精确绑定」——多 dll 同名符号的 per-symbol 绑定属后续链接器能力。
- `[[deprecated]]` 生效范围限于顶层函数与全局变量的引用点告警。
- `[[nodiscard]]`、`[[maybe_unused]]`、`[[noreturn]]`、`[[align]]` 等的实际效果尚未实装（当前仅静默接受）。
