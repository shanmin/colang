# Getting Started with Co

This chapter walks you through two minimal examples: one prints a greeting to the console, the other pops up a Windows message box. Both use only the basic forms already described above and need no extra project setup.

## Minimal Console Example

The program below prints a greeting directly from the top level and then ends. It demonstrates the simplest Co language example.

```co
printf("Hello, Colang!\n");
```

## Minimal Windows GUI Example

The program below is a UI example: it pops up a message box in the entry point, and the process exits when the user closes it. It shows how to import prototypes and constants from `winapi.co` with `import`, the GUI entry point, and how a message box is called.

```co
[[subsystem("windows")]]
import * from winapi;
MessageBoxA(0, "Hello, Colang!", "Colang GUI", MB_OK | MB_ICONINFORMATION);
```
