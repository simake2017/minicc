# 27 · 别名模板：只是名字，不是类型

> 一句话：`template<class T> using Vec = MyPtr<T>;` 里的 `Vec` **不是模板**，
> 它是"给一族类型起的一个共用名"。所以 `Vec<int>` 没有"实例化"这一步 ——
> 只有**解糖（desugar）**：把形参换掉，得到的是 `MyPtr<int>` **本身**，
> 不是"另一个碰巧长得像的类型"。不造新类、不发新符号。
>
> 但也正因为"只是名字"，它有三个地方必须各自解一次，漏一个就出错，
> 而且往往**不报错、只是静默选错或静默不匹配**。

---

## §0 定位与能力清单

别名模板 `template<class T> using Vec = MyPtr<T>;` 里的 `Vec` **不是模板**，
它是"给一族类型起的一个共用名"。所以 `Vec<int>` 没有"实例化"这一步 —— 只有
**解糖（desugar）**：把形参换掉，得到的是 `MyPtr<int>` **本身**，不是"另一个碰巧
长得像的类型"。不造新类、不发新符号。也正因为"只是名字"，它在**三个时机**各要解一次，
漏一个就出错，而且往往**不报错、只是静默选错或静默不匹配**。

| 能力 | 一句话 | 状态 |
|---|---|---|
| 别名模板的解析与登记 | `parseTemplateDecl` 分派 `using`；单独一张 `m_aliasTemplates` 表，不混进类模板表 | 已实现 |
| 别名**不是新类型**（[temp.alias]/1） | `Vec<int>` 与 `MyPtr<int>` 是同一个类型、同一份 mangling | 已实现 |
| 解糖点①：使用点（非依赖） | `resolveType` 的模板 id 分支，排在类模板分支**之前** | 已实现 |
| 解糖点②：替换期（依赖，直接上下文） | `substituteType` Case 5.8；解糖失败即 `Sfinae::fail` 软失败 | 已实现 |
| 解糖点③：推导期（合一之前） | `TemplateDeducer::desugarAlias` —— 漏了**不报错**，只是候选被静默剔除 | 已实现 |
| 递归别名检测 | `template<class T> using X = X<T>;` 展开时检出并报错 | 已实现（**使用期**才报） |
| 实参个数 / 形态校验（[temp.alias]/2） | `Vec<int,int>` 报个数错 | 已实现 |
| 类模板 id 的**结构合一**（[temp.deduct.type]/8 的 `T<T1...>`） | `MyPtr<T>` vs `MyPtr_int` 逐位合一 | 已实现 |
| 实例"出身"记录 | `Type::templateOriginName` / `templateOriginArgs` | 已实现 |
| NTTP 名在实参位被误判为类型 | 替换期按"表里绑的是值"还原成值实参 | 已实现 |
| 别名模板作**模板模板实参** | `Wrap<Alias>` 位宽/kind 取别名自身 | 已验证（learn/33） |
| `std::enable_if_t`（**std 版**） | 需要"条件选型" + std 名字注入 | 部分（机制已通：用户自写的 `enable_if` + `enable_if_t` 别名模板端到端可用；`std::` 前缀的垫片未注入） |
| 别名模板的**偏特化** | `template<class T> using X<T*> = ...` | 未做 |
| 递归别名的**定义期**报错 | 标准要求定义期即报 | 未做（本项目在使用期才报） |

> ★ §5「边界」是**成文时的快照**，本表按**当前**状态填写，差异在演进记录里逐条列出。

## 1. 理论背景

### 1.1 别名 vs 类模板：一张尺子量到底

