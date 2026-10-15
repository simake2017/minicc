# 37. 内建静态宏 `__MINICC__`：飞轮第一块砖

> 主题：预处理器内建宏（[cpp.predefined] 的 minicc 扩展）
> 背景任务：让 ministl 头文件成为**双栖**代码——宿主编译器（clang/gcc）用系统
> 标准库，minicc 用自供桩。开关就是 `#ifdef __MINICC__`。
> 理论点：[cpp.predefined]（标准规定实现"应当"预定义哪些宏）、静态宏 vs 动态宏
> clang 对照：`lib/Lex/Preprocessor.cpp` `InitPreprocessor()`
> → 本实现：`src/preprocessor.cpp` 构造函数（preprocessor.cpp:129）

## 1. 理论背景：内建宏的两种体质

[cpp.predefined] 列了翻译单元**开始之前**就已存在的宏。按实现方式分两类：

| 体质 | 值是否随位置变化 | 存放处 | 例子（任意编译器） |
|---|---|---|---|
| 静态宏 | 否 | 宏表（与 `#define` 同一张） | `__GNUC__`、`__clang__`、`__STDC__` |
| 动态宏 | 是 | 不存表，展开现场生成 | `__LINE__`、`__FILE__` |

minicc 已有后者（`__LINE__/__FILE__` 在 `expand()` 里就地生成，
见 src/preprocessor.cpp 的 builtin 分支）；`__MINICC__` 是前者——
值恒为 1，直接进宏表：

```cpp
// src/preprocessor.cpp 构造函数
MacroDef minicc{"__MINICC__", {}, "1", false};
m_macros[minicc.name] = minicc;
```

进宏表意味着**白得三条求值路径**，不需要任何新代码：

1. `#ifdef __MINICC__` —— 条件栈查 `m_macros.count`（[cpp.cond]）
2. `#if __MINICC__ >= 1` —— `evalConstantExpr` 第②步先对整个表达式跑一遍
   `expand()`，宏名就地换成 `1`，再走递归下降求值（[cpp.cond]/4：
   "宏展开后才对表达式求值"）
3. `#if defined(__MINICC__)` —— `defined` 运算符在第①步单独处理，
   先于宏展开查表（[cpp.cond]/5）

## 2. 设计决策

### 2.1 为什么值取 1，不带版本号

真实编译器把主版本号编码进去（`__GNUC__ = 12`、`__clang__ = 18`），
头文件据此写 `__GNUC__ >= 9` 这类门槛。minicc 没有版本号概念，
教学项目也不打算演"版本考古"，取 1 即可表达"是 minicc"。

### 2.2 为什么统计行要剔除它

main.cpp 末尾打印 `N macro(s) defined`，这个数字是**全部既有集成
日志基线的契约**。内建宏占一个表位后，`macros().size()` 每个翻译单元
恒 +1 —— 114 份基线全部漂移，而且漂移不含任何教学信息。

修法是给观测口径分层：`userMacroCount()` 数宏表时剔除
`isBuiltinMacro()` 名单里的名字：

```cpp
// include/preprocessor.h
static bool isBuiltinMacro(const std::string& name) { return name == "__MINICC__"; }
int userMacroCount() const {
    int n = 0;
    for (auto& [k, _] : m_macros) { if (!isBuiltinMacro(k)) n++; }
    return n;
}
```

`#define` 同名覆盖、`#undef` 删除对它同样生效（与 clang 一致——
标准不禁止重定义内建宏，只是诊断 quality of implementation 的事）。

### 2.3 与 ministl 飞轮的接口

ministl 头文件里将出现这样的双栖分支：

```cpp
#ifdef __MINICC__
#  include "ministl/detail/shim_cstddef.h"   // minicc 自供：size_t 等
#else
#  include <cstddef>                          // 宿主：系统标准库
#endif
```

注意桩头用**引号 include + 完整路径**（`"ministl/detail/..."`），
不在 `-I` 目录里造叫 `cstddef` 的文件——那样会连 clang 一起劫持
（`-I` 目录对尖括号 include 同样生效，见 tests/search-path 的演示），
`#include_next` 又超出 minicc 预处理器目前的语法面。

