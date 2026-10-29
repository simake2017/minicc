# 05.3 SFINAE 与依赖名 —— 软失败协议

> 上级：[00-总览](00-总览.md）。

## 管什么

「替换失败不是错误」（[temp.deduct]/8）的落地：三方协议 + 依赖名两阶段裁决。

## 关键判据

- **三方协议**（独立模块，收口点全项目仅三处）：SubstitutionFailure（异常
  信号）+ Sfinae::attempt（吸收器，把软失败变 optional）+ SfinaeContext
  （直接上下文标记——**出了直接上下文就是硬错误**）。对照 clang 的 SFINAETrap。
- **依赖类型名 `typename T::type`**（learn/25，[temp.res]/5 悬案）：定义期判
  不出 `T::x` 是类型还是值——Parser 收 typename 前缀建「限定者=TemplateParam」
  节点，Sema 原样保留，**替换当场**（Case 5.5）查实例成员表解糖；查不到即软
  失败 ⇒ `void_t<typename T::type>` 探测惯用法成立。硬错误文案与 clang 逐字。
- **void_t 与 CWG 1558**：别名模板特例——CWG 1558 前 void_t 探测在别名上会
  硬失败；本项目按现行标准实现（learn/20）。
- **std 垫片注入**：false_type/true_type/void_t/declval 走 registerBuiltins
  **内建注入**——绕开别名模板与类内 static 的语法缺口；declval 名字同时是种
  进 parser 名字登记表（内建名字 parser 必须知道，同 clang 名单制）。

## 代码地图

src/sema/sfinae.cpp（唯一 "[sfinae]" 日志出口）、include/sfinae.h（协议契约）、
substituteType Case 5.5/5.8（当场解糖的两个闸）

## 测试入口

tests/tmpl/（33..39 SFINAE 套件）、tests/unit/test_decltype_sfinae.cpp
（Decltype./Sfinae./PartialOrder./SfinaeProtocol. 四套件）

## 深入读

learn/20（SFINAE 协议）、learn/25（依赖类型名）、learn/21（偏序，→ 05.4）
