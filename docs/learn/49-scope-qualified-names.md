# 49. 作用域即身份：名字带上它属于哪个作用域（B38 / Stage 2）

> 前一节：learn/48（可见性交回符号表，红线 1 改写）
> 本节：把**种类**里剩下的最后一块——**作用域**——也交给 Sema 自己的事实。
> 缺陷台账：docs/BUGS.md **B38**；规格/设计：docs/specs/…spec.md、docs/designs/…design.md

## §0 定位与能力清单

作用域是名字身份的一部分 —— `N::B` 与 `::B` 是两个不同的声明、两个不同的
实体，就像 `int` 与 `double` 是两个不同的类型。minicc 修前把作用域当成
"名字外面的一层包装"，于是 `N::B<int>` 绑到全局 B、两个命名空间的同名
模板撞成同一个实例、嵌套命名空间丢外层。修法只有一句：
**前缀化 = 完整命名空间路径**，其余（注册键 / 实例名 / 缓存键 / mangling /
汇编符号）全是派生。

| 能力 | 一句话 | 状态 |
|---|---|---|
| 命名空间内声明的**全名**注册 | `m_currentNamespace` + "::" + 裸名 | 已实现 |
| 前缀化只写一遍 | 函数 / 类 / 全局变量 / 枚举 / 模板五类共用同一句 | 已实现 |
| 派生四件套 | 注册键 / 实例名 / 缓存键 / mangling 全部由前缀化派生 | 已实现 |
| 汇编符号前缀 | `N::B_int` → `N__B_int`（`symbolSafe` 净化） | 已实现 |
| 嵌套命名空间的限定名调用 `A::B::f()` | 修前报 `Undefined variable 'A::B::f'` | 已实现 |
| 同名模板跨命名空间不撞实例 | `N::B<int>` 与 `M::B<int>` 是两个实例 | 已实现 |
| 限定名查不到 ⇒ 报错**不兜底** | `no template named 'B' in namespace 'N'`，与 clang 逐字 | 已实现 |
| 名字登记表的 Sema 侧消费点退场 | 删 `m_names` / `setNameRegistry` 接线 | 已实现 |
| `using namespace N;` | 语法不支持 ⇒ 没有"非限定名回退到命名空间"这一路 | 未做 |
| 函数模板的作用域化 `N::f<int>(x)` | 仍不支持 | 未做 |
| 嵌套类 | 未做 | 未做 |
| 模板模板实参的跨作用域匹配 | 未做 | 未做 |
| 名字登记表的 **value 位**（"这个名字声明过吗"） | 恒 false 占位 | 未做（Stage 3） |
| Itanium ABI 的真 `<nested-name>` 编码（`_ZN1N1BIiE`） | 本项目用可读名 + 净化（`_Z4N__BIiE`） | 部分 |

---

## 1. 理论背景

### 1.1 [basic.lookup.qual]：限定名查找沿 DeclContext 链

限定名 `N::B` 的查找**不是**"把名字切开再猜"：编译器拿着 `N` 这个
DeclContext，在**它的成员表**里找 `B`；找不到就报错，**不退到全局的 B**。

这条规则的直接推论，也是本节的主题：

> **作用域是名字身份的一部分。**

`N::B` 与 `::B` 是两个不同的声明、两个不同的实体，就像 `int` 与 `double`
是两个不同的类型 —— 不是"同一个东西的两种写法"。

### 1.2 [temp.inst] + Itanium ABI：实例身份 = 模板 + 实参表 + **上下文**

clang 的模板实参类型（`TemplateSpecializationType`）里带着
`TemplateName` → `TemplateDecl` → `DeclContext`。同一份实参表，
在不同的 DeclContext 下是**不同的类型**：

```text
N::B<int>   ⇒  _ZN1N1BIiE      （N 是嵌套名 ⇒ N…E）
::B<int>    ⇒  _Z1BIiE
```

Itanium ABI 的 `<nested-name>` 编码（`N <source-name> … E`）就是这件事的
字面体现：**上下文被编进了符号名**。