## 3. clang 源码对照表

| clang（llvm-project/） | 本实现 | 简化了什么 |
|---|---|---|
| `lib/Lex/Preprocessor.cpp` `InitPreprocessor()`：注册几十上百个内建宏，值从 `LangOpts`/版本号推导 | `Preprocessor::Preprocessor()` 两行：一个静态宏，值恒 1 | 没有版本号、没有 target 相关宏族（`__x86_64__` 等）、没有 `__has_include` 这类函数式内建 |
| 宏表就是 `MacroInfo` 池，`#define` 与内建宏同池 | `m_macros`（unordered_map），`__MINICC__` 与 `#define` 产物同表 | 同构，未简化 |
| 动态宏 `__LINE__`：`Lex/PPExpressions.cpp` / `PPMacroExpansion.cpp` 特判 | `expand()` 内 `if (word == "__LINE__")` 就地生成 | 同构，未简化 |

## 4. 可复现实验

```bash
# ① #ifdef 分支 + 运行期返回值钉死宏的值
cat > /tmp/p.cpp <<'EOF'
#ifdef __MINICC__
int main() { return __MINICC__ - 1; }   // 1-1=0 ⇒ rc=0
#else
#error "not minicc"
#endif
EOF
./minicc /tmp/p.cpp -o /tmp/p && /tmp/p; echo "rc=$?"

# ② 宏统计不把它算进去（0 macro(s) defined）
./minicc tests/pp/test_pp_05_minicc_macro.cpp -o /tmp/p 2>&1 | grep "macro(s)"

# ③ 宿主编译器上未定义 ⇒ 同一份文件双栖可编译（走 #else 分支）
clang++-18 -std=c++20 tests/pp/test_pp_05_minicc_macro.cpp -o /tmp/p && /tmp/p; echo "host rc=$?"
```

预期日志特征（`#if` 路径）：

```text
[pp] /tmp/p.cpp:1 #ifdef __MINICC__ → branch taken
```

## 5. 测试

| 层 | 文件 | 内容 |
|---|---|---|
| 集成（正例） | tests/pp/test_pp_05_minicc_macro.cpp | `#ifdef`/`#if`/`defined()` 三路 + 运行期返回值钉值 + 宿主机 `#else` 分支（双栖可编译） |
| 单测 | tests/unit/test_preprocessor.cpp `BuiltinMacro.*` | 4 例：构造注入 / 用户宏统计剔除 / `#ifdef` 分支可达 / `#if` 值求值 |

**突变验证**：摘掉构造函数里的注入 ⇒ `InjectedAtConstruction`、
`IfdefBranchTaken`、`ValueInIfExpression` 三例实跑变红
（`UserMacroCountExcludesBuiltin` 钉的是统计口径，不含注入，保持绿是正确行为）。

## 6. 关键过程 ASCII 图

```text
              翻译单元起点
                   │
     Preprocessor 构造函数
                   │ 注入 MacroDef{"__MINICC__", {}, "1"}
                   ▼
        ┌─────────────────────┐
        │  m_macros（宏表）    │ ◄── #define 产物与内建宏同池
        └─────────────────────┘
          │          │            │
   #ifdef 查 count  #if 求值时    expand() 普通展开
   （条件栈）      先 expand      （代码里出现 __MINICC__）
   分支 taken      再求值 → 1     就地换成 1
                   │
        "0 macro(s) defined"     ── userMacroCount() 剔除内建，
                                   既有 114 份日志基线零漂移
```

## 7. 边界与后续

- 未做：`__has_include`、`__has_cpp_attribute` 这类**函数式内建宏**
  （需要预处理器在条件表达式里回调文件系统探测）；
  target 宏族（`__x86_64`、`__linux` 等）——minicc 只出 x86-64 Linux，
  暂不需要；`#undef __MINICC__` 目前无警告（教学版宽松）。
- 飞轮下一步：ministl 侧按 `__MINICC__` 分支提供
  `detail/shim_cstddef.h` 等自供桩头（README「现状 2026-10」的路线图），
  之后才会撞上真 parser 缺口（`initializer_list` 文法、`throw`）。