|  | 类模板 `MyPtr<T>` | 别名模板 `Vec<T>` |
|---|---|---|
| 标准条款 | [temp.class] | [temp.alias] |
| 产物 | **新类型** `MyPtr_int` | 既有类型 `MyPtr_int` |
| 做什么 | 深拷贝蓝图 + 替换 + 登记符号 | 只替换，然后**把名字扔掉** |
| 有缓存吗 | 有（同实参复用同一实例） | 不需要（解糖是纯函数） |
| 占汇编符号吗 | 占（`_Z5MyPtrIiE`） | **不占** |
| 能特化吗 | 能（全/偏特化） | 不能（要偏特化只能再写一个别名模板） |
| 能重载/区分吗 | 能 | **不能** —— 与底层类型完全等价 |

关键那句在 [temp.alias]/1：

> A template-declaration in which the declaration is an alias-declaration
> declares the identifier to be an alias template. **The type-id in an
> alias-declaration is the type denoted by the alias** — 别名所指代的
> 就是那个类型本身。

所以别名**不引入新类型**，只是既有类型的另一个拼写。

```
        Vec<int>            MyPtr<int>
            \                  /
             \                /
              ⇒  同一个类型  ⇐
           （mangling 都是 _Z5MyPtrIiE，绝不可能靠它区分重载）
```

### 1.2 为什么"只是名字"反而更难做：三个解糖点

一个名字被扔掉之前，编译器在三个不同的**时机**会遇到它，每个时机的
能力集都不一样：

```
 ① 使用点（非依赖）        ② 替换期（依赖，直接上下文）     ③ 推导期（合一之前）
 ─────────────────         ──────────────────────────      ────────────────────
 Vec<int> v;               template<class T>                template<class T>
                               void f(Vec<T> v);                T g(Vec<T>);
 Sema::resolveType         实例化 f<int> 时，参数位           调用 g(x)，x 是 MyPtr_int
 实参全具体 ⇒ 立刻解糖      T 已绑 ⇒ 当场解糖                 P 侧是 Vec<T>，A 侧是 MyPtr_int
                                                             ⇒ 不解糖就【名字不等】⇒ 候选被剔除
 落点：                      落点：                            落点：
 resolveType 的模板 id 分支   substituteType Case 5.8          TemplateDeducer::desugarAlias
```

第 ③ 点是**最容易漏**的一环，因为漏了不会报错：
合一失败在重载决议里表现为"这个候选不成立"（SFINAE 的正常行为），
最终只在调用点冒出一句 `no matching function`，根因被吞掉了。

### 1.3 依赖上下文里为什么必须"当场"解

```cpp
template<class T> void f(Vec<T> v);   // 定义期：T 未知，Vec<T> 保持依赖
```

定义期解不动，是因为实参还不存在。等调用点拿到 `MyPtr<int>` 之后，
替换发生的那一刻就是 [temp.deduct]/8 说的**直接上下文**：

- 解糖**成功** ⇒ 该绑定成立；
- 解糖**失败**（例如别名的底层类型要求 `T::type` 而 `int` 没有）⇒
  必须当场抛 `SubstitutionFailure`，被 `Sfinae::attempt` 吸收成
  "这个候选不成立"。**拖到后面就变成硬错误**，SFINAE 整条链失效。

### 1.4 顺带补齐的两块拼图

做别名模板时踩出两个此前一直缺的洞 —— 它们与别名无关，但**没有它们，
别名就只能"写出来好看"，一放进函数模板形参就废**：

**(a) 类模板 id 的结构合一** —— [temp.deduct.type]/8 的 `T<T1...>` 情形。
`deducePair` 此前只处理 `T` / `T*` / `T&` / `T&&` / `const T`，
遇到 `MyPtr<T>` 这种"带实参的类模板 id"直接落到"P 非依赖 ⇒ 要求恒等"
的兜底分支，于是 `pStr != aStr` 一律判失败。

```cpp
// 新增分支：同类模板 + 实参个数相同 ⇒ 逐位递归合一
P = MyPtr<T>   A = MyPtr_int
  出身 MyPtr，实参 [int]
  第 1 位：T ↔ int  ⇒  T := int ✓
```