### 1.3 clang 怎么答："这个限定名是不是模板"

```text
clang/lib/Sema/SemaDecl.cpp
  Sema::QualifiedNameLookup      ← 沿 DeclContext 链查找
  err_no_template_named          ← 诊断：no template named 'X' in namespace 'Y'

clang/lib/Sema/SemaTemplate.cpp
  Sema::LookupTemplateName       ← 模板名查找（区分类模板/函数模板/变量模板）
```

**关键**：clang 的这条路径里**没有"名字登记表"这种前向扫描的产物** ——
它的答案来自**活着的**符号表。这就是本项目与 clang 的差距所在。

## 2. 本项目怎么落地

### 2.1 修前的三处漏（同一个根因）

| 落点 | 修前 | 症状 |
|---|---|---|
| `processNamespaceDecl` 前缀化 | 用 `decl->name`（只取本层） | `A::B::f` 注册成 `B::f` ⇒ `Undefined variable 'A::B::f'` |
| 模板注册 | 走 `default:` 分支，不前缀化 | 两个命名空间的同名模板**撞成同一实例** |
| 限定名模板 id 绑定 | 「剥 :: 末段查全局蓝图」 | `N::B<int>` 绑到**全局** B |

三者合起来是一句话：**作用域没有进到名字的身份里**。

### 2.2 修后：作用域只写一遍，其余全是派生

```text
              ┌────────────────────────────────────────────┐
              │ processNamespaceDecl：全名 = m_currentNamespace │
              │              + "::" + 裸名                   │
              └──────────────────┬─────────────────────────┘
                                 │ 派生（不再各写一份）
        ┌────────────┬───────────┼───────────┬──────────────┐
     注册键       实例名       缓存键      mangling      汇编符号
   "N::B"     "N::B_int"  "N::B<int>"  "_Z4N__BIiE"  "N__B_int"
```

★ 这张表就是"判据单点"（红线 3）的收益：**作用域只在前缀化那一句出现过一次**，
下游全是派生。改规则只改一处，不会出现"定义点改了、调用点没改"的老坑
（B10 / B13~B15 / B20 / B22 都是它的变体）。

### 2.3 顺带退场：第二份作用域事实

修前，Sema 回答"限定名 `N::B` 是不是模板"靠的是**名字登记表**
（`classifyQualified`），而 Parser 回答"这个名字是不是模板"也靠同一张表
—— 同一件事，两个数据源。本批把 Sema 侧那一份删掉（Sema 自己就有注册表了），
登记表**只剩 Parser 侧的候选过滤**用途。

这一步的意义不在"少写几行"，而在**红线 1 的推进**：登记表是**过渡态**，
Stage 3（边解析边 `ActOn*` 登记）之后它可以整体退场。

## 3. clang 源码对照表

| clang 位置 | 本实现位置 | 简化了什么 |
|---|---|---|
| `clang/lib/Sema/SemaDecl.cpp` `Sema::QualifiedNameLookup`（沿 DeclContext 链） | `semantic_analyzer.cpp` `resolveType` 的模板 id 分支：按**完整限定名**查 `m_classTemplates` / `m_aliasTemplates` | 查表是 O(1) 的 map（名字已前缀化 ⇒ 键就是全名），没有真正的 DeclContext 链对象 |
| `clang/lib/Sema/SemaDecl.cpp` `err_no_template_named` | 同处 `error("no template named '{}' in namespace '{}'")` | 不做 "did you mean" 建议 |
| `clang/include/clang/Basic/TargetInfo` … `ItaniumMangle`（`<nested-name>` = `N…E`） | `template_instantiation.cpp` `NameMangler::mangleTemplateInstance` + `symbolSafe` | 编成 `_Z4N__BIiE` 而非 `_ZN1N1BIiE`（可读名 + 净化，非真 Itanium 嵌套名） |
| `clang/lib/Sema/SemaTemplate.cpp` `Sema::getTemplateName` / `LookupTemplateName` | `semantic_analyzer.cpp` `processNamespaceDecl` 的 `case NodeKind::Template` | 前缀化而非查表；注册键即全名 |
| `clang/lib/Sema/SemaDecl.cpp` `Sema::ActOnTagStartDefinition`（声明一进来就入作用域） | `processNamespaceDecl` 里集中前缀化后再 `processClassDecl` | minicc 是三趟架构，声明不是"边解析边入作用域" |

