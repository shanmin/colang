# 走进Co

本章用两个最小示例带你上手：一个向控制台打印一句问候，一个弹出 Windows 消息框。两段代码都只使用前文已经写明的基本写法，不涉及额外的工程配置。

## 最简控制台示例

下面这段程序在顶层直接打印一句问候，然后结束。它演示了最简的Co语言示例。

```co
printf("Hello, Colang!\n");
```

## 最简 Windows GUI 示例

下面这段程序是 UI 示例，在入口里弹出一个消息框，用户关闭消息框后进程退出。它演示了用 `import` 取用 `winapi.co` 中的原型与常量、GUI 入口、以及消息框的调用方式。

```co
[[subsystem("windows")]]
import * from winapi;
MessageBoxA(0, "Hello, Colang!", "Colang GUI", MB_OK | MB_ICONINFORMATION);
```