**(b) 实例"出身"记录** —— 这条是被 minicc 自己的实现方式逼出来的：
实例化后的类型名被**改写**成了 `MyPtr_int`（可读串，不是单射），
模板 id 时代的信息（哪个模板、哪些实参）在改名那一刻就丢了。
clang 不存在这个问题 —— 它的实例类型本身还是一个
`ClassTemplateSpecializationDecl`，实参表一直挂在身上。

```cpp
// include/type.h —— 实例类型专有的只读记录
std::string              templateOriginName;   // "MyPtr"
std::vector<TemplateArg> templateOriginArgs;   // [Type:int]
bool isTemplateInstance() const { return !templateOriginName.empty(); }
```

★ 为什么**不**复用现成的 `templateArgs` 字段：那个字段的语义是
"**待实例化的半成品**" —— `resolveType` 一见到非空 `templateArgs`
就去触发实例化。实例类型若也填它，每次解析都会重走一遍实例化分支。
出身字段是纯只读记录，不参与任何解析决策。

---

## 2. 数据流（ASCII 图）

```
源码   template<class T> using Vec = MyPtr<T>;
       Vec<int> v;
       v.value = 7;

── 解析 ──────────────────────────────────────────────────────────────
  parseTemplateDecl
    ├─ 读形参表 <class T>            → templateParams = [T:Type]
    ├─ 分派：下一个 Token 是 KwUsing ⇒ parseTypeAliasDecl
    └─ aliasTemplate = TypeAliasDecl{aliasName="Vec", underlyingType=MyPtr<T>}
                      （underlyingType 里的 T 是 TemplateParam 节点 —— 
                        解析时 T 已在模板形参作用域里）

── 注册（Sema Pass 1）────────────────────────────────────────────────
  processTemplateDecl
    └─ m_aliasTemplates["Vec"] = 蓝图
       ★ 单独一张表，不混进 m_classTemplates：
         类模板要"实例化"(造新类/发新符号)，别名只要"替换"(解糖)

── 使用点解糖（Sema Pass 3）──────────────────────────────────────────
  resolveType(Class("Vec", args=[int]))
    ├─ 先问 m_aliasTemplates：命中！
    └─ expandAliasTemplate("Vec", [int])
         ├─ 递归防护：Vec 已在展开中？→ 报 recursive alias template
         ├─ 实参个数/形态校验（[temp.alias]/2）
         ├─ 绑定 subst {T := int}
         └─ m_instantiator.substituteType(MyPtr<T>, {T:=int})
              ├─ Case 5.2  模板 id 实参替换：MyPtr<T> → MyPtr<int>
              └─ （Case 5.2 已无别名可解，返回重建结果）
            ⇒ resolveType(MyPtr<int>)
                 └─ m_classTemplates 命中 ⇒ getOrInstantiateClass
                      ⇒ MyPtr_int（记下出身 MyPtr + [int]）
    ⇒ 返回 MyPtr_int —— 与手写 MyPtr<int> 走的是【同一条路】，零分叉
```

依赖上下文（`Vec<T>` 留在模板体里）走的是同一个 `expandAliasTemplate`，
只是触发者从 `resolveType` 换成 `substituteType` 的 **Case 5.8**：

```
  实例化 W<int>，字段类型 Vec<T>
    substituteType(Class("Vec",[TemplateParam T]), {T:=int})
      ├─ Case 5.2：实参 T → int，重建为 Class("Vec",[int])
      └─ Case 5.8：别名解糖 → expandAliasTemplate("Vec",[int]) → MyPtr_int
```

推导侧则是第三处：

```
  g(x)  x : MyPtr_int
    TemplateDeducer::deduce
      └─ 逐位：P = 形参类型
           desugarAlias(Vec<T>, subst)  →  MyPtr<T>
             （只替换、不实例化 —— T 仍是形参，正好交给合一去绑）
           deducePair(MyPtr<T>, MyPtr_int)
             └─ 类模板 id 结构合一 ⇒ T := int ✓
```

---

## 3. 实现改动清单

