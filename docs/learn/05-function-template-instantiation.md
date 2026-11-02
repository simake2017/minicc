# 05 函数模板实例化（S5）

> 子阶段 S5：推导结果 → 克隆蓝图 → 替换 T → mangled 符号 → 注册 codegen。

## §0 定位与能力清单

推导给出 `T := int` 之后，真正的函数**还不存在**——蓝图只是一张类型模式。
本文讲的是"把蓝图变成真函数"的那一步：克隆蓝图子树、把 `T` 替换成 `int`、
算出一个全局唯一的符号、注册给 CodeGen，让 `twice(21)` 的调用点能指到它。
这一步也是**两阶段查找的第二阶段**的落点——模板体里那些依赖 `T` 的名字，
直到这里才第一次被真正解析。

| 能力 | 一句话 | 状态 |
|---|---|---|
| [temp.inst] 隐式实例化 | 函数模板在被调用（ODR-used）时才实例化，与类模板一致 | 已实现 |
| 克隆蓝图 + 替换 | 复用类模板的 `cloneMethod` 引擎，`T → int` 一次替换到位 | 已实现 |
| [temp.subst] 替换分两层 | 类型位置走 `substituteType`、表达式位置走 `cloneExpr` | 已实现 |
| [dcl.ref]/6 引用折叠 | 替换期把 `T& &` / `T&& &` 等折成 `T&` | 已实现 |
| 符号唯一性 | `twice<int>` 与 `twice<double>` 产出不同符号，调用点被重写为该符号 | 已实现 |
| Itanium ABI 的尾部参数编码 | 函数模板实例把返回类型与形参类型编进符号尾部（`_Z5twiceIiET_T_`） | 已实现（2026-09-22 补齐，与 clang 逐字符一致） |
| 实例缓存 | 同一组实参只实例化一次（以无损实参键为键） | 已实现 |
| 两阶段查找第二阶段 | 实例化后立刻对函数体做完整语义分析 | 已实现 |
| [temp.expl.spec] 函数模板显式特化 | `template<> void f<int>(int) {}` | 未做（类模板的全特化/偏特化已做，见 learn/19） |
| 显式实例化声明 / 定义 | `template void f<int>(int);` | 未做 |
| 实例化深度限制诊断 | 递归实例化的层数上限与对应诊断 | 未做 |
| 实例点的有序规则（POI） | clang 规定"实例点"顺序以消除二义 | 未做 |
| 模板实参的链接期语义 | 实例的 ODR 合并、弱符号 | 未做 |

## ① 理论背景

- **隐式实例化**（[temp.inst]）：函数模板在**被调用（ODR-used）时**才实例化，
  与类模板的"使用点实例化"一致。推导出的实参就是实例化的输入。
- **两阶段名称查找的第二阶段**：蓝图存储期（S1）只登记不检查；
  实例化时函数体在**具体类型**下做完整的语义分析
  （本项目：对实例调用 `analyzeFunctionBody`）。
- **符号唯一性**：`twice<int>` 与 `twice<double>` 必须生成不同符号
  （_Z5twiceIiE / _Z5twiceIdE），调用点重写为该符号，与普通函数调用同构。
- **实例化缓存**：同一组实参只实例化一次（clang 用 SpecializationDecl 去重）。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 复用类模板克隆引擎 | `TemplateInstantiator::cloneMethod(blueprint, subst, "")` | 替换规则（含引用折叠）与类方法完全一致，零重复代码 |
| 符号命名 | `NameMangler::mangleTemplateInstance(name, args)` → `_Z5twiceIiE` | 与类实例 mangling 同构；真实 ABI 还带尾部参数编码，教学版省略 |
| callee 重写 | sema 把 VarExpr 名字改成 mangled 符号 | codegen 的 `callq <name>` 无需改造，最小侵入 |
| 缓存键 | mangled 名 → 实例 | 天然唯一 |
| 实例体检查 | 实例化后立即 `analyzeFunctionBody(instance)` | 依赖名在此阶段才解析（两阶段查找落地） |

## ③ clang 对照

