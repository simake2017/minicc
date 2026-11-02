# 37. 内建静态宏 `__MINICC__`：飞轮第一块砖

> 主题：预处理器内建宏（[cpp.predefined] 的 minicc 扩展）
> 背景任务：让 ministl 头文件成为**双栖**代码——宿主编译器（clang/gcc）用系统
> 标准库，minicc 用自供桩。开关就是 `#ifdef __MINICC__`。
> 理论点：[cpp.predefined]（标准规定实现"应当"预定义哪些宏）、静态宏 vs 动态宏
> clang 对照：`lib/Lex/Preprocessor.cpp` `InitPreprocessor()`
> → 本实现：`src/preprocessor.cpp` 构造函数（preprocessor.cpp:129）

## §0 定位与能力清单

内建宏是"翻译单元开始之前就已存在"的宏 —— 它是**编译器向库作者发信号**的
唯一通道：库代码靠它分成"给宿主编译器跑"与"给 minicc 跑"两条支路。
minicc 只做一个（`__MINICC__`），但这一个就足以让 ministl 头文件成为
**双栖**代码。代价是这个信号会占掉宏表的一个表位，于是还牵出一条
**统计口径**的账（§2.2）。

| 能力 | 一句话 | 状态 |
|---|---|---|
| 内建静态宏（[cpp.predefined] 的实现定义宏） | `__MINICC__` 值恒 1，与 `#define` 产物同表 | 已实现 |
| `#ifdef` 查表分支 | 条件栈查 `m_macros.count`（[cpp.cond]） | 已实现 |
| `#if` 表达式里"先展开再求值" | [cpp.cond]/4：宏展开后就地换成 1 再递归下降求值 | 已实现 |
| `defined()` 运算符 | [cpp.cond]/5：先于宏展开单独处理 | 已实现 |
| 动态宏 `__LINE__` / `__FILE__` | 展开现场就地生成，不存表（本批之前已有） | 已实现 |
| 统计口径分层（内建宏不计入 `N macro(s) defined`） | `userMacroCount()` 剔除内建名单 | 已实现 |
| `#define` 同名覆盖 / `#undef` 删除对内建宏同样生效 | 与 clang 一致（标准不禁止重定义内建宏） | 已实现 |
| 版本号编码（`__GNUC__ = 12` 那种） | minicc 没有版本号概念 | 未做 |
| target 宏族（`__x86_64__` / `__linux__` 等） | 只出 x86-64 Linux，暂不需要 | 未做 |
| 函数式内建宏 `__has_include` / `__has_cpp_attribute` | 需预处理器在条件表达式里回调文件系统探测 | 未做 |
| `#undef` 内建宏的诊断 | 教学版宽松，无警告 | 未做 |

---

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

---

## 8. 真实版多做了什么、为什么

素材来自 §3 的「简化了什么」列与 §7 的边界清单。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `InitPreprocessor()` 注册几十上百个内建宏，值从 `LangOpts` / 版本号推导 | minicc 只出 x86-64 Linux 且无版本号概念；一个 `__MINICC__` 已够"双栖"这一个用途 |
| 版本号编进宏值（`__GNUC__ = 12`） | 教学项目不演"版本考古" —— 头文件不会写 `__MINICC__ >= 9` 这类门槛 |
| target 宏族（`__x86_64__`、`__linux__` 等） | 目标平台唯一，不存在"按宏分支选实现"的需求 |
| `__has_include` / `__has_cpp_attribute` 这类**函数式内建** | 需要预处理器在条件表达式里回调文件系统探测 —— 当前预处理器语法面没有这个入口 |
| 内建宏按类别注册（builtin / predef / command line 三类分别登记） | minicc 只有一张 `m_macros`，分类维度对双栖这个用途没有增量 |
| 内建宏的 `#undef` 语义（clang 的 `BuiltinMacro` 带 `isPredefined` 属性，undef 后另有处理） | 本项目一条宏路走通即可；标准不禁止重定义内建宏，诊断属 quality of implementation（§2.2 已述） |

★ 与 clang **同构、未简化**的两处（不在本表列）：宏表即 `MacroInfo` 池
（`#define` 产物与内建宏同池）；动态宏 `__LINE__` 也是展开现场特判。

---

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-10-15 | 与 ministl::map/set 同一提交落地内建静态宏 `__MINICC__`，并把统计口径分层（`userMacroCount()` 剔除内建名单） | 飞轮第一块砖：让 ministl 头文件成为双栖代码；不剔除的话 114 份日志基线全漂移且漂移不含教学信息（git 3a2f6be） |
