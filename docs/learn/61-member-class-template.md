# 61. 成员类模板与逐段模板限定名

> 定位：外层类模板先绑定自身实参，成员类模板保留内层形参，在使用点继续实例化。
> 标准：[temp.mem]、[temp.inst]、[basic.lookup.qual]。
> 关联：[成员模板形参表](58-member-template-param-list-unification.md)、[模板限定表达式](60-template-qualified-expr.md)。

## §0 定位与能力清单

| 语法 | 结果 | 验证入口 |
|---|---|---|
| 类体内 `template<class U> struct Inner` | 保存成员类模板蓝图 | 集成测试 tmpl 79 |
| `Outer<int>::Inner<long>` | 分两层绑定，生成具体类型 | `TemplateQualifiedExpr.MemberClassTemplateFieldsBindBothLayers` |
| `Outer<int>::Inner<long>::value_type` | 从内层实例查类型别名 | 集成测试 tmpl 79 |
| `Outer<int>::Inner<long>::value` / `add(4)` | 读取静态常量、调用静态方法 | 集成测试 tmpl 79 |
| 函数模板体中的非依赖成员类模板限定访问 | 克隆时保留限定属性和内层实参 | `TemplateQualifiedExpr.MemberClassTemplateAccessSurvivesFunctionInstantiation` |

## 1 理论背景

[temp.mem] 允许类的成员本身是模板。外层类模板实例化与成员类模板实例化是两次结构化替换：外层参数已经确定，并不意味着成员模板自己的参数也已确定。

[temp.inst] 下，外层实例需要保留对应的成员模板声明，成员模板的具体实例则在使用时产生。[basic.lookup.qual] 要求每个限定段决定下一段的查找环境，因此不能只保存末尾的 `Inner` 或 `value_type`。

编译原理上，这相当于嵌套环境中的分阶段代入：外层环境绑定 T，内层环境随后绑定 U。两组实参共同决定最终类型身份。

```text
Outer<T> 蓝图
  └─ Inner<U> 蓝图：字段 T first; U second;

Outer<int> 实例
  └─ Inner<U> 蓝图：字段 int first; U second;
       └─ Inner<long> 实例：字段 int first; long second;

Outer<long> 实例
  └─ Inner<U> 蓝图：字段 long first; U second;
       └─ Inner<int> 实例：字段 long first; int second;
```

## 2 设计决策

类声明将成员类模板保存在 `memberClassTemplates`，与成员函数模板及普通嵌套类分开。解析 template-head 仍调用共享形参表入口，仅在模板体处按 class/struct 或函数声明分派。

类型位置以 `nestedQualifier` 保留限定链，每段的 `templateArgs` 保存自己的实参。表达式位置复用 MemberExpr 链，每段通过 `explicitTemplateArgs` 保存实参；类型限定访问属性在克隆中必须保留，否则 Sema 会把模板名误当运行时变量。

外层实例化复用 `cloneNestedClass` 克隆成员类模板体，替换外层参数并保留内层蓝图。Sema 将蓝图登记在外层实例的限定名下，使用点沿现有 `resolveType` 和 `getOrInstantiateClass` 入口实例化，复用参数检查、缓存、布局和方法注册。

实例名包含外层实例的身份，因此 `Outer<int>::Inner<long>` 和 `Outer<long>::Inner<int>` 不共享成员声明或布局。CodeGen 消费现有具体类布局和函数声明，无需增加成员类模板专用指令生成分支。

## 3 clang 源码对照

| 上游入口 | 本实现位置 | 简化 |
|---|---|---|
| `clang/lib/Sema/SemaTemplateInstantiateDecl.cpp:2317:TemplateDeclInstantiator::VisitClassTemplateDecl` | `src/sema/template_instantiation.cpp`：`instantiate`、`cloneNestedClass` | 直接复制教学 AST，并替换外层参数 |
| `Parser::ParseOptionalCXXScopeSpecifier` | `src/parse/parser.cpp`：`parseType`、`parsePrimaryExpr` | 在既有限定链中保存每段模板实参 |
| 声明上下文中的限定查找与模板实例化 | `src/sema/semantic_analyzer.cpp`：`resolveType`、`resolveTypeQualifier` | 使用实例限定名登记表和既有实例化缓存 |

### 真实版多做了什么、为什么

`VisitClassTemplateDecl` 还维护模板声明与被模板化类声明的上下文、重声明关系和特化信息；这些信息支撑完整语言中的声明合并与延迟实例化。本实现只复制当前教学 AST 所需的蓝图和成员。

`ParseOptionalCXXScopeSpecifier` 还处理依赖限定名、template 消歧和错误恢复。本批验收限于非依赖限定链；`typename Outer<T>::template Inner<U>::value_type` 尚未形成完整支持承诺。成员类模板偏特化、类外定义和完整访问控制也需要独立验证。

## 4 可复现实验

```bash
clang++-18 -std=c++20 -emit-llvm -S tests/tmpl/test_tmpl_79_member_class_template.cpp -o /tmp/member79.ll
clang++-18 -std=c++20 tests/tmpl/test_tmpl_79_member_class_template.cpp -o /tmp/member79-clang
/tmp/member79-clang
cmake --build build-linux -j6
build-linux/minicc tests/tmpl/test_tmpl_79_member_class_template.cpp -o /tmp/member79-mini
/tmp/member79-mini
build-linux/unit_tests --gtest_filter='TemplateQualifiedExpr.MemberClassTemplate*'
ctest --test-dir build-linux -j6 --output-on-failure
./logdiff.sh diff
```

两个程序预期返回 0。字段单测直接检查两组实例的字段类型分别为 int/long 和 long/int，避免仅凭实例名判断替换正确。

## 5 观测点与验证边界

- 成员类模板登记日志显示所属外层实例与模板形参数量。
- 类型替换日志显示外层参数绑定，使用点日志显示内层实例化或缓存命中。
- 函数模板体限定访问的回归测试曾因克隆丢失类型限定属性而报 `use of undeclared identifier 'Box'`；保留该属性后通过。
- tmpl 79 同时检查字段读写、两层类型别名、静态常量与静态方法的运行结果。
- 日志基线必须使用最新构建的编译器；重新保存后的零差异只证明可重复性，不能单独证明相对旧基线没有漂移。

## 6 演进记录

| 日期 | 变化 | 证据 |
|---|---|---|
| 2026-11-16 | 成员类模板蓝图、分层替换、非依赖逐段模板限定名 | tmpl 79 与两项成员类模板单测 |