## 4. 可复现实验

```bash
# ① 两个命名空间的同名模板 ⇒ 两个实例（修前撞成一个）
cat > /tmp/t28.cpp <<'EOF'
template<class T> struct B { T v; };
namespace N { template<class T> struct B { T v; T w; }; }
namespace M { template<class T> struct B { T v; T z; }; }
int main() { N::B<int> n; n.w = 3; M::B<int> m; m.z = 5; return 0; }
EOF
./minicc /tmp/t28.cpp -o /tmp/t28 2>&1 | grep instantiate:name
#   修后：[instantiate:name] 'N::B<int>' → 汇编符号前缀 'N::B_int'
#         [instantiate:name] 'M::B<int>' → 汇编符号前缀 'M::B_int'
#   修前：两者都是 'B<int>' → 同一个 B_int ⇒ "No member 'w' in class 'B_int'"

# ② 嵌套命名空间的限定名调用
cat > /tmp/t29.cpp <<'EOF'
namespace A { namespace B { int f() { return 7; } } }
int main() { return A::B::f(); }
EOF
./minicc /tmp/t29.cpp -o /tmp/t29 && /tmp/t29; echo "rc=$?"   # 修后 rc=0（修前 rc=1）

# ③ 限定名查不到 ⇒ 与 clang 逐字
cat > /tmp/t30.cpp <<'EOF'
template<class T> struct B { T v; };
namespace N {}
int main() { N::B<int> x; return 0; }
EOF
./minicc /tmp/t30.cpp -o /tmp/t30 2>&1 | grep ERROR
#   [Semantic Error] 1:1: no template named 'B' in namespace 'N'
clang++-18 -std=c++20 -fsyntax-only /tmp/t30.cpp 2>&1 | head -1
#   error: no template named 'B' in namespace 'N'; did you mean simply 'B'?

# ④ 单测（5 例）+ 全量回归 + 基线
ctest --test-dir build-linux -R ScopeQualifiedNames -j6
ctest --test-dir build-linux -j6
./logdiff.sh diff
```

## 5. 关键过程 ASCII 图

### 5.1 三个症状 → 一个根因 → 一处修法

```text
  N::B<int> 绑到全局 B        N::B 与 M::B 撞实例        A::B::f 未定义
          │                        │                          │
          └──────────── 作用域没进名字的身份 ──────────────────┘
                                     │
                    ┌────────────────┴────────────────┐
                    │  一处修法：前缀化 = 完整命名空间路径 │
                    │  （函数/类/全局变量/枚举/模板共用） │
                    └────────────────┬────────────────┘
                                     │
        注册键 / 实例名 / 缓存键 / mangling / 汇编符号 全部派生
```

### 5.2 限定名绑定的两条路径（修后只剩一条）

```text
   使用点 "N::B<int>"
          │
          ▼
   ┌──────────────────────────────┐
   │ 查 m_classTemplates["N::B"] │
   │ 查 m_aliasTemplates["N::B"] │
   └──────────────┬───────────────┘
          命中 │            │ 未命中
               ▼            ▼
        用它（不剥尾）   error: no template named 'B' in namespace 'N'
```

## 6. 边界与后续

**本批明确不做**
1. `using namespace N;`（本项目不支持该语法 ⇒ 没有"非限定名回退到命名空间"这一路）；
2. 函数模板的作用域化（`N::f<int>(x)` 仍不支持）；
3. 嵌套类；模板模板实参的跨作用域匹配。