| 位置 | 改动 |
|---|---|
| `include/ast.h` | `TemplateDecl::aliasTemplate` + `isAliasTemplate()`；`templateName()` 三分支 |
| `src/parser.cpp` | `parseTemplateDecl` 在 `template<...>` 之后分派 `using` → 复用 `parseTypeAliasDecl` |
| `include/template_instantiation.h` | 新接口 `AliasTemplateResolver`（`isAliasTemplate` / `expandAliasTemplate`）+ setter |
| `src/template_instantiation.cpp` | Case 5.2（模板 id 实参替换 + NTTP 名还原）、Case 5.8（别名解糖） |
| `include/template_deduction.h` / `src/template_deduction.cpp` | `desugarAlias()`；`deducePair` 开头的解糖；`deduce()` 逐位解糖；新增类模板 id 结构合一分支 |
| `include/type.h` | `templateOriginName` / `templateOriginArgs` / `isTemplateInstance()` |
| `include/semantic_analyzer.h` / `src/semantic_analyzer.cpp` | `m_aliasTemplates` 注册表；`expandAliasTemplate` / `isAliasTemplate`；`resolveType` 别名分支（排在类模板分支**之前**）；`containsTemplateParam` 依赖守卫；`getOrInstantiateClass` 写"出身" |
| `src/main.cpp` | Phase 4 演示循环跳过别名模板（`classTemplate` 为空，不跳过会空指针崩） |

### 3.1 为什么 `AliasTemplateResolver` 是第三个同形状的接口

`DecltypeEvaluator`、`MemberTypeResolver`、`AliasTemplateResolver`
是同一个分层问题的三次出现：

```
   需要"能力"的地方              拥有"能力"的地方
   ─────────────────             ─────────────────
   TemplateInstantiator          SemanticAnalyzer
   TemplateDeducer                （类表 / 别名表 / 按需实例化）
        │                              │
        └──── 持抽象接口指针 ──────────┘
              （Sema 构造时注入自己）
```

若让 Instantiator 直接持有 Sema，`Sema ⇄ Instantiator` 立刻变成双向依赖。
对照 clang：不需要这层，因为 clang 的 TreeTransform **本身就派生自 Sema**。
本项目为保住分层与可单测性把它外提 —— 代价是多三个接口。

### 3.2 一个被踩出来的静默错误：`Box<T>` 被当成 `Box_T` 实例化

加 `containsTemplateParam` 守卫之前，`template<class T> struct W { Box<T> b; };`
的字段类型 `Box<T>` 会**真的去实例化**，实参是 `TemplateParam(T)` 本身，
造出一个名叫 `Box_T` 的假实例 —— 它的字段类型是 `T`、方法返回 `T`。
不报错，只是后面全线对不上。

守卫写起来很短，难的是想到要写它：

```cpp
// 实参里还挂着未绑定的模板形参 ⇒ 保持依赖，别实例化
if (containsTemplateParam(type)) return type;
```

对照 clang：`Sema::CheckTemplateIdType` 遇到依赖实参会建成
`TemplateSpecializationType`（带 sugar 的依赖类型），而不是实例化。

### 3.3 NTTP 名在实参位被误判为类型：在替换阶段还原

```cpp
template<int N> struct Buf { int a; };
template<int N> using BufA = Buf<N>;    // ← 这里的 N
```

Parser 的实参分流只看 **Token 形态**（`parseTemplateArgumentList`）：
整数字面量 → 值实参，其余 → `parseType`。而 `N` 是标识符，于是被建成
`Class("N")` 类型节点，替换时就撞上
`non-type template parameter 'N' is used as a type`。

**凭什么能还原**：替换表是唯一权威 —— `N` 在表里绑的是**值**，
而**类型位置的实参不可能绑值**。所以这里的裸 `N` 只能是值实参。
Parser 缺的是"这个作用域里 N 是值"的知识（它的 `m_templateParamScope`
只压类型形参），替换阶段恰好有。

