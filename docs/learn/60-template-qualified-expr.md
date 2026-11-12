# 60. 模板限定表达式与静态调用

> 定位：在表达式位置保留 template-id 后的完整限定链，并区分类型限定与对象求值。
> 标准：[basic.lookup.qual]、[expr.prim.id.qual]、[class.static]、[temp.inst]。
> 关联：docs/learn/53-nested-class.md（嵌套类）、docs/BUGS.md B59。

## §0 定位与能力清单

| 语法 | 结果 | 验证入口 |
|---|---|---|
| `Box<int>::Inner::b` | 在实例的嵌套类中读取静态常量 | `TemplateQualifiedExpr.NestedConstantsInReturnAndArithmetic` |
| `Box<int>::Inner::Deep::c` | 任意深度的非模板嵌套类限定链 | 同上 |
| `Box<int>::make()` | 静态调用不传 this | `TemplateQualifiedExpr.StaticCallUsesFirstArgumentRegister` |
| `Box<int>::Inner::add(2)` | 嵌套类的静态方法 | 集成测试 39 |
| `Box<int>::bump(x)` | 可写引用形参沿既有地址通道传递 | 集成测试 39 |
| `Factory<int>::make(7)` 返回类对象 | 沿普通函数 sret 通道返回 | 集成测试 39 |
| `Box<int>::a::b`，a 为值 | 语义阶段拒收非法限定者 | `TemplateQualifiedExpr.RejectsValueAsQualifier` |
| `Box<int>::f()`，f 非静态 | 拒收缺少对象的调用 | `TemplateQualifiedExpr.RejectsNonStaticCallWithoutObject` |

## 1 理论背景

限定名查找是一种逐层环境选择：每个中间段决定下一段在哪个声明上下文中查找。
`Box<int>::Inner::b` 的 `Box<int>` 和 `Inner` 都是限定者，不是需要加载的运行时变量。
[ basic.lookup.qual ] 的核心是保留每一层的身份，而不是把最后一个标识符交给全局查找。

[class.static] 规定静态成员函数没有 this 指针。类型限定调用必须选择函数声明，
再按该声明的形参传参；若选中的是非静态方法，缺少对象就是语义错误。
[temp.inst] 下实例化的方法仍是原声明的对应成员，static 属性不能在克隆时丢失。

## 2 设计决策

### 2.1 AST 保留限定链

复用 `VarExpr::explicitTemplateArgs` 与 `MemberExpr::isTypeAccess`。
Parser 消费连续的 `::标识符`，每段产生一个 MemberExpr，保留结构化实参和层次。

```text
Box<int>::Inner::Deep::c
          │
Member(c, type-access)
└─ Member(Deep, type-access)
   └─ Member(Inner, type-access)
      └─ Var(Box, template-args=[int])
```

### 2.2 限定者解析只有一个入口

`SemanticAnalyzer::resolveTypeQualifier` 将根 template-id 交给 `resolveType` 实例化，
后续段交给 `findMemberType`，从上一段的类作用域找类型。
常量折叠、任意表达式中的成员访问和静态调用共享这个入口。
不存在成员类型或限定者不是类时，诊断包含 `is not a class, namespace, or enumeration`。
当前入口只处理本次支持的类限定链，诊断文案不表示已新增命名空间或枚举限定能力。

### 2.3 调用由声明决定

Sema 保留既有成员候选查找，并把选中的 `mangledName` 写入 `resolvedCalleeSymbol`。
类型限定调用选中非静态方法立即拒收。模板方法克隆同时保留 `isStatic`，避免合法静态方法被误判。
对象形式的静态调用也依选中声明省略 this，但对象表达式仍求值一次，保留其副作用。
CodeGen 的类型限定调用使用普通函数通道，复用引用形参和 sret 规则：

```text
静态 int add(int)       → 显式参数从 rdi 开始
静态 void bump(int&)    → rdi 放被绑定对象地址
静态 Result make(int)  → rdi 放返回缓冲地址，显式参数从 rsi 开始
对象的非静态调用       → 保留既有 this 通道
```

## 3 clang 源码对照

| 上游入口 | 本实现位置 | 简化 |
|---|---|---|
| `clang/lib/Parse/ParseExprCXX.cpp:107`，`Parser::ParseOptionalCXXScopeSpecifier` | `src/parse/parser.cpp`，`parsePrimaryExpr` | 仅扩展已有 template-id 后的标识符链 |
| `clang/lib/Sema/SemaTemplateInstantiateDecl.cpp:3142`，`TemplateDeclInstantiator::VisitCXXMethodDecl` | `src/sema/template_instantiation.cpp`，`cloneMethod` | 直接复制教学 AST 方法属性 |
| C++ 的声明上下文限定查找 | `src/sema/semantic_analyzer.cpp`，`resolveTypeQualifier` / `findMemberType` | 使用现有类类型与成员表 |
| 静态方法调用无隐式对象的语义 | `src/codegen/codegen.cpp`，`emitCall` | 使用既有 x86-64 普通函数参数通道 |

### 真实版多做了什么、为什么

`ParseOptionalCXXScopeSpecifier` 处理依赖限定名、template 消歧、模板化的中间段和错误恢复。
本实现本次只支持已有根 template-id 后的非模板类成员链。
`VisitCXXMethodDecl` 同时处理声明上下文、特化、约束和延迟实例化等信息；
教学实现的对应职责是克隆形参、函数体和方法属性，static 仍属于必须保留的语义信息。
完整 C++ 的类/命名空间/枚举查找与歧义规则继续由既有边界约束，本次没有扩大语法范围。

## 4 可复现实验

```bash
clang++-18 -std=c++20 -emit-llvm -S tests/lang/test_basics_39_template_qualified.cpp -o /tmp/qualified.ll
clang++-18 -std=c++20 tests/lang/test_basics_39_template_qualified.cpp -o /tmp/qualified-clang
/tmp/qualified-clang
./build.sh
./minicc tests/lang/test_basics_39_template_qualified.cpp -o /tmp/qualified-mini
/tmp/qualified-mini
build-linux/unit_tests --gtest_filter='TemplateQualifiedExpr.*'
ctest --test-dir build-linux -j6 --output-on-failure
./logdiff.sh diff
```

两个可执行程序的预期退出码均为 0。负例由单测直接检查语义诊断关键词。
汇编单测检查静态调用的 rdi 通道及 this 暂存缺席，集成用例验证实际执行效果。

## 5 观测点与边界

- Parser 的类型限定访问日志逐段出现；嵌套层次可用 `--dump-ast` 观察。
- Sema 的静态常量命中日志包含实例及嵌套类名，避免误查全局同名类型。
- 生成汇编中的静态 call 使用 Sema 回填的符号，参数位置可直接核对。
- 成员模板显式实参、模板化中间限定段及完整 C++ 重载决议不属于此次扩展。

## 6 演进记录

| 日期 | 变化 | 证据 |
|---|---|---|
| 2026-11-12 | 多级模板限定链、静态调用通道与方法 static 属性保留 | 6 个单测及集成测试 39 |