| 本实现 | clang | 简化 |
|---|---|---|
| instantiateFunction | `SemaTemplateInstantiate.cpp → TemplateInstantiator`（TreeTransform 子类，18k 行） | clang 在 AST 上"原地变换"而非深拷贝；本项目深拷贝更直观 |
| 实例注册/去重 | `FunctionTemplateSpecializationInfo` + ASTContext 去重 | 无 explicit instantiation 声明、无实例化深度限制诊断 |
| 调用点重写 | CodeGen 层直接引用 specialization decl | 本项目改 AST 名字，更"看得见" |

## ④ 实验

```bash
./build-linux/minicc tests/test_tmpl_14_deduce_basic.cpp -o /tmp/t14.s
# 日志关注：Function Template Instantiation (S5) 框、Substitution map、Symbol 行
grep -n "_Z5twiceIiE" /tmp/t14.s          # 定义与调用点都用 mangled 符号
gcc /tmp/t14.s -o /tmp/t14 && /tmp/t14; echo $?   # 0
```

## ⑤ 过程图

```
deducedArgs [int]
    ▼
instantiateFunction(twice, [int])
    subst = {T → int}
    cloneMethod: returnType T→int, param T x→int x, body x+x 类型随之具体化
    mangledName = _Z5twiceIiE
    ▼
sema: cache[mangled] = instance
      m_functions += instance        → codegen 发射函数体
      analyzeFunctionBody(instance)  → 两阶段查找第二阶段
    ▼
inferCall: calleeVar->name = "_Z5twiceIiE"   → callq 闭环
```

## ⑥ 真实版多做了什么、为什么

★ 素材来源：本文 §③ 对照的「简化」列 + `docs/BUGS.md` B18/B20/B22
+ CLAUDE.md「未做」清单。**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `clang/lib/Sema/SemaTemplateInstantiate.cpp` 的 `TemplateInstantiator`（`TreeTransform` 子类，上万行）——在 **AST 上原地变换** | 本项目走**深拷贝**路线（`cloneMethod` / `cloneExpr` / `cloneStmt` 手写）：蓝图要能反复用、实例要能独立改，深拷贝比原地变换更好讲。代价是类型节点靠 `shared_ptr` 共享而非 clang 的 `ASTContext` 唯一化 |
| `FunctionTemplateSpecializationInfo` + `ASTContext` 的实例去重 | 本项目用**缓存表**（无损实参键 → 实例）达到同样的"只实例化一次"；键必须单射（`losslessArgumentsKey`），否则 `Buf<4>` 与 `Buf<4L>` 会静默撞键（learn/35） |
| 显式实例化声明 / 定义（`extern template`） | 未做：它们的价值在**跨翻译单元避免重复实例化**，教学编译器是单 TU 的 |
| 实例化深度限制与"实例化回溯"诊断 | 未做：本项目的实例化是单趟克隆，没有回溯 ⇒ 也不需要回溯诊断 |
| 实例点（POI）的有序规则 | 未做：clang 要给"同一个模板在两个实例点被实例化"定顺序以消除二义；本项目每个实例只在一处产生 |
| `TreeTransform` 对**依赖名**的延迟处理 | 本项目在实例化后才整体走一遍语义分析，依赖名自然在这一步被解析——机制不同但目的相同 |
| 实例化时对实参做**隐式转换** | 未做：实参形态精确匹配（learn/18 记为已知简化） |
| 模板实参的链接期 ODR 合并 | 未做：教学编译器不产 .o 级别的模板弱符号 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-08-10 | S5 落地：克隆蓝图 → 替换 T → mangled 符号 → 注册 codegen | 两阶段查找的第二阶段在这里第一次真正跑起来 |
| 2026-09-16 | 模板机制主线收官：替换分两层（`substituteType` 管类型位 / `cloneExpr` 管表达式位） | NTTP 让"值位"挤进同一个替换表——一层装不下（learn/18） |
| 2026-09-22 | 语言基础补齐批次：函数模板实例的**尾部参数编码**补齐（`_Z5twiceIiET_T_`） | 少了尾部段，`T f(T)` 与 `T f(T, int)` 会撞成同一符号 |
| 2026-10-08 | vtable 槽里的符号名与定义点不同源（B20）/ 成员方法身份收口（B22） | 实例产物的符号名要与 vtable 槽里的名字**同源**——同一判据不许写两份（learn/13 §13.4.3） |
| 2026-10-09 | 类模板里的成员模板在实例类里丢失（B18） | "类实例化"这一步只搬了字段/方法/类内别名，**没搬 `memberTemplates`**（learn/34） |