对照 clang：Parser 维护 `TemplateParameterScope`，非类型形参在实参位
直接走常量表达式解析，不存在这一步还原。

---

## 4. 可复现实验

```bash
# ① 端到端（期望 173；clang++-18 -std=c++20 同样 173）
./minicc tests/tmpl/test_tmpl_50_alias_templates.cpp -o /tmp/t50 && /tmp/t50; echo $?

# ② 看三级解糖各自的日志
./minicc tests/tmpl/test_tmpl_50_alias_templates.cpp -S 2>&1 \
  | grep -E "parse:template.*alias|alias template .* registered|sema:alias|\[alias\]|deduce:alias"

# ③ 最小复现：使用点解糖（非依赖）
cat > /tmp/at1.cpp <<'EOF'
template<class T> struct MyPtr { T value; };
template<class T> using Vec = MyPtr<T>;
int main() { Vec<int> v; v.value = 7; return v.value; }
EOF
./minicc /tmp/at1.cpp -o /tmp/at1 && /tmp/at1; echo "期望 7"

# ④ 推导侧解糖（缺 desugarAlias 就会在这里报 no matching function）
cat > /tmp/at2.cpp <<'EOF'
template<class T> struct MyPtr { T value; };
template<class T> using Vec = MyPtr<T>;
template<class T> T readAt(Vec<T> v) { return v.value; }
int main() { Vec<int> v; v.value = 7; return readAt(v); }
EOF
./minicc /tmp/at2.cpp -o /tmp/at2 && /tmp/at2; echo "期望 7"

# ⑤ 错误用例（三条，各自对应一条规则）
printf 'template<class T> struct P{T v;};\ntemplate<class T> using V=P<T>;\nint main(){V v;return 0;}\n' > /tmp/ae1.cpp
./minicc /tmp/ae1.cpp 2>&1 | grep ERROR      # 裸别名模板名 ⇒ 需要实参
printf 'template<class T> struct P{T v;};\ntemplate<class T> using V=P<T>;\nint main(){V<int,int> v;return 0;}\n' > /tmp/ae2.cpp
./minicc /tmp/ae2.cpp 2>&1 | grep ERROR      # 实参个数不符（[temp.alias]/2）
printf 'template<class T> using X = X<T>;\nint main(){X<int> x;return 0;}\n' > /tmp/ae3.cpp
./minicc /tmp/ae3.cpp 2>&1 | grep ERROR      # 递归别名

# ⑥ 全量回归
ctest --test-dir build-linux            # 期望 154/154
python3 /tmp/a21/cmpall.py              # 与旧版本逐文件对照
python3 /tmp/a21/cmpplang.py            # 与 clang++-18 退出码对照（期望 0 处不一致）
```

对照 clang 的语义 oracle：

```bash
clang++-18 -std=c++20 -o /tmp/t50c tests/tmpl/test_tmpl_50_alias_templates.cpp
/tmp/t50c; echo $?     # 173
```

---

## 5. 边界（本项目**未做**的）

| 未做 | 为什么 | clang 的行为 |
|---|---|---|
| `std::enable_if_t` | 需要**条件选型**（按常量条件在两种类型间二选一），本实现的别名底层类型只能是静态表达式，没有"cond ? X : Y"的类型级形式 | 真 <type_traits> |
| 别名模板的偏特化 | 标准要求写成另一个别名模板（`template<class T> using X<T*> = ...`），涉及别名模板的局部特化语法 | 支持 |
| 别名模板作模板模板实参 | `template<template<class> class C>` 未实现 | 支持 |
| 递归别名的**定义期**报错 | 本项目在**使用期**才报（展开时检测）。定义期查需要"别名模板不允许直接/间接引用自身"的静态分析 | 定义期即报 |