**Stage 3（下一步）**：解析期"名字是不是模板"这一问仍由登记表回答（过渡态近似）。
clang 的答案是 Sema 的活符号表（`Actions.isTemplateName`）+ 语法试探兜底
（`isTemplateArgumentList` + `RevertingTentativeParsingAction`），
需要边解析边 `ActOn*` 登记的架构改造。

★ 顺带记下一条**还没做但已定位**的债：clang 对**未声明**的名字会按 C++20 规则
当作 template-id 解释（探针：`unknown<x>(y)` ⇒ clang 报 "use of undeclared
identifier 'x'"），而 minicc 走比较链 ⇒ 报 "Undefined variable 'unknown'"。
两者的分水岭是「这个名字**声明过**吗」—— 登记表至今没有 **value 位**
（value 按拍板恒 false 占位），因为"声明过"要靠真解析才判得准。
这正是 Stage 3 要连同 value 位一起解决的那一问。

## 7. 自检清单（这一批学到了什么）

- [x] **作用域是身份的一部分**，不是名字外面的一层包装 ——
      注册键 / 实例名 / 缓存键 / mangling 四处必须同源，否则撞名只在链接期炸。
- [x] **"名字长得像"与"名字是谁"是两回事** —— `N::B` 与 `::B` 语法同形，
      只有作用域能区分；靠"剥末段查全局"来兜底是把两个实体当成一个。
- [x] **修法要落在【前缀化】那一处**，其余全是派生 —— 判据单点的收益。
- [x] **收窄方向也要敢做**：旧行为"宽"（`N::B` 默默绑全局 B）看着安全，
      实则静默算错；本批把它改成**报错**，与 clang 逐字。
- [x] **突变验证要逐条实跑** —— 4 条突变分别打红 1/3/1/1 个用例，
      证明判据真的被测住了（而不是"看起来有测试"）。
- [x] **基线会被过期二进制骗** —— 上一版基线里的 369 是没重建的 unit_tests
      留下的假象（learn/47 §6 的第三种失败面具，这回连基线也骗了）。

---

## 8. 真实版多做了什么、为什么

素材来自 §3 对照表的「简化了什么」列与 §6 的边界清单。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `Sema::QualifiedNameLookup` 沿 **DeclContext 链**逐级查找 | 本实现查表是 O(1) 的 map（名字已前缀化 ⇒ 键就是全名），没有真正的 DeclContext 链对象 |
| `err_no_template_named` 附 **"did you mean simply 'B'?"** 建议 | 建议属诊断体系；先保证错误文案逐字对齐，建议留待诊断体系成型 |
| Itanium ABI 的 `<nested-name>`（`N…E`）真嵌套名编码 | 本项目用可读名 + `symbolSafe` 净化（`_Z4N__BIiE`），教学上更易读；真编码只在"要求与 clang 逐字符相同"时才必要 |
| `Sema::ActOnTagStartDefinition`（声明一进来就入作用域） | minicc 是三趟架构，声明不是"边解析边入作用域"；Stage 3 才谈得上 |
| `Sema::getTemplateName` / `LookupTemplateName` 查**活符号表** | 本实现是"前缀化而非查表"：注册键即全名，改规则只改一处 |
| clang 对**未声明**的名字按 C++20 规则当 template-id 解释（探针：`unknown<x>(y)` ⇒ 报 "use of undeclared identifier 'x'"） | 本项目走比较链 ⇒ 报 `Undefined variable 'unknown'`；分水岭是"这个名字**声明过**吗" —— 登记表至今没有 value 位（Stage 3） |

---

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-11-02 | 落地 B38：`processNamespaceDecl` 的前缀化改用 `m_currentNamespace`（完整路径），函数 / 类 / 全局变量 / 枚举 / 模板共用同一句；限定名不再剥尾，查不到即报错；删掉 `SemanticAnalyzer::m_names` / `setNameRegistry` 接线 | 三个症状（限定名绑全局 / 同名模板撞实例 / 嵌套命名空间丢外层）同一个根因；旧行为"默默绑全局 B"看着安全、实则静默算错 —— 由宽改严（git 4f1fb49） |
