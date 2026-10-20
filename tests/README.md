# clumsy 单元测试约定

纯逻辑模块（不依赖 GUI、不加载驱动）配一个独立测试程序。

## 约定

- 每个被测单元一个文件：`tests/test_<name>.c`
- 每个测试有自己的 `main()`，用 `<assert.h>` 断言
- 成功时最后一行打印 `OK: <name>`，退出码 0；失败由 `assert` 中止（非 0）
- 编译产物放 `build/tests/`，不进 git
- 需要 WinDivert 用户态 helper 时链接对应的 `WinDivert.dll`（无需管理员、不加载驱动）
- 不测试 IUP 界面与真实抓包；那些走人工验收（见 spec §7）

## 编译运行（MSYS2 mingw64 环境）

```bash
mkdir -p build/tests
gcc -std=gnu11 -g -Wall -Iexternal/WinDivert-2.2.0-A/include -Isrc \
    tests/test_<name>.c src/<name>.c \
    external/WinDivert-2.2.0-A/x64/WinDivert.dll \
    -lws2_32 -liphlpapi \
    -o build/tests/test_<name>.exe
./build/tests/test_<name>.exe
```

测试只编译它实际用到的 `src/` 文件，避免拉入 IUP `main()`。