已验证的行为一致性：`Vec<int>` 与 `MyPtr<int>` 传参互通、
`Vec<int,int>` 报个数错、`V v;`（裸名）报需要实参、递归别名报错、
NTTP 别名 `BufA<4>` 与 `Buf<4>` 等价 —— 五条均与 clang 结论一致
（错误文案不同，但"是否拒绝"一致）。

---

## 6. clang 源码对照表

| clang 位置（文件:函数） | 本实现位置 | 简化了什么 |
|---|---|---|
| `Sema::ActOnAliasDeclaration` / `TypeAliasTemplateDecl` | `parseTemplateDecl` 的 `KwUsing` 分支 + `TemplateDecl::aliasTemplate` | 不建独立 Decl，塞进 `TemplateDecl` 的一个字段（类/函数/别名三选一） |
| `Sema::CheckAliasTemplateId`（实参个数/形态校验） | `expandAliasTemplate` 开头的两道校验 | 走 `Sfinae::fail` 而非专用诊断，因为在直接上下文里 |
| `Sema::SubstType` 对 `TemplateSpecializationType` 判 `isTypeAlias()` 后解糖 | `substituteType` Case 5.8 | clang 保留 `AliasTemplateSpecializationType` 这层 sugar 供诊断；本实现直接扔掉名字 |
| `Type::getCanonicalType`（比类型前先剥别名） | `TemplateDeducer::desugarAlias` | 只在推导入口剥，不是全类型系统的规范化 |
| `TreeTransform::TransformTemplateSpecializationType`（实参逐个 Transform） | `substituteType` Case 5.2 | 无 sugar 重建、无推导指引参与 |
| `ClassTemplateSpecializationDecl` 自带 `TemplateArgumentList` | `Type::templateOriginName` / `templateOriginArgs` | 本项目实例类型是**改名后的 Type**，只能额外挂一份出身记录 |
| `Parser::ParseTemplateArgumentList` + `TemplateParameterScope` | `parseTemplateArgumentList` + 替换期的"值实参还原"（§3.3） | Parser 只认 Token 形态，NTTP 名的判定推迟到替换阶段 |

## 6 之补 真实版多做了什么、为什么

（把 §3.1~§3.3 与 §6 对照表第三列、§5 边界表合并成一张两列表。）

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| 别名模板有**独立的 Decl**（`TypeAliasTemplateDecl`） | 本项目不建独立 Decl，塞进 `TemplateDecl` 的一个字段（类 / 函数 / 别名三选一）—— 保持 AST 扁平 |
| 保留 `AliasTemplateSpecializationType` 这层 **sugar** 供诊断 | 本项目**直接扔掉名字**：解糖后只剩底层类型；代价是诊断里看不到"原来是别名" |
| `Type::getCanonicalType` 在**全类型系统**里先剥别名 | 本项目只在**推导入口**剥（`desugarAlias`）—— 不是规范化，是"名字不等 ⇒ 候选被剔除"的修正 |
| `Sema::CheckAliasTemplateId` 有**专用诊断** | 本项目走 `Sfinae::fail` 而非专用诊断 —— 因为失败发生在直接上下文里，必须是软失败 |
| `TreeTransform::TransformTemplateSpecializationType` 做 sugar **重建** | 本项目无 sugar 重建、无推导指引参与 |
| 实例类型自带 `TemplateArgumentList` | 本项目的实例类型是**改名后的 Type**（`MyPtr_int`），只能额外挂一份"出身"记录（`templateOriginName` / `templateOriginArgs`） |
| `Parser::ParseTemplateArgumentList` 配 `TemplateParameterScope`，非类型形参在实参位**直接**走常量表达式解析 | 本项目 Parser 只认 Token 形态 ⇒ `N` 被建成 `Class("N")`；还原推迟到替换阶段（依据是"替换表里绑的是值"） |
| `Sema::CheckTemplateIdType` 遇依赖实参建 `TemplateSpecializationType`（带 sugar 的依赖类型） | 本项目用 `containsTemplateParam` 守卫**保持依赖、不去实例化** —— 否则会造出假实例 `Box_T`，不报错只是全线算错 |
| 别名模板的**偏特化**（写成另一个别名模板） | 未做 —— 涉及别名模板的局部特化语法，是独立的一条线 |
| 递归别名的**定义期**静态分析 | 本项目在**使用期**（展开时）才检出 —— 定义期查需要"别名模板不允许直接 / 间接引用自身"的静态分析 |
| clang 的 TreeTransform **派生自 Sema** | 本项目为保住分层与可单测性把它外提 ⇒ 多三个同形状的接口（`DecltypeEvaluator` / `MemberTypeResolver` / `AliasTemplateResolver`） |


