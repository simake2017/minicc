# 05.5 NTTP —— 非类型模板实参

> 上级：[00-总览](00-总览.md）。

## 管什么

`template<int N>` 一族：值实参的表示、校验、编码、参与特化模式。

## 关键判据

- **TemplateArg tagged 值**取代裸 TypePtr（learn/18）：类型/值混排实参表
  （`Box<T, 4>`）、负数实参、形态校验。实例缓存键吃无损编码（→ 05.2）。
- **类型域扩展**（learn/35）：形参从只认 int 扩到整型家族 + 字符 + `auto`。
  三段独立：① 形参写法（parseBuiltinTypeSpecifierSeq 六组计数器再一次归一，
  对照 clang DeclSpec 位掩码；非法组合各自报错）② 实参字面量（[lex.icon]/2
  全形态）③ 可表示性判据（→ 04.1 canRepresentValue，B17）。
- **`template<auto V>`**（[temp.param]/6）：形态即类型 ⇒ K<4>(int) 与
  K<4L>(long) 是两个实例——形态只在 auto 形参位写入键。
- **常量表达式实参**（learn/39）：`Buf<2+2>`——Parser 只存树不评价
  （[expr.const] 裁决权在语义期，同 clang Expression→Value 形态转换）；
  **求值归一发生在算缓存键之前**，否则 Buf<2+2> 与 Buf<4> 撞/分键都错；
  归一后下游（形态归一/可表示性/mangling）一行未动、既有用例零漂移。
- **常量变量实参**（learn/40）：`Buf<k>` 经编译期常量表重解（→ 04.6）。
- **mangling 与 clang 逐字符相同**（12/12，含 char/signed char/unsigned
  char 三元区分）；TnDa（函数模板 auto NTTP 的 <template-param-decl>）
  有意不写——没有能到达该编码的路径。

## 代码地图

src/sema/template_instantiation.cpp（getOrInstantiateClass 归一点）、
src/sema/semantic_analyzer.cpp（checkTemplateArguments 可表示性）、
src/ast/type.cpp（canRepresentValue）

## 测试入口

tests/tmpl/（21..26、61..68、74..77）、tests/unit/test_nttp_type_domain.cpp
（10 例四条判据分支逐条突变验证）

## 深入读

learn/18（tagged 值两层替换）、learn/35（类型域）、learn/39、40（常量表达式/常量变量实参）