---

## 7. 关键日志片段（`test_tmpl_50` 实跑）

```
  [parse:template]   parsing alias template...
  [parse:template] ◀ alias template 'Vec' with 1 parameter(s) stored as blueprint
  [register] template <typename T> using Vec = MyPtr<T> (alias blueprint stored,
             expands by substitution only)
    ↳ alias template 'Vec' registered
  [sema:alias] 遇到别名模板 id Vec<...> ⇒ 解糖
  [alias] 展开别名模板 Vec → MyPtr<T>，替换表 {T := int}
    [subst] ★ TemplateParam 'T' → 'int' (direct replacement)
    [subst] ★ 模板 id 实参替换: MyPtr<T> → MyPtr<int>
    [subst] ★ 别名模板 Vec<...> 解糖 ⇒ MyPtr_int
  [alias] ★ Vec<...> 解糖 ⇒ MyPtr_int
  ...
  [deduction] ▶ readAt — 模板参数 <T>，实参 1 个
  [deduction]   形参位 Vec<T> 解糖 ⇒ MyPtr<T>
  [deduction]   P=MyPtr<T>     A=MyPtr_int    ⇒ 同类模板 MyPtr（出身 MyPtr），逐位合一
  [deduction]   P=T            A=int          ⇒ T := int
  [deduction] ◀ 推导成功: <T=int>
```

最后两行就是 **§1.4(a)** 那块拼图落地时的样子 ——
`Vec<T>` 与 `MyPtr_int` 能对上，靠的正是"先解糖、再按出身逐位合一"。

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-09-16 | 模板机制主线收官（commit 5d9b78d）：别名模板的解析 / 登记 / 三级解糖 + `AliasTemplateResolver` 接口 | 别名不是新类型 ⇒ 没有"实例化"只有"解糖"；但三个解糖点缺一不可 |
| 2026-09-16 | 顺带补齐两块拼图：类模板 id 的**结构合一**（[temp.deduct.type]/8）与实例"出身"记录 | ①漏了 ⇒ `MyPtr<T>` vs `MyPtr_int` 名字不等、候选被静默剔除；②minicc 的实例类型是**改名后的 Type**，模板 id 的信息在改名那刻就丢了 |
| 2026-09-16 | 加 `containsTemplateParam` 守卫 | 否则 `Box<T>` 会拿形参 T 当实参真的去实例化，造出假实例 `Box_T` —— 不报错只是全线算错 |
| 2026-09-16 | NTTP 名在实参位被误判为类型：按"替换表里绑的是值"在替换阶段还原 | Parser 缺的是"这个作用域里 N 是值"的知识，替换阶段恰好有 ⇒ `template<int N> using BufA = Buf<N>;` 由此可用 |
| 2026-09-16 | 学习文档 18..29 一次性成文（commit 582887c） | — |
| 2026-09-22 | NTTP 值位参与偏特化模式（learn/32，commit e0ff653） | ★ **补上了 §5 第一行的地基**：值位必须真的比，`enable_if<true,T>` / `enable_if<false,T>` 才能一真一假地分流 |
| 2026-09-28 | 别名模板作模板模板实参获验证（learn/33，commit 09f50e5） | 形参表取别名自身、替换期正常解糖 ⇒ §5 第三行由"未做"改为"已验证" |

