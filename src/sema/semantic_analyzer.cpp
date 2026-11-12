// 语义分析：解析名字与类型、选择调用目标、实例化模板并计算对象布局。
// 三遍处理分别收集类型/模板、注册函数、分析函数体，支持递归与前向调用。
// Parser 的名字种类查询也由本类维护的活作用域表提供。
// 参考：clang/lib/Sema/SemaDecl.cpp、SemaExpr.cpp、SemaTemplate.cpp。

#include "semantic_analyzer.h"
#include "template_deduction.h"
#include <format>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <map>
#include <set>
#include <functional>

namespace minicc {

// ─── SymbolKind 名称（仅供日志 / dump）──────────────────────────────────────
// demo: dump 符号表打印 `x  Variable  int  stack@-8` ⇒ "Variable" 由本函数产出
const char* symbolKindName(SymbolKind k) {
    switch (k) {
        case SymbolKind::Variable:    return "Variable";
        case SymbolKind::Parameter:   return "Parameter";
        case SymbolKind::Function:    return "Function";
        case SymbolKind::FunctionTemplate: return "FunctionTemplate";
        case SymbolKind::ClassField:  return "ClassField";
        case SymbolKind::ClassMethod: return "ClassMethod";
        case SymbolKind::Type:        return "Type";
    }
    return "Unknown";
}

// ═══ Scope 实现 ════════════════════════════════════════════════════════════
// 向本层符号表插入一条符号。emplace 失败（本层已有同名）返回 false —— 重声明检查
// [basic.scope.scope] 的落点。
// demo: 块内已有 `int x;` 再写 `int x;` ⇒ define 返回 false ⇒ processVarDecl 报
// "Variable 'x' already declared in this scope"。
//  外层同名不拦截（内层遮蔽外层合法）—— 遮蔽由 lookup 的顺序实现。
bool Scope::define(const std::string& name, Symbol sym) {
    auto [it, inserted] = m_symbols.emplace(name, std::move(sym));
    return inserted;
}

// 作用域链查找（[basic.scope.scope] 名字可见性的核心算法）：从本层开始，未命中则
// 递归父作用域，直到全局作用域为止。
// demo: block 内查 `x` ⇒ block ✗ → f ✓（内层遮蔽外层：先查到谁就用谁）；
//       若各层皆无 ⇒ nullptr ⇒ 上层转查类字段 / 函数名（见 inferVar）。
Symbol* Scope::lookup(const std::string& name) {
    auto it = m_symbols.find(name);
    if (it != m_symbols.end()) return &it->second;
    if (m_parent) return m_parent->lookup(name);
    return nullptr;
}

// 只查本层、不上链：用于需要"仅限同一作用域"语义的场合（对比 lookup）。
Symbol* Scope::lookupLocal(const std::string& name) {
    auto it = m_symbols.find(name);
    return (it != m_symbols.end()) ? &it->second : nullptr;
}

// ─── Scope::dump：打印当前作用域中的所有符号 ────────────────────────────────
void Scope::dump(int indent) const {
    std::string pad(indent * 2, ' ');
    std::cout << std::format("{}┌─ Scope[{}] '{}' (depth={}) ─────────────────\n",
        pad, m_depth, m_name.empty() ? "anonymous" : m_name, m_depth);

    if (m_symbols.empty()) {
        std::cout << std::format("{}│  (empty)\n", pad);
    }

    for (auto& [name, sym] : m_symbols) {
        std::string typeStr = sym.type ? sym.type->toString() : "?";
        std::string offsetStr = sym.isLocal
            ? std::format("  stack@{}", sym.stackOffset) : "";

        std::cout << std::format("{}│  {:<12} {:<10} {:<12}{}{}\n",
            pad,
            name,
            symbolKindName(sym.kind),
            typeStr,
            offsetStr,
            sym.ownerClass.empty() ? "" : std::format(" [{}]", sym.ownerClass));
    }

    std::cout << std::format("{}└──────────────────────────────────────\n", pad);
}

// ═══ SymbolTable 实现 ══════════════════════════════════════════════════════
// 构造：创建全局作用域（链根，depth=0，名字 "global"），类名/函数名等顶层符号都注册在这里。
SymbolTable::SymbolTable()
    : m_globalScope(std::make_unique<Scope>(nullptr, 0, "global"))
    , m_currentScope(m_globalScope.get())
    , m_currentDepth(0) {}

// 压栈进入新作用域：新 Scope 的 parent 指向当前作用域。
// demo: analyzeFunctionBody 调 enterScope("f")，函数体再调 enterScope("block")
//       ⇒ 形成 global ← f ← block 链。
// 所有权：newScope 移交 m_allScopes 统一持有（退出作用域 ≠ 销毁对象，保留到编译结束供 dump）。
void SymbolTable::enterScope(const std::string& name) {
    m_currentDepth++;
    auto newScope = std::make_unique<Scope>(m_currentScope, m_currentDepth, name);
    m_currentScope = newScope.get();
    m_allScopes.push_back(std::move(newScope));
}

// 弹栈回到父作用域：子作用域中的名字从此不可见（对象仍保留在 m_allScopes）。
// 防御：global 的 parent 为 nullptr，多退一步也不会崩溃。
void SymbolTable::exitScope() {
    if (m_currentScope && m_currentScope->parent()) {
        m_currentScope = m_currentScope->parent();
        m_currentDepth--;
    }
}

// 委托当前作用域：符号插入当前层（跨层插入不存在——可见性规则使然）。
bool SymbolTable::define(const std::string& name, Symbol sym) {
    return m_currentScope->define(name, std::move(sym));
}

// 委托当前作用域：从当前层沿链向外查（实现见 Scope::lookup）。
Symbol* SymbolTable::lookup(const std::string& name) {
    return m_currentScope->lookup(name);
}

void SymbolTable::dump() const {
    std::cout << "\n  ╔══════════════════════════════════════════════╗\n";
    std::cout << "  ║         SYMBOL TABLE (符号表快照)            ║\n";
    std::cout << "  ╚══════════════════════════════════════════════╝\n";
    m_globalScope->dump(2);
    std::cout << "\n";
}

void SymbolTable::dumpCurrentScope() const {
    m_currentScope->dump(2);
}

// 类型兼容检查剥除顶层引用/const，并支持教学版数值转换。
// 引用能否绑定须另查 [dcl.init.ref]，不能由剥除引用后的类型兼容代替。
bool SemanticAnalyzer::classSpecAtLeastAsSpecialized(const TemplateDeclPtr& a,
                                                     const TemplateDeclPtr& b) {
    // ① a 的模式 → 合成实参（形参名替换为唯一名）
    //   值位（NTTP）原样搬过去：偏序只需把"两边都是变量"的位置去相关，
    //   值位是常量，不与 b 的形参名同名，天然无关，无需重命名。
    std::vector<TemplateArg> synthArgs;
    const std::string prefix = "$ord_" + a->templateName() + "_";
    for (const auto& t : a->specPattern) {
        if (t.isType() && t.type) {
            synthArgs.push_back(TemplateArg::ofType(renameTemplateParams(t.type, prefix)));
        } else {
            synthArgs.push_back(t);
        }
    }

    // ② b 的形参名列表 —— 只有它们才是可绑定的"变量"
    std::vector<std::string> paramNames;
    for (const auto& p : b->templateParams) paramNames.push_back(p->name);

    //  SFINAE 吸收点 ③（全项目三处之一，见 include/sfinae.h 的收口点一览）
    // 这里"吸收"的语义有个特别之处：失败【不表示候选被淘汰】，而表示
    // "a 不比 b 更特化" —— 即偏序关系里的一个方向不成立。
    // 同一份 matchPattern 在两个场景里承载两种结论，故两处都必须显式标注，
    // 否则读代码的人会误以为这里是普通的匹配失败。
    // （吸收动作本身由 matchPattern 内部的 Sfinae::attempt 完成，本处只翻译结论。）
    std::unordered_map<std::string, TypePtr> subst;
    std::string reason;
    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);
    bool ok = deducer.matchPattern(b->specPattern, synthArgs, paramNames, subst, reason);

    std::cout << std::format(
        "  [order] '{}' 推 '{}' ⇒ {}    ({} 是否至少与 {} 同样特化)\n",
        patternToString(a->specPattern), patternToString(b->specPattern),
        ok ? "成功" : "失败（" + reason + "）",
        patternToString(a->specPattern), patternToString(b->specPattern));
    return ok;
}

// 深拷贝 + 把模板形参名换成 prefix+序号。只覆盖模式里可能出现的节点形态：
// TemplateParam / Class / Pointer / 引用 / Const。
// 对照 clang：TreeTransform 里 MakeUniqueSynthesizedType 的替换过程。
TypePtr SemanticAnalyzer::renameTemplateParams(const TypePtr& t,
                                               const std::string& prefix) {
    if (!t) return nullptr;
    if (t->isTemplateParam()) {
        auto r = Type::makeTemplateParam(prefix + t->templateParamName);
        return r;
    }
    if (t->isClass()) {
        // 模式里的裸形参名既可能是 TemplateParam 也可能是 Class（Parser 的产物），
        // 统一改名为合成名；带实参的类模板（如 std::void_t<...>）改内层实参。
        auto r = Type::makeClass(prefix + t->name);
        for (const auto& ta : t->templateArgs) {
            if (ta.isType() && ta.type) {
                r->templateArgs.push_back(
                    TemplateArg::ofType(renameTemplateParams(ta.type, prefix)));
            } else {
                r->templateArgs.push_back(ta);
            }
        }
        return r;
    }
    if (t->isPointer() && t->pointeeType) {
        return Type::makePointer(renameTemplateParams(t->pointeeType, prefix));
    }
    if (t->isLValueReference() && t->referencedType) {
        return Type::makeLValueReference(renameTemplateParams(t->referencedType, prefix));
    }
    if (t->isRValueReference() && t->referencedType) {
        return Type::makeRValueReference(renameTemplateParams(t->referencedType, prefix));
    }
    if (t->isConst() && t->innerType) {
        return Type::makeConst(renameTemplateParams(t->innerType, prefix));
    }
    return t;
}

std::string SemanticAnalyzer::patternToString(const std::vector<TemplateArg>& pattern) {
    std::string s;
    for (size_t i = 0; i < pattern.size(); i++) {
        if (i > 0) s += ", ";
        s += pattern[i].toString();   // 类型位印类型名，值位印值（含形态）
    }
    return s;
}

std::string SemanticAnalyzer::typeListToString(const std::vector<TemplateArg>& types) {
    return patternToString(types);
}

// [temp.res]：依赖限定名在替换后查找成员类型，如 `typename T::type`。
// 查找包含类内别名与基类；限定者必须保留类型结构以便替换。
TypePtr SemanticAnalyzer::findMemberType(const TypePtr& qual, const std::string& member) {
    if (!qual || !qual->isClass()) return nullptr;
    auto cit = m_classDecls.find(qual->name);
    if (cit == m_classDecls.end()) return nullptr;
    auto ait = cit->second->typeAliases.find(member);
    if (ait != cit->second->typeAliases.end() && ait->second) return ait->second;
    //  嵌套类（[class.nest]）：类作用域里的【类名】也是该类的成员类型 ——
    // `A::B` 里的 B 指 A 的嵌套类 B（与类内类型别名同属"类作用域里的类型名"）。
    // 登记名是限定名（"A::B"）⇒ 按 "限定者::成员名" 配。
    for (auto& nested : cit->second->nestedClasses) {
        if (nested->name == qual->name + "::" + member && nested->classType) {
            return nested->classType;
        }
    }
    return nullptr;
}

TypePtr SemanticAnalyzer::resolveMemberType(const TypePtr& qualifier,
                                            const std::string& member) {
    TypePtr q = resolveType(qualifier);
    TypePtr m = findMemberType(q, member);
    if (!m) {
        // 直接上下文内的失败 ⇒ 软失败信号（见 sfinae.h 的三方协议）
        Sfinae::fail(std::format("no type named '{}' in '{}'",
            member, q ? q->toString() : "?"));
    }
    return resolveType(m);
}

// [temp.alias]：别名模板只替换目标类型，不创建新的类身份。
// `template<class T> using Vec = MyPtr<T>;` 实例化后继续解析 MyPtr 的具体类型。
bool SemanticAnalyzer::isAliasTemplate(const std::string& name) const {
    return m_aliasTemplates.count(name) != 0;
}

// [dcl.type.class.deduct]：`MyPtr m(7);` 从构造实参或推导指引反推模板实参。
// 推导结果交给普通类模板实例化入口，避免另建实例缓存。
void SemanticAnalyzer::registerDeductionGuide(const DeductionGuideDeclPtr& g) {
    m_deductionGuides[g->guideName].push_back(g);
    std::cout << std::format(
        "  [register] deduction guide for '{}' (#{}) registered\n",
        g->guideName, m_deductionGuides[g->guideName].size());
}

TypePtr SemanticAnalyzer::deduceClassTemplateArgs(const VarDeclStmt& decl) {

    TypePtr t = decl.declaredType;
    // 适用条件（[dcl.type.class.deduct]/1）：裸的类模板名 + 括号直接初始化。
    // 已写实参（MyPtr<int> m(7)）、别名、指针等一律不走 CTAD。
    if (!t || !t->isClass() || !t->templateArgs.empty() || t->isNestedName()) return t;
    if (!m_classTemplates.count(t->name)) return t;
    if (decl.ctorArgs.empty()) return t;

    // ── 实参类型 ──
    std::vector<TypePtr> argTypes;
    for (auto& a : decl.ctorArgs) {
        argTypes.push_back(inferType(a));
    }
    return deduceCtadCore(t, argTypes, decl.name);
}

// CTAD 核心（语句级与表达式级 `Box(7)` 构造共用，[dcl.type.class.deduct]）：
// 两条来源：① 用户写的推导指引 ② 构造函数的隐式指引（拿每个构造函数的形参表当
// "指引形参表"，与函数模板推导同一套合一算法）。推导失败原样返回 t（调用方按
// 原路径继续，该报的错照报）；dbgName 仅供 [ctad] 日志冠名。
TypePtr SemanticAnalyzer::deduceCtadCore(TypePtr t,
                                         const std::vector<TypePtr>& argTypes,
                                         const std::string& dbgName) {
    TemplateDeclPtr primary = m_classTemplates[t->name];

    std::cout << std::format("  [ctad] ▶ {} {}(", dbgName, t->name);
    for (size_t i = 0; i < argTypes.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << (argTypes[i] ? argTypes[i]->toString() : "?");
    }
    std::cout << ") —— 未写模板实参，尝试类模板实参推导\n";

    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);

    auto tryGuide = [&](const std::vector<Parameter>& params,
                        const std::vector<std::string>& paramNames,
                        const std::string& via,
                        std::unordered_map<std::string, TypePtr>& outSubst) -> bool {
        if (params.size() != argTypes.size()) return false;
        DeductionResult out;
        outSubst.clear();
        for (size_t i = 0; i < params.size(); i++) {
            if (!deducer.deducePair(params[i].type, argTypes[i],
                                    /*argIsLValue=*/true, paramNames, outSubst, out)) {
                std::cout << std::format("  [ctad]   ✗ {} 不成立：{}\n", via, out.failureReason);
                return false;
            }
        }
        // 所有形参都必须推出来（[temp.deduct.type] 的收尾检查）
        for (const auto& p : paramNames) {
            if (!outSubst.count(p)) {
                std::cout << std::format("  [ctad]   ✗ {} 里有推不出的形参 '{}'\n", via, p);
                return false;
            }
        }
        return true;
    };

    // ── 来源①：用户写的推导指引（显式优先于隐式）──
    auto git = m_deductionGuides.find(t->name);
    if (git != m_deductionGuides.end()) {
        for (auto& g : git->second) {
            std::unordered_map<std::string, TypePtr> subst;
            std::vector<std::string> gParams;
            for (auto& p : g->templateParams) gParams.push_back(p->name);
            if (!tryGuide(g->parameters, gParams, "推导指引", subst)) continue;

            // 指引的 targetArgs 是"用推导结果参数化"的实参表
            TypePtr tid = Type::makeClass(t->name);
            for (auto& ta : g->targetArgs) {
                tid->templateArgs.push_back(
                    TemplateArg::ofType(resolveType(substituteInType(ta, subst))));
            }
            std::cout << std::format("  [ctad] ✔ 命中推导指引 ⇒ {}\n", tid->toString());
            return tid;
        }
    }

    // ── 来源②：构造函数的隐式指引 —— 拿每个构造函数的形参表当"指引形参表" ──
    // demo: template<class T> struct MyPtr { MyPtr(T q); };   实参 int
    //       ⇒ P = T, A = int ⇒ T := int ⇒ MyPtr<int>
    for (auto& method : primary->classTemplate->methods) {
        if (method->kind != NodeKind::Constructor) continue;
        auto ctor = std::static_pointer_cast<ConstructorDecl>(method);
        std::vector<Parameter> params = ctor->parameters;
        if (params.size() != argTypes.size()) continue;

        std::unordered_map<std::string, TypePtr> subst;
        std::string via = std::format("构造函数 {}({})", t->name, params.size());
        if (!tryGuide(params, primary->typeParams, via, subst)) continue;

        TypePtr tid = Type::makeClass(t->name);
        for (auto& p : primary->typeParams) {
            tid->templateArgs.push_back(TemplateArg::ofType(subst[p]));
        }
        std::cout << std::format("  [ctad] ✔ 由构造函数推出 ⇒ {}\n", tid->toString());
        return tid;
    }

    std::cout << std::format(
        "  [ctad] ✗ 推导失败 —— 没有可用的推导指引，构造函数也推不出模板实参\n");
    return t;   // 交回原路径：由 resolveType 报"缺模板实参"
}

// 把替换表套用到类型上（指引 targetArgs 用）。
// 指引写的是 `-> MyPtr<T>`，T 是**指引自带的模板形参**，而要推的可能是外层类的模板
// 形参（两者名字空间不同）⇒ 借道一次完整替换（复用 substituteType）。
TypePtr SemanticAnalyzer::substituteInType(
    const TypePtr& type, const std::unordered_map<std::string, TypePtr>& subst) {
    TemplateInstantiator::TypeSubstitution tagged;
    for (auto& [k, v] : subst) tagged[k] = TemplateArg::ofType(v);
    return m_instantiator.substituteType(type, tagged);
}

TypePtr SemanticAnalyzer::expandAliasTemplate(
    const std::string& name, const std::vector<TemplateArg>& args) {

    auto it = m_aliasTemplates.find(name);
    if (it == m_aliasTemplates.end()) return nullptr;   // 不是别名模板，交给类模板路径

    TemplateDeclPtr decl = it->second;

    // ── 递归防护：`template<class T> using X = X<T>;` 展开 X 的过程中又遇到 X ──
    // 对照 clang: error: recursive alias template instantiation
    for (const auto& expanding : m_expandingAliases) {
        if (expanding == name) {
            error(std::format(
                "recursive alias template instantiation '{}' —— 别名模板的底层"
                "类型里又用到了它自己，展开会无限递归", name), SourceLocation{});
        }
    }

    // ── 实参个数校验（[temp.alias]/2）：必须与形参表一致 ──
    // 同样走软失败通道 —— `Vec<int,int>` 出现在重载候选 / 偏特化模式里时，
    // 应当表现为"该候选不成立"而不是硬错误。
    if (args.size() != decl->templateParams.size()) {
        Sfinae::fail(std::format(
            "[Alias Error] alias template '{}' expects {} argument(s), got {}",
            name, decl->templateParams.size(), args.size()));
    }

    // ── 逐位绑定：形参名 → 实参（tagged：类型与 NTTP 值混排）──
    TemplateInstantiator::TypeSubstitution subst;
    for (size_t i = 0; i < args.size(); i++) {
        const TemplateParam& param = *decl->templateParams[i];
        const auto& arg   = args[i];

        // kind 校验（[temp.arg]/1）：类型形参收类型实参，非类型形参收值。
        if (param.kind == TemplateParamKind::Type && !arg.isType()) {
            Sfinae::fail(std::format(
                "[Alias Error] alias template '{}' parameter '{}' expects a type",
                name, param.name));
        }
        if (param.kind != TemplateParamKind::Type && arg.isType()) {
            Sfinae::fail(std::format(
                "[Alias Error] alias template '{}' parameter '{}' expects a "
                "non-type value", name, param.name));
        }
        subst[param.name] = arg;
    }

    std::cout << std::format("  [alias] 展开别名模板 {} → {}，替换表 {{",
        name, decl->aliasTemplate->underlyingType
                  ? decl->aliasTemplate->underlyingType->toString() : "?");
    bool first = true;
    for (auto& [k, v] : subst) {
        std::cout << std::format("{}{} := {}", first ? "" : ", ", k, v.toString());
        first = false;
    }
    std::cout << "}\n";

    // ── 只替换、不实例化（[temp.alias]/1）──
    // 复用类模板那套结构化替换引擎：`MyPtr<T>` 里的 T 换成 int 后，会变成一个
    // Class(MyPtr) + templateArgs=[int] 的节点，再交回 resolveType 按普通类模板 id
    // 实例化 ⇒ 别名与手写 MyPtr<int> 走【完全同一条路】，实例化引擎无需第二套逻辑。
    m_expandingAliases.push_back(name);
    TypePtr expanded;
    try {
        expanded = m_instantiator.substituteType(
            decl->aliasTemplate->underlyingType, subst);
        expanded = resolveType(expanded);
    } catch (...) {
        m_expandingAliases.pop_back();
        throw;      // SubstitutionFailure 原样上抛给 Sfinae::attempt
    }
    m_expandingAliases.pop_back();

    std::cout << std::format("  [alias] ★ {}<...> 解糖 ⇒ {}\n",
        name, expanded ? expanded->toString() : "?");

    // ── 解糖后仍停留在形参上 ⇒ 外层模板还没绑，保持依赖 ──
    // demo: template<class T> struct W { Vec<T> v; }; 在 W 的蓝图里展开 Vec<T> 时 T 仍是
    //   TemplateParam，substituteType 原样返回；等 W<int> 实例化时对字段类型再做替换，
    //   才真正落到 MyPtr_int。
    if (expanded && expanded->isTemplateParam()) {
        std::cout << std::format(
            "  [alias] 结果仍是模板形参 '{}' ⇒ 保持依赖，等外层替换\n",
            expanded->templateParamName);
    }
    return expanded;
}

// [dcl.type.decltype]：未加括号的名字/成员取声明类型，其余表达式按值类别生成类型。
// `decltype(x)` 与 `decltype((x))` 不可仅凭推导出的操作数类型合并处理。
TypePtr SemanticAnalyzer::evaluateDecltype(const ExprPtr& expr, bool paren) {
    if (!expr) {
        Sfinae::fail("decltype: operand is empty");
    }

    std::cout << std::format("  [decltype] 求值 decltype({}) —— {}\n",
        paren ? "(e)" : "e",
        paren ? "加括号 ⇒ 取【表达式类型】(左值带 &)"
              : "未加括号 ⇒ 若为 id-expression 则取【声明类型】");

    // SFINAE 立即上下文中的 decltype 错误只使候选失败，不能污染整个编译单元的诊断。
    // 普通表达式仍保留硬错误。
    TypePtr t;
    m_inferDepth++;
    try {
        t = inferType(expr);
    } catch (const SubstitutionFailure&) {
        m_inferDepth--;
        throw;                       // 已经是软失败，原样上抛
    } catch (const std::runtime_error& e) {
        m_inferDepth--;
        // 全项目唯一一处"硬错误 → 软失败"的转换点，日志由 SFINAE 模块统一出口
        Sfinae::demote(e.what());
        Sfinae::fail(std::format(
            "decltype: operand is not valid in this context — {}", e.what()));
    }
    m_inferDepth--;

    if (!t) {
        Sfinae::fail("decltype: cannot deduce the type of the operand");
    }
    if (t->isAuto()) {
        // 推导不出来（如 declval<T>() 里 T 仍未确定）——按 immediate-context 失败处理
        Sfinae::fail(
            "decltype: operand type is still undeduced ('auto')");
    }

    // ── 未加括号：取声明类型（inferType 给的就是它）──
    if (!paren) {
        std::cout << std::format("  [decltype]   ⇒ 声明类型 = {}\n", t->toString());
        return t;
    }

    // ── 加括号：取表达式类型，左值要带上 & ──
    // 【本实现的边界】minicc 的 inferType 不携带值类别信息，故用 isLValueExpr 对操作数
    //   形态做一次结构判定来近似：id-expression / 成员访问 / 解引用 / 下标 / 字符串字面量
    //   判为左值，其余（算术、比较、函数调用返回值）判为 prvalue。
    //   真 C++ 是在每个表达式节点上带 ValueKind 的，见 [basic.lval]。
    TypePtr result = t;
    if (!t->isReference() && isLValueExpr(expr)) {
        result = Type::makeLValueReference(t);
        std::cout << std::format("  [decltype]   ⇒ 左值 ⇒ 表达式类型 = {}&\n", t->toString());
    } else {
        std::cout << std::format("  [decltype]   ⇒ 右值 ⇒ 表达式类型 = {}\n", t->toString());
    }
    return result;
}

// 值类别判定的简化版：哪些表达式形态是左值。
// 对照 clang：clang/include/clang/AST/Expr.h:Expr::isLValue / Expr::getValueKind。
bool SemanticAnalyzer::isLValueExpr(const ExprPtr& expr) const {
    if (!expr) return false;
    // 值类别由节点种类直接决定，一次 switch 把分派表看全（零 RTTI 标签分派）。
    switch (expr->kind) {
        // 变量引用 [expr.prim.id]、成员访问 [expr.ref]、
        // 下标 [expr.sub]、字符串字面量 [lex.string] —— 都是左值
        case NodeKind::Var:
        case NodeKind::Member:
        case NodeKind::Index:
        case NodeKind::StringLiteral:
            return true;
        // 注：真 C++ 里解引用 *p 也是左值（[expr.unary.op]/1），但本项目的
        // UnaryOp 只有 Neg/Not 两种，构造不出解引用节点，故无此分支。
        default:
            return false;   // 其余（算术/比较/调用/new/字面量）按 prvalue
    }
}

// [class.member.lookup]：静态常量成员沿基类查找，命中后可折叠为字面量。
bool SemanticAnalyzer::lookupStaticConst(const std::string& className,
                                         const std::string& member,
                                         int64_t& outValue) {
    std::string cur = className;
    // 防御性深度上限：继承成环时不死循环（本项目不做环检测）
    for (int depth = 0; depth < 64 && !cur.empty(); depth++) {
        auto it = m_classDecls.find(cur);
        if (it == m_classDecls.end()) return false;
        ClassDeclPtr decl = it->second;

        auto sc = decl->staticConsts.find(member);
        if (sc != decl->staticConsts.end()) {
            outValue = sc->second.value;
            m_lastStaticConstType = sc->second.type;   // 供折叠时决定字面量种类
            std::cout << std::format(
                "  [static] ★ 静态常量命中：{}::{} = {} : {}（沿继承链第 {} 层）\n",
                cur, member, outValue,
                sc->second.type ? sc->second.type->toString() : "?",
                depth);
            return true;
        }
        cur = decl->firstBase();   // 继续往基类找
    }
    return false;
}

// `Cls<Args>::value` 的静态常量必须折叠，不能当对象字段按偏移寻址。
// 结果保留常量类型，供条件与非类型模板实参使用。
TypePtr SemanticAnalyzer::resolveTypeQualifier(const ExprPtr& expr) {
    if (expr->resolvedType) return expr->resolvedType;
    TypePtr type;
    if (expr->kind == NodeKind::Var) {
        auto root = std::static_pointer_cast<VarExpr>(expr);
        type = Type::makeClass(root->name);
        type->templateArgs = root->explicitTemplateArgs;
        type = resolveType(type);
    } else if (expr->kind == NodeKind::Member) {
        auto member = std::static_pointer_cast<MemberExpr>(expr);
        TypePtr parent = resolveTypeQualifier(member->object);
        type = findMemberType(parent, member->memberName);
        if (!type) {
            error(std::format("'{}::{}' is not a class, namespace, or enumeration",
                              parent->toString(), member->memberName), expr->location);
        }
        type = resolveType(type);
    }
    if (!type || !type->isClass()) {
        error(std::format("'{}' is not a class, namespace, or enumeration",
                          type ? type->toString() : "?"), expr->location);
    }
    expr->resolvedType = type;
    return type;
}

ExprPtr SemanticAnalyzer::foldStaticConst(ExprPtr expr) {
    // 定向形态判定：只看是不是 `X::value` 这种静态成员访问（isTypeAccess）
    if (expr->kind != NodeKind::Member) return expr;
    auto me = std::static_pointer_cast<MemberExpr>(expr);
    if (!me->isTypeAccess) return expr;

    const std::string clsName = resolveTypeQualifier(me->object)->name;
    ExprPtr root = me->object;
    while (root->kind == NodeKind::Member)
        root = std::static_pointer_cast<MemberExpr>(root)->object;
    auto ve = std::static_pointer_cast<VarExpr>(root);

    int64_t value = 0;
    m_lastStaticConstType = nullptr;
    if (!lookupStaticConst(clsName, me->memberName, value)) {
        // 类型限定访问只允许对应类中的静态成员；未命中时就地诊断，避免退回对象字段路径。
        bool isMethod = false;
        auto clsIt = m_classDecls.find(clsName);
        if (clsIt != m_classDecls.end()) {
            for (auto& m : clsIt->second->methods) {
                if (m->name == me->memberName) { isMethod = true; break; }
            }
        }
        if (!isMethod) {
            error(std::format(
                "no static member '{}' in class '{}' —— 类型限定访问 Cls<Args>::member "
                "只解析静态成员；若此处是 SFINAE 探测，说明偏特化被移除后回退到的"
                "主模板没有该成员（对照 test_tmpl_36 的兜底写法）",
                me->memberName, clsName), me->location);
        }
        return expr;   // 是方法 —— 交回常规调用路径
    }

    // 按常量自身的类型生成对应字面量：bool 常量给 BoolLiteralExpr，其余给 Int。
    //  不能一律给 int —— 本项目 typeCompatible(int, bool) 为 false，
    // `bool v = Trait<T>::value;` 会因此误报类型不匹配。
    ExprPtr lit;
    if (m_lastStaticConstType && m_lastStaticConstType->isBool()) {
        lit = std::make_shared<BoolLiteralExpr>(value != 0);
    } else {
        lit = std::make_shared<IntLiteralExpr>(value);
    }
    lit->location = me->location;
    std::cout << std::format("  [static] 折叠为字面量：{}::{}({}) → {}\n",
        clsName, me->memberName, ve->name, value);
    return lit;
}

// 类型解析示例：`Box<int>` 实例化类；未知类型拒收；依赖类型在替换后继续解析。
static bool containsTemplateParam(const TypePtr& t) {
    if (!t) return false;
    for (const auto& arg : t->templateArgs) {
        if (!arg.isType() || !arg.type) continue;
        if (arg.type->isTemplateParam()) return true;
        if (containsTemplateParam(arg.type)) return true;
    }
    return false;
}

// 名字的类型可见性由此统一裁决，resolveType 与无声明符声明共用相同规则。
// 查找遵循当前作用域，不能因全局注册表中存在同名类就视为可见。
// 参考：clang::Sema 的类型名查找入口。
SemanticAnalyzer::VisibleType
SemanticAnalyzer::lookupVisibleType(const std::string& name) {
    VisibleType hit;

    // 类成员类型从当前类及其外层类查找；模板实例的别名保存在克隆后的成员表中。
    if (!m_currentClassName.empty()) {
        std::string cls = m_currentClassName;
        while (!cls.empty()) {
            auto cit = m_classDecls.find(cls);
            if (cit == m_classDecls.end()) break;
            // ①0 类自身的名字在【本类作用域】内可见（[basic.scope.class]/2.1：
            // "the name of the class is inserted into the scope of the class itself"）——
            // `struct A { A* p; };` 里的 A 指本类（自引用指针，常见惯用法）。
            // 类的【可见名】= 登记名的最后一段（"N::A" ⇒ "A"，"A::B" ⇒ "B"）。
            //  这一条同时让"嵌套类在自己的作用域里能用裸名指自己"成立。
            const size_t selfSep = cls.rfind("::");
            const std::string selfVisible = (selfSep == std::string::npos)
                ? cls : cls.substr(selfSep + 2);
            if (selfVisible == name && cit->second->classType) {
                hit.type = cit->second->classType;
                hit.level = 1;
                hit.scope = cls;
                hit.isSelfClass = true;
                return hit;
            }
            // ①a 类内类型别名（using X = T; / typedef T X;）
            auto ait = cit->second->typeAliases.find(name);
            if (ait != cit->second->typeAliases.end() && ait->second) {
                hit.type = resolveType(ait->second);
                hit.raw = ait->second;      // 未解糖的别名目标（日志逐字用）
                hit.level = 1;
                hit.scope = cls;
                return hit;
            }
            // ①b 嵌套类（[class.nest]）：类作用域里的【类名】也是可见的类型 ——
            //   `A` 的成员函数里写 `B b;` 里的 B 指 A::B，不是全局 B。
            //   嵌套类的登记名是【限定名】（"A::B"）⇒ 按 "本类限定名::名字" 配。
            const std::string qualified = cls + "::" + name;
            for (auto& nested : cit->second->nestedClasses) {
                if (nested->name == qualified && nested->classType) {
                    hit.type = nested->classType;
                    hit.level = 1;
                    hit.scope = cls;
                    hit.isNestedClass = true;
                    return hit;
                }
            }
            cls = cit->second->ownerClassName;   // 沿类作用域链外溯一级
        }
    }

    // [basic.lookup.unqual]：从当前命名空间逐级向外查找，再查全局。
    // 例如 A::B 内可找到外层 A::S；不能只查一层限定前缀。
    std::string scope = m_currentNamespace;
    while (true) {
        const std::string qualified = scope.empty() ? name : scope + "::" + name;
        Symbol* sym = m_symbolTable.lookup(qualified);
        if (sym && sym->kind == SymbolKind::Type && sym->type) {
            hit.type = sym->type;
            hit.level = scope.empty() ? 3 : 2;
            hit.scope = scope;
            return hit;
        }
        if (scope.empty()) break;
        const size_t sep = scope.rfind("::");
        scope = (sep == std::string::npos) ? std::string() : scope.substr(0, sep);
    }
    return hit;   // level=0 ⇒ 不可见
}

// 这个名字【声明过】吗（任意 kind：变量/函数/类型皆算）——裸名声明裁决的第二问。
//   （第一问"它是不是可见的类型"见 lookupVisibleType。）
// 为什么单列一个：clang 的 ClassifyName 答的是"它是什么"，本实现拆成两问，
//   因为 minicc 的类作用域没有变量、局部作用域没有类型 —— 两问的查找面不同。
bool SemanticAnalyzer::isNameDeclared(const std::string& name) {
    if (m_symbolTable.lookup(name)) return true;     // 局部 → 全局
    std::string scope = m_currentNamespace;
    while (true) {
        const std::string qualified = scope.empty() ? name : scope + "::" + name;
        if (m_symbolTable.lookup(qualified)) return true;
        if (scope.empty()) break;
        const size_t sep = scope.rfind("::");
        scope = (sep == std::string::npos) ? std::string() : scope.substr(0, sep);
    }
    return false;
}

// 模板名与具体类型名分别查询；类模板/别名模板蓝图不能直接当已实例化类型。
bool SemanticAnalyzer::isVisibleTemplateName(const std::string& name) const {
    return m_classTemplates.count(name) > 0 || m_aliasTemplates.count(name) > 0;
}

TypePtr SemanticAnalyzer::resolveType(TypePtr type) {
    if (!type) return nullptr;

    // ── 数组类型（[dcl.array]）──
    if (type->isArray()) {
        TypePtr resolvedElem = resolveType(type->elementType);
        int64_t sz = type->arraySize;
        if (type->arraySizeExpr) {
            auto val = m_constantFolder.evaluateIntegral(type->arraySizeExpr);
            if (!val.has_value()) {
                error("array size is not a constant expression", SourceLocation{});
            }
            if (val->value <= 0) {
                error(std::format("array size must be greater than zero, got {}", val->value),
                      SourceLocation{});
            }
            sz = val->value;
        }
        return Type::makeArray(resolvedElem, sz);
    }

    // ── decltype(expr)：非依赖上下文的立即求值（依赖上下文见 substituteType Case 1.5）──
    if (type->isDecltype()) {
        std::cout << "  [decltype] resolveType 遇到 decltype 节点，尝试立即求值\n";
        try {
            return evaluateDecltype(type->decltypeExpr, type->decltypeParen);
        } catch (const SubstitutionFailure& e) {
            // 求值不成（多半因为操作数里有还没绑定的模板参数）——
            // 保留 Decltype 节点继续传递，等替换阶段再算。
            std::cout << std::format(
                "  [decltype] 立即求值不成（{}）⇒ 保持延迟，等替换阶段\n", e.what());
            return type;
        }
    }

    // ── std::void_t<...>：归约为 void ──
    // 解析期若把 void_t 当成普通类名，会撞上"'std::void_t' is not a class template"
    // 的误报。它的语义是"实参全部合法则等价于 void"，故直接归约；实参不合法时抛
    // SubstitutionFailure（SFINAE）。
    if (type->isClass() &&
        (type->name == "std::void_t" || type->name == "void_t")) {
        std::cout << "  [void_t] 归约 std::void_t<...> → void\n";
        for (const auto& ta : type->templateArgs) {
            if (ta.isType() && ta.type) {
                // 递归解析各实参：内部含 decltype 时会在上面被求值，
                // 求值失败即抛 SubstitutionFailure（软失败）。
                resolveType(ta.type);
            }
        }
        return Type::makeVoid();
    }

    if (type->isClass()) {
        // `S::type` 与 `S<int>::type` 先解析限定者，再查成员别名或嵌套类。
        // 不能只凭成员裸名查全局注册表。
        if (type->isNestedName()) {
            // ── 限定者还是模板形参 ⇒ 依赖上下文，不能在这里查 ──
            // `typename T::type` 要等实例化拿到实参才知道 T 是谁。
            // 原样返回（保持依赖），由 TemplateInstantiator::substituteType
            // 的 Case 5.5 在替换阶段查表 —— 那里才是 [temp.deduct]/8 的直接上下文。
            if (type->nestedQualifier && type->nestedQualifier->isTemplateParam()) {
                std::cout << std::format(
                    "  [sema:alias] {}::{} 限定者仍是模板形参 ⇒ 保持依赖，等替换阶段\n",
                    type->nestedQualifier->templateParamName, type->name);
                return type;
            }

            TypePtr qual = resolveType(type->nestedQualifier);
            std::string qualName = (qual && qual->isClass()) ? qual->name : "?";
            TypePtr member = findMemberType(qual, type->name);
            if (member) {
                TypePtr target = resolveType(member);
                std::cout << std::format("  [sema:alias] {}::{} ⇒ {}\n",
                    qualName, type->name, target ? target->toString() : "?");
                return target;
            }

            // 查不到：看当下是不是在直接上下文里 —— 是则降级为软失败
            // （SFINAE：该候选不成立，换下一个），不是才硬报错。
            std::string why = std::format("no type named '{}' in '{}'",
                                          type->name, qualName);
            if (SfinaeContext::inImmediateContext()) Sfinae::fail(why);
            error(why, SourceLocation{});
        }

        // ── P3：模板 id（Box<int>）→ 按需实例化，产出具体实例类型 ──
        // 判定条件：携带非空实参，且该名字是已登记的类模板蓝图（m_classTemplates，O(1)）。
        // （普通已实例化类如 Box_int 实参为空，走下面的符号表替换。）
        if (!type->templateArgs.empty()) {
            // `N::B<int>` 以完整限定名查模板蓝图；作用域是身份的一部分。
            // 限定查找失败时不能退回另一命名空间中的裸名 B。
            if (!m_aliasTemplates.count(type->name)
                && !m_classTemplates.count(type->name)) {
                if (auto sep = type->name.rfind("::");
                    sep != std::string::npos) {
                    const std::string scope = type->name.substr(0, sep);
                    const std::string tail = type->name.substr(sep + 2);
                    error(std::format(
                        "no template named '{}' in namespace '{}'",
                        tail, scope), SourceLocation{});
                } else {
                    // [temp.names]：未知模板名诊断为 no template named，区别于普通未知类型名。
                    error(std::format("no template named '{}'", type->name),
                          SourceLocation{});
                }
            }

            // ── 别名模板 id X<int>（[temp.alias]）──
            //  必须排在类模板分支【前面】：别名与类模板在语法上长得一模一样
            // （都是 名字<实参>），只能靠注册表区分。先问别名表，命中即解糖。
            if (m_aliasTemplates.count(type->name)) {
                std::vector<TemplateArg> args;
                args.reserve(type->templateArgs.size());
                for (size_t i = 0; i < type->templateArgs.size(); i++) {
                    auto& arg = type->templateArgs[i];
                    // 同一套按 kind 分派：类型实参递归解析（可为内层模板 id），
                    // NTTP 值实参原样带过。
                    if (arg.isType()) {
                        // NTTP 位裸名字按蓝图形参种类解析为编译期常量，如 `BufA<k>`。
                        const auto& aParams = m_aliasTemplates[type->name]->templateParams;
                        const TemplateParam* fp =
                            (i < aParams.size()) ? aParams[i].get() : nullptr;
                        if (fp && fp->kind == TemplateParamKind::NonType
                            && arg.type && arg.type->isClass()
                            && arg.type->templateArgs.empty()
                            && !arg.type->isNestedName()
                            && lookupConstValue(arg.type->name).has_value()) {
                            auto cv = *lookupConstValue(arg.type->name);
                            std::cout << std::format(
                                "  [sema:targ]   ⤷ 别名第 {} 位是 NTTP：'{}' 按"
                                "编译期常量重解 ⇒ {}（{}）\n",
                                i + 1, arg.type->name, cv.value, cv.type->toString());
                            args.push_back(TemplateArg::ofValue(cv.value, cv.type));
                            continue;
                        }
                        args.push_back(TemplateArg::ofType(resolveType(arg.type)));
                    }
                    else
                        args.push_back(arg);
                }
                std::cout << std::format(
                    "  [sema:alias] 遇到别名模板 id {}<...> ⇒ 解糖\n", type->name);
                return expandAliasTemplate(type->name, args);
            }
            // ── 实参里还有没绑定的模板形参 ⇒ 保持依赖，不实例化 ──
            //  硬去实例化会拿 TemplateParam 当模板实参造出一个名叫 `Box_T` 的【假实例】
            // （字段类型是 T、方法返回 T），后面全部对不上号，且不报错只是算错。
            // 对照 clang：clang/lib/Sema/SemaTemplate.cpp:Sema::CheckTemplateIdType 里若含依赖实参，建成
            // TemplateSpecializationType（带 sugar 的依赖类型）而非实例化。
            if (containsTemplateParam(type)) {
                std::cout << std::format(
                    "  [sema] {} 的实参仍是模板形参 ⇒ 保持依赖，等外层替换\n",
                    type->toString());
                return type;
            }
            if (m_classTemplates.count(type->name)) {
                // 实参先递归解析（实参本身可能是模板 id：Box<Box<int>>）
                TypePtr tid = Type::makeClass(type->name);
                tid->templateArgs.reserve(type->templateArgs.size());

                // 形参表（逐位判"这一位期望什么"）—— 模板模板位要特殊对待：
                // Parser 把实参位的 `Box` 建成了 Class("Box") 类型节点，若直接送
                // resolveType，会撞上"裸类模板名不能当类型"的报错。故先看形参位。
                const std::vector<TemplateParamPtr>* fps = nullptr;
                if (auto ptIt = m_classTemplates.find(type->name);
                    ptIt != m_classTemplates.end()) {
                    fps = &ptIt->second->templateParams;
                }

                for (size_t i = 0; i < type->templateArgs.size(); i++) {
                    auto& arg = type->templateArgs[i];
                    //  按 kind 分派（[temp.arg]）：类型实参 → 递归 resolveType
                    // （内层模板 id 也要实例化，如 Box<Box<int>>）；
                    // 非类型实参（NTTP）→ 是常量值，没有类型可解析，原样带过。
                    if (arg.isType()) {
                        // ──  该位期望一个【模板】时，实参是模板名而不是类型 ──
                        // `Wrap<Box, int>` 的 Box 不是"要被实例化的类型"，它本身就是
                        // 实参（[temp.arg.template]）。取出名字标成 Template 形态，
                        // 交给替换期去做 `C<T>` → `Box<T>` 的改名。
                        const TemplateParam* fp =
                            (fps && i < fps->size()) ? fps->at(i).get() : nullptr;
                        if (fp && fp->kind == TemplateParamKind::Template) {
                            // 模板模板实参要求模板名；`Wrap<int>` 和 `Wrap<Box<int>>` 不能代替 `Wrap<Box>`。
                            // 提前区分种类，避免把普通类型或具体特化伪装成模板蓝图。
                            const std::string& tn = arg.type ? arg.type->name : std::string();
                            if (!arg.type || !arg.type->isClass() || tn.empty()
                                || !arg.type->templateArgs.empty()   // ②：带实参的 id 是类型不是模板名
                                || (!m_classTemplates.count(tn)
                                    && !m_aliasTemplates.count(tn))) {
                                error(std::format(
                                    "template argument {} for '{}' ('{}') must be a class "
                                    "template, but '{}' is not a template",
                                    i + 1, type->name, fp->name,
                                    arg.type ? arg.type->toString() : "?"),
                                    SourceLocation{});
                            }
                            std::cout << std::format(
                                "  [sema:targ]   ⤷ 第 {} 位期望模板 ⇒ 实参 '{}' 按"
                                "【模板名】处理（[temp.arg.template]）\n",
                                i + 1, arg.type ? arg.type->name : "?");
                            tid->templateArgs.push_back(
                                TemplateArg::ofTemplate(arg.type ? arg.type->name : ""));
                            continue;
                        }
                        // NTTP 位的裸名字需由 Sema 区分类型与常量，如 `Buf<k>` 中的 constexpr k。
                        if (fp && fp->kind == TemplateParamKind::NonType
                            && arg.type && arg.type->isClass()
                            && arg.type->templateArgs.empty()
                            && !arg.type->isNestedName()) {
                            if (auto cv = lookupConstValue(arg.type->name)) {
                                std::cout << std::format(
                                    "  [sema:targ]   ⤷ 第 {} 位是 NTTP：'{}' 不是类型"
                                    " —— 按编译期常量重解 ⇒ {}（{}）\n",
                                    i + 1, arg.type->name,
                                    cv->value, cv->type->toString());
                                tid->templateArgs.push_back(
                                    TemplateArg::ofValue(cv->value, cv->type));
                                continue;
                            }
                        }
                        tid->templateArgs.push_back(
                            TemplateArg::ofType(resolveType(arg.type)));
                    }
                    else {
                        tid->templateArgs.push_back(arg);
                    }
                }
                // 模板 id 尚未实例化 ⇒ 当场实例化，拿到具体的类类型
                // （只有"带实参的类模板 id"走到这里；别名模板走另一条分支）
                return getOrInstantiateClass(tid, SourceLocation{});
            }
        } else if (m_classTemplates.count(type->name) ||
                   m_aliasTemplates.count(type->name)) {
            // ── 裸类模板名 / 裸别名模板名（[temp.arg.explicit]）──
            // `Box b;` 不带实参：蓝图不是类型，不能当类名解析。此处早期报错，
            // 而不是放行未解析类型到使用期才报 'Class not declared'（诊断不指向根因）。
            // 对照 clang：use of class template 'Box' requires template arguments。
            // 别名模板同理：`Vec v;` 在 clang 里也是同一句诊断。
            bool isAlias = m_aliasTemplates.count(type->name) != 0;
            error(std::format("'{}' is {} {} template; provide template "
                              "arguments (e.g. {}<int>)",
                              type->name, isAlias ? "an" : "a",
                              isAlias ? "alias" : "class",
                              type->name), SourceLocation{});
        }
        // [basic.lookup.unqual]：类作用域 → 命名空间链 → 全局，统一由 lookupVisibleType 裁决。
        // A::B 与全局 B 并存时，A 的成员函数中裸 B 指向 A::B。
        VisibleType hit = lookupVisibleType(type->name);
        if (hit.level == 1) {
            if (hit.isSelfClass) {
                // 类自身的名字（[basic.scope.class]/2.1）：`struct A { A* p; };`
                std::cout << std::format(
                    "  [sema:self] {} 在类 '{}' 作用域内 ⇒ {}（类自身的名字在本类作用域内可见）\n",
                    type->name, hit.scope, hit.type->toString());
            } else if (hit.isNestedClass) {
                // 嵌套类命中（[class.nest]）：类作用域里的类名，登记名带作用域
                std::cout << std::format(
                    "  [sema:nested] {} 在类 '{}' 作用域内 ⇒ {}\n",
                    type->name, hit.scope, hit.type->toString());
            } else {
                // 类作用域别名命中：打印【未解糖】的别名目标（既有日志行逐字不变）
                std::cout << std::format(
                    "  [sema:alias] {} 在类 '{}' 作用域内 ⇒ {}\n",
                    type->name, hit.scope,
                    hit.raw ? hit.raw->toString() : hit.type->toString());
            }
            return hit.type;
        }
        if (hit.level == 2) {
            // 命名空间命中：沿用既有日志行（逐字不变，基线契约）
            std::cout << std::format(
                "  [resolve] '{}' 在命名空间 '{}' 内 ⇒ {}\n",
                type->name, hit.scope, hit.scope + "::" + type->name);
            return hit.type;
        }

        // 全局：符号表的作用域链（函数/块/全局）
        Symbol* sym = m_symbolTable.lookup(type->name);
        // Parser 遇到 `Dog*` 这类书写名时会临时 new 一个 Class("Dog")，其 classLayout
        // 是空的；真正带字段/偏移/虚表布局的类型在 processClassDecl 阶段已登记进符号表
        // （kind=Type）。只要查到的注册类型与来者不是同一个对象，就用注册版替换 ——
        // 否则后面 inferMember 的 classLayout.findField 会在空布局上查无此字段。
        if (sym && sym->kind == SymbolKind::Type && sym->type &&
            sym->type != type) { // 指针不相同说明指向的不是同一个对象
            return sym->type;
        }
        if (hit.type) return hit.type;   // level 3 = 全局（lookupVisibleType 的兜底）
// ── 限定名类型 `Q::member`（嵌套类 / 类内类型别名）──
// 限定者先解析成【类型】，再在它的成员表里查成员类型（findMemberType）。
// 查不到 ⇒ clang 的 "no type named 'member' in 'Q'"（逐字对齐）。
//  放在符号表查找【之后】：登记名（"A::B"）已在符号表里，走符号表更快；
// 只有"限定名存在但成员不存在"才落到这里 —— 诊断因此指向根因。
        if (auto sep = type->name.rfind("::"); sep != std::string::npos) {
            const std::string qualifier = type->name.substr(0, sep);
            const std::string member    = type->name.substr(sep + 2);
            VisibleType q = lookupVisibleType(qualifier);
            if (q.type && q.type->isClass()) {
                TypePtr m = findMemberType(q.type, member);
                if (m) {
                    TypePtr target = resolveType(m);
                    std::cout << std::format("  [sema:nested] {}::{} ⇒ {}\n",
                        qualifier, member, target ? target->toString() : "?");
                    return target;
                }
                error(std::format("no type named '{}' in '{}'", member, qualifier),
                      SourceLocation{});
            }
        }

        // 未声明的类型名必须在此拒收，不能把空布局占位节点当成合法类继续生成代码。
        error(std::format(
            "unknown type name '{}' —— 该名字没有作为类/类型被声明过。"
            "（语句层的声明/表达式歧义（`a < b > c;`、裸 `S;`）由语义期的"
            "名字可见性判据裁决（docs/learn/48）；能走到这一步的 "
            "`名字 标识符/;` 是【真正的】未知类型名。）",
            type->name), SourceLocation{});
    }
    if (type->isPointer()) {
        auto base = resolveType(type->pointeeType);
        if (base != type->pointeeType) return Type::makePointer(base);
    }
    if (type->isReference()) {
        auto base = resolveType(type->referencedType);
        if (base != type->referencedType) return Type::makeLValueReference(base);
    }
    if (type->isRValueReference()) {
        auto base = resolveType(type->referencedType);
        if (base != type->referencedType) return Type::makeRValueReference(base);
    }
    if (type->isConst()) {
        auto base = resolveType(type->innerType);
        if (base != type->innerType) return Type::makeConst(base);
    }
    return type;
}

static bool typeCompatible(const TypePtr& expected, const TypePtr& got) {
    if (!expected || !got) return false;
    TypePtr e = expected, g = got;
    while (e->isReference()) e = e->referencedType;
    while (g->isReference()) g = g->referencedType;
    if (e->isConst()) e = e->innerType;
    if (g->isConst()) g = g->innerType;
    if (e->equals(g)) return true;
    // [conv]：整型家族之间允许整型转换，整型到浮点允许浮点-整型转换。
    // 按类型家族判断，包含 bool、char、short、unsigned 等整型。
    if (e->isInteger() && g->isInteger()) return true;
    if (e->isDouble()  && g->isInteger()) return true;
    return false;
}

// [overload.best.viable]：成员方法和构造函数共用候选择优，避免同一参数表得到不同结果。
// 本实现使用粗粒度转换等级，平局时报告歧义。
static FuncDeclPtr pickBestByArgs(const std::vector<FuncDeclPtr>& candidates,
                                  const std::vector<TypePtr>& argTypes,
                                  const std::string& what) {
    if (candidates.empty()) return nullptr;
    // 依赖上下文中实参类型未知时暂取声明序首个候选，避免把无法判定当作匹配失败。
    for (auto& t : argTypes) {
        if (!t) return candidates.front();
    }

    // 候选不少于两个时才输出择优日志，避免普通单候选调用淹没决议信息。
    const bool verbose = candidates.size() > 1;

    // ① 精确匹配（逐位 Type::equals；引用/const 的剥离交给 equals 自己）
    for (auto& m : candidates) {
        bool ok = m->parameters.size() == argTypes.size();
        for (size_t i = 0; i < m->parameters.size() && ok; ++i) {
            ok = m->parameters[i].type && argTypes[i]
              && m->parameters[i].type->equals(argTypes[i]);
        }
        if (ok) {
            if (verbose)
                std::cout << std::format("  [overload] {}({} args): {} 个候选 ⇒ 精确匹配 '{}'\n",
                    what, argTypes.size(), candidates.size(), m->mangledName);
            return m;
        }
    }
    // ② 可隐式转换（[conv]：整型家族互转、浮点 ← 整型）
    for (auto& m : candidates) {
        bool ok = m->parameters.size() == argTypes.size();
        for (size_t i = 0; i < m->parameters.size() && ok; ++i) {
            ok = typeCompatible(m->parameters[i].type, argTypes[i]);
        }
        if (ok) {
            if (verbose)
                std::cout << std::format("  [overload] {}({} args): {} 个候选 ⇒ 按隐式转换选 '{}'\n",
                    what, argTypes.size(), candidates.size(), m->mangledName);
            return m;
        }
    }
    // ③ 无可行候选 ⇒ nullptr（调用点据此报错，不再静默选一个）
    std::cout << std::format("  [overload] ✗ {}({} args): {} 个候选，无一可行 ⇒ 匹配失败\n",
        what, argTypes.size(), candidates.size());
    return nullptr;
}

SemanticAnalyzer::SemanticAnalyzer() {
    // 把自己挂成实例化引擎的三个求值器（decltype / 成员类型 / 别名模板）。
    // 这是"依赖倒置"的落地点：TemplateInstantiator 需要求值能力，但求值住在 Sema 里，
    // 于是 instantiator 只持有抽象接口指针，由 Sema 在构造时注入自己。
    // 对照 clang：不需要这层 —— 它的 Instantiator 本身就是 Sema（TreeTransform 派生自
    // Sema），本项目为保住分层与可单测性而外提。
    m_instantiator.setDecltypeEvaluator(this);
    m_instantiator.setMemberTypeResolver(this);
    m_instantiator.setAliasTemplateResolver(this);

    // 常量名字查询通过回调注入 ConstantFolder，保持折叠引擎不依赖符号表实现。
    m_constantFolder.setConstResolver(
        [this](const std::string& name) -> std::optional<ConstantFolder::IntegralValue> {
            // ① 直查全名（`A::limit` 粘连名、普通 const 变量）
            if (auto v = lookupConstValue(name)) return v;
            // 成员函数内静态常量裸名在折叠阶段即可查询，沿当前类与基类链查找。
            // 未命中保留原表达式，交正常名字解析。
            if (m_currentClassName.empty()) return std::nullopt;
            std::vector<std::string> work = {m_currentClassName};
            std::set<std::string> visited;
            while (!work.empty()) {
                std::string cur = work.back();
                work.pop_back();
                if (!visited.insert(cur).second) continue;
                if (auto v = lookupConstValue(cur + "::" + name)) return v;
                auto it = m_classDecls.find(cur);
                if (it == m_classDecls.end()) continue;
                for (auto& bn : it->second->baseClassNames) work.push_back(bn);
            }
            return std::nullopt;
        });

    // 为无需源码声明的内建名字预置解析期名字种类，如 std::declval。
    m_parseScopes.openScope(ScopeKind::Namespace, "std", ScopeBit::Namespace);
    m_parseScopes.declare("declval", ScopeBit::Template);
    m_parseScopes.leave();
}

// Parser 经此接口登记与查询名字；活作用域表由 Sema 唯一持有。
// 作用域进入/退出与声明点同步，保证类型、值和模板的遮蔽规则一致。
// 参考：clang 的 Parser Actions/Sema 协作。
void SemanticAnalyzer::enterParseScope(ScopeKind kind, const std::string& name) {
    m_parseScopes.enter(kind, name);
}

void SemanticAnalyzer::leaveParseScope() {
    m_parseScopes.leave();
}

void SemanticAnalyzer::declareParseName(const std::string& name, unsigned bits) {
    m_parseScopes.declare(name, bits);
}

std::string SemanticAnalyzer::openParseScope(ScopeKind kind,
                                              const std::string& name,
                                              unsigned bits) {
    return m_parseScopes.openScope(kind, name, bits);
}

unsigned SemanticAnalyzer::classifyParseName(const std::string& name) const {
    return m_parseScopes.classify(name);
}

unsigned SemanticAnalyzer::classifyParseQualified(
    const std::string& qualified) const {
    return m_parseScopes.classifyQualified(qualified);
}

// ─── 推导缩进辅助 ───────────────────────────────────────────────────────────
std::string SemanticAnalyzer::inferIndent() const {
    return std::string(m_inferDepth * 2, ' ');
}

// 三遍处理：先收集类型与模板，再注册函数，最后分析函数体。
// 调用体分析前已具备完整函数候选，支持递归与前向调用。
static void forEachClassMethod(const ClassDeclPtr& cls,
                               const std::function<void(FuncDeclPtr)>& action) {
    if (!cls) return;
    for (auto& method : cls->methods) action(method);
    for (auto& nested : cls->nestedClasses) forEachClassMethod(nested, action);
}

// ─── forEachFunctionDecl：对每个函数声明执行 action ──────────────────────────
// 顶层函数、类内方法，并【递归进命名空间】—— 命名空间里的类与函数不在
//   unit.declarations 顶层，扁平遍历会整个漏掉它们的方法（症状：Sema 侧一切正常，
//   但 CodeGen 从没收到那些方法 ⇒ 链接期 undefined reference to 'N__S_N__S'）。
// 分派方式：handler 需要 shared_ptr（registerFunction / analyzeFunctionBody 都收
//   FuncDeclPtr）⇒ 用 NodeKind 标签分派，判据同 processDecl。
void SemanticAnalyzer::forEachFunctionDecl(
    const std::vector<DeclPtr>& decls,
    const std::function<void(FuncDeclPtr)>& action) {
    for (auto& decl : decls) {
        switch (decl->kind) {
            case NodeKind::Function:
                action(std::static_pointer_cast<FunctionDecl>(decl));
                break;
            case NodeKind::Class: {
                auto cls = std::static_pointer_cast<ClassDecl>(decl);
                forEachClassMethod(cls, action);
                break;
            }
            case NodeKind::Namespace: {
                auto ns = std::static_pointer_cast<NamespaceDecl>(decl);
                std::string saved = m_currentNamespace;
                m_currentNamespace = saved.empty() ? ns->name : saved + "::" + ns->name;
                forEachFunctionDecl(ns->declarations, action);
                m_currentNamespace = saved;
                break;
            }
            default:
                break;   // 其余声明不产生独立函数体
        }
    }
}

// 声明按节点种类分派；需要保留声明身份的处理器接收声明指针。
void SemanticAnalyzer::processDecl(DeclPtr decl) {
    if (!decl) return;

    // 一次 switch（跳表）分派。static_pointer_cast 安全的前提：节点 kind 由构造函数
    // 设定，恒等于自身类型（见 ast_visitor.h 的标签不变式）。
    // 各支上方的 `数据事例` = 「源码 ⇒ Pass 1 的日志行」，全部出自
    // demos/core/04_decl_kinds.cpp 的编译输出（文件头有复现命令）。
    switch (decl->kind) {
        // ① 数据事例：`struct Box { int v; Box(); };`
        //    ⇒ [register] class 'Box'
        //       （随后跟着 field/method 逐条、══ Memory Layout ══ 与 vtable/RTTI 注入）
        case NodeKind::Class:
            processClassDecl(std::static_pointer_cast<ClassDecl>(decl)); break;
        // ② 数据事例：`template<class T> struct Wrap { T x; };`
        //    ⇒ [register] template <typename T> Wrap (class blueprint stored, not analyzed)
        //    另一形态（带外壳的推导指引）：
        //    `template<class T> Wrap(T) -> Wrap<T>;`
        //    ⇒ [register] template <typename T> deduction guide for 'Wrap' (rule stored, …)
        case NodeKind::Template:
            processTemplateDecl(std::static_pointer_cast<TemplateDecl>(decl)); break;
        // ③ 数据事例：`int g = 3;`（初始化式先折叠，再登记）
        //    ⇒ [infer] IntLiteral(3) → int  ⇒ [register] global variable 'g' : int
        case NodeKind::GlobalVar:
            processGlobalVarDecl(std::static_pointer_cast<GlobalVarDecl>(decl)); break;
        // ④ 数据事例：`enum Color { RED, GREEN };` ⇒ [register] enum 'Color' (2 items)
        case NodeKind::Enum:
            processEnumDecl(std::static_pointer_cast<EnumDecl>(decl)); break;
        // ⑤ 数据事例：`using Int = int;` ⇒ [register] type alias 'Int' = int
        case NodeKind::TypeAlias:
            processTypeAliasDecl(std::static_pointer_cast<TypeAliasDecl>(decl)); break;
        // ⑥ 数据事例：`namespace N { struct S { int w; }; }`
        //    ⇒ [namespace] enter namespace 'N' ⇒ [register] class 'N::S'（里面的类/函数随它递归）
        case NodeKind::Namespace:
            processNamespaceDecl(std::static_pointer_cast<NamespaceDecl>(decl)); break;
        // ── 非模板推导指引：`Box(int) -> Box<int>;` —— 顶层独立形态（不带 template<>
        //    外壳），单独接一支；带外壳的那种随 TemplateDecl 走 processTemplateDecl。
        // ⑦ 数据事例：`Wrap(int) -> Wrap<int>;`
        //    ⇒ [register] deduction guide for 'Wrap' (#2) registered
        //       （#N 是全局序号：带外壳的那条先登记 ⇒ 它是 #1、本条 #2）
        case NodeKind::DeductionGuide:
            registerDeductionGuide(std::static_pointer_cast<DeductionGuideDecl>(decl)); break;
        // default 数据事例：`int add(int a, int b) { return a + b; }` ⇒ **无日志**
        default:
            break;   // 函数声明等由 analyze 的三趟流程各自处理，不经过这里
    }
}

// 类布局示例：多态基类保留 vptr，派生字段放在基类子对象之后；偏移统一由布局计算产生。
void SemanticAnalyzer::analyze(TranslationUnit& unit) {

    // P4：libc 内建原型先登记——早于 Pass 1，全程可见
    registerBuiltins();

    std::cout << "\n  ┌──── Pass 1: 注册类、模板、全局变量与命名空间 ────\n";
    for (auto& decl : unit.declarations) {
        processDecl(decl);
    }

    // Pass 1 完成后所有类的继承关系已就绪 → 打印继承图（可观测性）
    printInheritanceGraph();

    std::cout << "\n  ┌──── Pass 2: 注册所有函数（支持递归）──────────────\n";
    //  必须【递归进命名空间】：命名空间里的类和函数不在 unit.declarations 顶层，
    // 扁平遍历会整个漏掉它们的方法 —— 症状是 Sema 侧一切正常（类布局、符号表都有），
    // 但 CodeGen 从没收到那些方法 ⇒ 合成构造/析构不发射，链接期报
    // undefined reference to 'N__S_N__S'。对照 clang：DeclContext 树本身就是递归的。
    forEachFunctionDecl(unit.declarations, [&](FuncDeclPtr f) { registerFunction(f); });

    // dump 全局符号表
    m_symbolTable.dump();

    std::cout << "  ┌──── Pass 3: 分析函数体（类型推导 + 符号决议）────\n";
    // 与 Pass 2 同理：命名空间内的函数体也要分析（形参/局部变量的类型解析、
    // 成员访问偏移，全都发生在这里）。
    forEachFunctionDecl(unit.declarations, [&](FuncDeclPtr f) { analyzeFunctionBody(f); });
}

// 继承图以无基类的类为根打印；多继承节点可从多个分支出现。
void SemanticAnalyzer::printInheritanceGraph() {
    // ① 邻接表：父类名 → 子类名列表（m_classDecls 是 Pass 1 的成果）
    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> roots; // 无基类的根类
    for (auto& [name, decl] : m_classDecls) {
        if (decl->baseClassNames.empty()) {
            roots.push_back(name);
        } else {
            for (auto& bn : decl->baseClassNames)
                children[bn].push_back(name);
        }
    }

    if (m_classDecls.empty()) return; // 无类可打印，静默跳过

    auto isPoly = [this](const std::string& cls) {
        auto it = m_classTypes.find(cls);
        return it != m_classTypes.end() && it->second->classLayout.hasVTable;
    };

    std::cout << "  │ ═══ 类型继承图（inheritance graph）═══\n";

    // ② 深度优先递归：prefix 记录竖线骨架，branch 决定本节点的分叉符号
    std::function<void(const std::string&, const std::string&, const std::string&)> dfs =
        [&](const std::string& cls, const std::string& prefix, const std::string& branch) {
            std::cout << std::format("  │ {}{}{}{}\n", prefix, branch, cls,
                isPoly(cls) ? " [polymorphic]" : "");
            auto it = children.find(cls);
            if (it == children.end()) return;
            auto& kids = it->second;
            std::sort(kids.begin(), kids.end()); // 排序保证输出可复现
            for (size_t i = 0; i < kids.size(); ++i) {
                bool last = (i + 1 == kids.size());
                // 本层分叉符号 + 下探一层的骨架（最后子节点以下空格延续）
                dfs(kids[i],
                    prefix + (branch.empty() ? "" : (last ? "    " : "│   ")),
                    last ? "└── " : "├── ");
            }
        };

    std::sort(roots.begin(), roots.end());
    for (auto& r : roots) {
        dfs(r, "", "");
    }
}

void SemanticAnalyzer::registerConstVar(
    const std::string& name, const TypePtr& type,
    ExprPtr& initializer, bool isConstexpr, const SourceLocation& loc) {
    // 只收 const（constexpr 隐含 const，见 parseType 前缀处理）的整型家族。
    if (!type || !type->isConst() || !type->stripConst()->isInteger()) return;

    // 初始化式此刻已经过 fold（registerConstVar 的调用点保证）；
    // evaluateIntegral 再折一遍是幂等的（已折好的树无 [fold] 新日志）。
    auto val = initializer ? m_constantFolder.evaluateIntegral(initializer)
                           : std::nullopt;

    if (!val.has_value()) {
        if (isConstexpr) {
            // clang++-18 原文：constexpr variable 'm' must be initialized by
            // a constant expression（[dcl.constexpr]/9 的强制义务）
            error(std::format(
                "constexpr variable '{}' must be initialized by a constant "
                "expression", name), loc);
        }
        // 普通 const 变量初始化式非常量：合法（运行期初始化），
        // 只是不能进编译期常量表 —— 不登记，传播/NTTP 重解都查不到它。
        return;
    }

    // 形态取【声明类型】（剥 const）：`constexpr long k = 4;` 的 k 是
    // long —— K<k> 必须是 K<4:long>（docs/learn/35 的撞键教训）。
    m_constValues[name] = ConstantFolder::IntegralValue{
        val->value, type->stripConst()};
    std::cout << std::format(
        "  [const] ★ '{}' = {}（{}）登记为编译期常量{}\n",
        name, val->value, type->stripConst()->toString(),
        isConstexpr ? "（constexpr）" : "");
}

void SemanticAnalyzer::processGlobalVarDecl(GlobalVarDeclPtr decl) {
    decl->declaredType = resolveType(decl->declaredType);

    // [class.static.data]：`int A::count = 7;` 是静态成员定义，不是新全局变量。
    // 按完整限定名关联原声明，避免生成第二份成员存储。
    bool isStaticMemberDef = false;
    {
        const auto sep0 = decl->name.rfind("::");
        if (sep0 != std::string::npos
            && m_classDecls.find(decl->name.substr(0, sep0)) != m_classDecls.end()) {
            isStaticMemberDef = true;
        }
    }
    if (isStaticMemberDef) {
        const auto sep = decl->name.rfind("::");
        const std::string qual = decl->name.substr(0, sep);
        const std::string member = decl->name.substr(sep + 2);

        // ① 类存在（m_classDecls 是全名注册：命名空间类叫 "N::A"）
        auto clsIt = m_classDecls.find(qual);
        if (clsIt == m_classDecls.end()) {
            error(std::format("'{}' is not a class, namespace, or enumeration", qual),
                  decl->location);
        }
        // ② 该类声明过这个 static 成员
        const StaticDataMember* smDecl = nullptr;
        for (auto& sm : clsIt->second->staticDataMembers) {
            if (sm.name == member) { smDecl = &sm; break; }
        }
        if (!smDecl) {
            error(std::format("no member named '{}' in '{}'", member, qual),
                  decl->location);
        }
        // ③ 类型一致：这里要的是【同一类型】而非可转换 —— typeCompatible 会把
        //    long/int 这种可隐转的对判成兼容，而 clang 对 `long A::count`（声明
        //    int）报 "redefinition of 'count' with a different type"（探针校准）。
        if (!smDecl->type || !decl->declaredType
            || !smDecl->type->equals(decl->declaredType)) {
            error(std::format(
                "redefinition of '{}' with a different type: '{}' vs '{}'",
                member, decl->declaredType ? decl->declaredType->toString() : "?",
                smDecl->type ? smDecl->type->toString() : "?"), decl->location);
        }
        // ④ 一个成员只许一个定义（ODR；C++ 要到链接期才爆，这里编译期拦）。
        //    clang 口径：报的是【成员裸名】不是全名。
        if (!m_definedStaticMembers.insert(decl->name).second) {
            error(std::format("redefinition of '{}'", member), decl->location);
        }
        std::cout << std::format(
            "  [static] ★ 类外定义：{}（存储 = 全局符号 {}）\n",
            decl->name, decl->name);
        // 初值过一遍折叠后走下面的类型检查/登记（与全局变量同路）。
    }

    // 全局初始化也先常量折叠，再登记常量表，使 `constexpr int k = 2+3;` 可传播或用作 NTTP。
    if (decl->initializer)
        decl->initializer = m_constantFolder.fold(std::move(decl->initializer));
    registerConstVar(decl->name, decl->declaredType, decl->initializer,
                     decl->isConstexpr, decl->location);
    if (decl->initializer) {
        TypePtr initType = inferType(decl->initializer);
        if (decl->declaredType->isAuto()) {
            decl->declaredType = initType;
        } else if (!typeCompatible(decl->declaredType, initType)) {
            error(std::format("Cannot initialize global variable '{}' of type '{}' with value of type '{}'",
                decl->name, decl->declaredType->toString(), initType->toString()), decl->location);
        }
    }
    m_globalVars.push_back(decl);

    Symbol sym;
    sym.name = decl->name;
    sym.type = decl->declaredType;
    sym.kind = SymbolKind::Variable;
    sym.isLocal = false;
    sym.definedAt = decl->location;
    // static 成员的类外定义：符号表条目在类内声明时已注册（first-wins，
    // 类型以声明点为准），重复 define 只会让快照里多出一条同名片花。
    if (!isStaticMemberDef) {
        m_symbolTable.globalScope()->define(decl->name, sym);
    }

    std::cout << std::format("  [register] global variable '{}' : {}\n",
        decl->name, decl->declaredType ? decl->declaredType->toString() : "auto");
}

void SemanticAnalyzer::processEnumDecl(EnumDeclPtr decl) {
    m_enumDecls[decl->name] = decl;
    TypePtr enumType = decl->underlyingType ? decl->underlyingType : Type::makeInt();

    for (auto& item : decl->items) {
        if (item.valueExpr) {
            inferType(item.valueExpr);
        }
        if (!decl->isScoped) {
            Symbol sym;
            sym.name = item.name;
            sym.type = enumType;
            sym.kind = SymbolKind::Variable;
            sym.isLocal = false;
            sym.definedAt = item.location;
            m_symbolTable.globalScope()->define(item.name, sym);
        }
        if (!decl->name.empty()) {
            Symbol scopedSym;
            scopedSym.name = decl->name + "::" + item.name;
            scopedSym.type = enumType;
            scopedSym.kind = SymbolKind::Variable;
            scopedSym.isLocal = false;
            scopedSym.definedAt = item.location;
            m_symbolTable.globalScope()->define(scopedSym.name, scopedSym);
        }
    }
    std::cout << std::format("  [register] enum '{}' ({} items)\n", decl->name, decl->items.size());
}

void SemanticAnalyzer::processNamespaceDecl(NamespaceDeclPtr decl) {
    std::cout << std::format("  [namespace] enter namespace '{}'\n", decl->name);

    // [basic.namespace]：处理命名空间时进入对应作用域，内部可非限定引用同空间成员。
    std::string savedNs = m_currentNamespace;
    m_currentNamespace = savedNs.empty()
        ? decl->name
        : savedNs + "::" + decl->name;

    for (auto& innerDecl : decl->declarations) {
        // 命名空间声明以限定名注册；函数定义的注册仍统一留给第二遍扫描。
        switch (innerDecl->kind) {
            case NodeKind::Function: {
                auto func = std::static_pointer_cast<FunctionDecl>(innerDecl);
                func->name = m_currentNamespace + "::" + func->name;
                break;
            }
            case NodeKind::Class: {
                auto cls = std::static_pointer_cast<ClassDecl>(innerDecl);
                cls->name = m_currentNamespace + "::" + cls->name;
                processClassDecl(cls);
                break;
            }
            case NodeKind::GlobalVar: {
                auto gvar = std::static_pointer_cast<GlobalVarDecl>(innerDecl);
                gvar->name = m_currentNamespace + "::" + gvar->name;
                processGlobalVarDecl(gvar);
                break;
            }
            case NodeKind::Enum: {
                auto enm = std::static_pointer_cast<EnumDecl>(innerDecl);
                enm->name = m_currentNamespace + "::" + enm->name;
                processEnumDecl(enm);
                break;
            }
            case NodeKind::Template: {
                // 模板按完整限定名注册；`N::B<int>` 与 `M::B<int>` 具有独立实例、缓存和符号。
                auto tpl = std::static_pointer_cast<TemplateDecl>(innerDecl);
                if (tpl->isClassTemplate() && tpl->classTemplate) {
                    tpl->classTemplate->name =
                        m_currentNamespace + "::" + tpl->classTemplate->name;
                } else if (tpl->isAliasTemplate() && tpl->aliasTemplate) {
                    tpl->aliasTemplate->aliasName =
                        m_currentNamespace + "::" + tpl->aliasTemplate->aliasName;
                }
                // 函数模板与推导指引：名字不加前缀（限定名调用函数模板本项目
                // 不支持，见 spec §2.2 第 2 条）。
                processDecl(innerDecl);
                break;
            }
            default:
                processDecl(innerDecl);   // 其余（别名、推导指引…）走通用入口
                break;
        }
    }

    m_currentNamespace = savedNs;   // 退出命名空间作用域
}

void SemanticAnalyzer::processTypeAliasDecl(TypeAliasDeclPtr decl) {
    // 别名目标注册时即解析：`using IntBox = Box<int>;` 必须指向已解析的实例类型。
    // 不能把空布局的模板 id 占位节点作为可用对象类型保存。
    TypePtr resolved = resolveType(decl->underlyingType);

    // ── 命名空间成员【名字前缀化】（与类/函数同约定，processNamespaceDecl）──
    // 曾用裸名注册 ⇒ 类外 `N::IntBox` 查无此名（unknown type name）。
    std::string fullName = m_currentNamespace.empty()
        ? decl->aliasName
        : m_currentNamespace + "::" + decl->aliasName;

    Symbol sym;
    sym.name = fullName;
    sym.type = resolved;
    sym.kind = SymbolKind::Type;
    sym.isLocal = false;
    sym.definedAt = decl->location;
    m_symbolTable.globalScope()->define(fullName, sym);
    std::cout << std::format("  [register] type alias '{}' = {}\n",
        fullName, resolved ? resolved->toString() : "?");
}

// 方法符号只由 registerFunction 在声明上确定，调用点与虚表槽引用该结果。
// 重载身份依赖形参类型；不能在布局和函数注册两个时刻分别计算符号。
// 参考：clang/lib/AST/ItaniumMangle.cpp:ItaniumMangleContext。
static std::string paramTypeChain(const std::vector<Parameter>& params) {
    std::string chain;
    for (auto& p : params) {
        if (!chain.empty()) chain += "_";
        chain += sanitizeSymbolChars(p.type ? p.type->toString() : "?");
    }
    return chain;
}

// 同类同名同参数个数的方法需要类型链后缀，且每个同组方法采用同一命名口径。
// 例如 `f(int)` 与 `f(S)` 不能只给后出现的方法加后缀。
static size_t countSameNameSameArity(const std::vector<FuncDeclPtr>& methods,
                                     const std::string& name, size_t arity) {
    size_t n = 0;
    for (auto& m : methods) {
        if (m->name == name && m->parameters.size() == arity) ++n;
    }
    return n;
}

// 槽位名：析构统一记作 "dtor"（vtable 里不区分是哪个类的析构）。
//  判据单点：覆写检测、新槽登记、句柄回查三处都用它，析构名不许各写一遍。
// demo：method->name="~Outer" ⇒ "dtor"；"speak" ⇒ "speak"
static std::string vtableSlotName(const std::string& methodName) {
    return methodName.starts_with("~") ? "dtor" : methodName;
}

static std::string memberMethodSymbolName(const std::string& owner,
                                          const std::string& methodName,
                                          const std::vector<Parameter>& params,
                                          size_t earlierSameNameCount,
                                          bool needsTypeDisambiguation) {
    if (methodName.starts_with("~")) return owner + "_dtor";
    std::string sym = owner + "_" + methodName;
    if (earlierSameNameCount > 0 || !params.empty()) {
        sym += "_" + std::to_string(params.size());
    }
    // 同名同参数个数的重载以形参类型链区分，如 `f(int)` 与 `f(S)`。
    if (needsTypeDisambiguation) {
        std::string chain = paramTypeChain(params);
        sym += "_" + (chain.empty() ? std::string("void") : chain);
    }
    return sym;
}

// 类语义分析建立成员类型、基类子对象、字段布局、虚表与 RTTI。
// 布局阶段保存方法声明句柄，符号在函数注册时统一确定。
// 参考：clang/lib/AST/RecordLayoutBuilder.cpp、VTableBuilder.cpp。
void SemanticAnalyzer::processClassDecl(ClassDeclPtr decl) {
    // 前置声明只登记名字，不生成布局；`struct A; struct B { A* p; };` 不要求 A 已完整定义。
    if (decl->isForwardDecl) {
        std::cout << std::format(
            "  [register] class '{}' (forward declaration ⇒ 只登记名字，不产符号)\n",
            decl->name);
        // 三处登记（与正常路径末尾同构）：类型表 + AST 声明 + 符号表。
        //  符号表这条不能省 —— resolveType 查的是 m_symbolTable.lookup()，
        // 少了它 `struct B { A* pa; };` 会误报 unknown type name 'A'。
        TypePtr fwdType = Type::makeClass(decl->name);
        decl->classType = fwdType;
        m_classTypes[decl->name] = fwdType;
        m_classDecls[decl->name] = decl;

        Symbol fwdSym;
        fwdSym.name = decl->name;
        fwdSym.type = fwdType;
        fwdSym.kind = SymbolKind::Type;
        fwdSym.isLocal = false;
        fwdSym.definedAt = decl->location;
        m_symbolTable.define(decl->name, fwdSym);
        return;
    }

    // 处理本类期间置"当前类"，供 resolveType 的类作用域回退用（类体内 `type x;` 要先在
    // 类作用域的 typeAliases 里找）。方法体的分析由 registerFunction 另行覆盖/还原这个值。
    std::string savedClassScope = m_currentClassName;
    m_currentClassName = decl->name;

    std::cout << std::format("  [register] class '{}' ", decl->name);
    if (!decl->baseClassNames.empty()) {
        std::cout << ": public ";
        for (size_t i = 0; i < decl->baseClassNames.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << decl->baseClassNames[i];
        }
    }
    std::cout << "\n";

    // 为本类新建类型对象：classLayout（字段/偏移/vtable）都挂在它上面，完成后注册进
    // m_classTypes（"编译期看符号"的那个符号）。
    //  必须【立刻】挂到 decl->classType —— 后面的 computeClassLayout(decl) 从
    // decl->classType 取类型计算偏移；若拖到函数末尾才赋值，布局会算在一个
    // hasVTable=false 的临时孤儿类型上：字段偏移漏掉 _vptr、totalSize=0，
    // CodeGen 按 0 字节 malloc，运行期写必然越界。
    TypePtr classType = Type::makeClass(decl->name);
    decl->classType = classType;
    // 立刻登记 AST 声明：下面的字段/方法类型解析要经 resolveType 的"类作用域回退"查
    // 本类的 typeAliases，而那条回退是经 m_classDecls 找类声明的 —— 晚到函数末尾才登记
    // 的话，类体内的别名一律查不到（`Int a;` 报 unknown type name）。
    m_classDecls[decl->name] = decl;

    // 嵌套类先于外层字段/方法处理，使 `struct A { struct B {}; B b; };` 中 B 可见。
    // 限定名字由 processClassDecl 统一生成，克隆处不重复拼接。
    for (auto& nested : decl->nestedClasses) {
        nested->ownerClassName = decl->name;      // 类作用域链的一环（外溯用）
        nested->name = decl->name + "::" + nested->name;
        processClassDecl(nested);
    }

    // ── 处理继承（多继承：[class.mi] Itanium 主基类优化模型）──
    // 第一个多态基类 = 主基类（共享主表 + 字段无限定），其余多态基类 = 次基类（独立次表
    // entries，字段带 "BaseName." 前缀）。非多态基类的字段也带前缀（避免多基类同名冲突）。
    std::vector<std::string> allBaseFieldNames; // 所有基类字段名（用于自身字段限定）
    bool hasPrimaryVTable = false;
    //  本循环【不计算】子对象偏移 —— offset 统一由循环后的 [relocate] 阶段按 Itanium
    // 规则摆放（primary 恒占 0，其余从 primary 尾部对齐累加）。
    //  边扫边放会出两个 offset=0：非多态基类声明在多态基类之前时 A 先占 0、primary P
    // 又写死 0 ⇒ 子对象重叠（A.x 压 _vptr）；且 primary 分支的 currentOffset 是"重置"
    // 而非累加，会把先摆的进度悄悄丢弃。

    if (!decl->baseClassNames.empty()) {
        // 保存自身字段（Parser 已将它们填入 decl->fields），继承处理中重建顺序
        std::vector<FieldInfo> ownFields = std::move(decl->fields);
        decl->fields.clear();

        for (size_t baseIdx = 0; baseIdx < decl->baseClassNames.size(); ++baseIdx) {
            const std::string& baseName = decl->baseClassNames[baseIdx];
            auto baseIt = m_classTypes.find(baseName);
            if (baseIt == m_classTypes.end()) { // 这里就是要 必须先声明base 类才可以，必须要按照顺序去初始化才可以
                error(std::format("Base class '{}' not found", baseName), decl->location);
            }
            TypePtr baseType = baseIt->second;

            // 收集基类字段名（用于自身字段限定）
            for (auto& bf : baseType->classLayout.fields) {
                std::string rawName = bf.name;
                auto dot = rawName.find('.');
                if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                allBaseFieldNames.push_back(rawName);
            }

            if (!hasPrimaryVTable && baseType->classLayout.hasVTable) { // 第一个有虚函数的类才算是主基类
                // ═══ 主基类（primary base）═══
                // 合并字段到主字段列表 + 合并 vtable 到主表
                // 字段名不加限定前缀（保持单继承兼容）
                for (size_t k = 0; k < baseType->classLayout.fields.size(); ++k) {
                    FieldInfo fi = baseType->classLayout.fields[k];
                    if (fi.declaredName.empty()) fi.declaredName = fi.bareName();
                    if (fi.sourceClass.empty()) fi.sourceClass = baseName;
                    // 继承字段的 viaBase/baseFieldIndex 必须改为当前层直接基类来源。
                    // 照搬更深层记录会把字段偏移加到错误子对象。
                    fi.viaBase = baseName;
                    fi.baseFieldIndex = static_cast<int>(k);
                    decl->fields.push_back(fi);
                }
                for (auto& baseEntry : baseType->classLayout.vtableEntries) {
                    classType->classLayout.vtableEntries.push_back(baseEntry);
                }
                classType->classLayout.hasVTable = true;
                hasPrimaryVTable = true;

                BaseSubobject primarySub;
                primarySub.baseClassName = baseName;
                primarySub.hasVTable = true;
                primarySub.isPrimary = true;
                primarySub.vtableSegmentOffset = 0;
                // offset 不在此设置：[relocate] 阶段统一置 0 并摆放其余子对象
                classType->classLayout.bases.push_back(primarySub);

                std::cout << std::format("    ↳ [primary] '{}': {} fields, {} vtable entries\n",
                    baseName, baseType->classLayout.fields.size(),
                    baseType->classLayout.vtableEntries.size());
            } else if (baseType->classLayout.hasVTable) {
                // ═══ 次基类（secondary base）═══
                // 字段带 "BaseName." 限定前缀；vtable 条目存入独立次表 entries
                for (size_t k = 0; k < baseType->classLayout.fields.size(); ++k) {
                    FieldInfo fi = baseType->classLayout.fields[k];
                    fi.declaredName = fi.bareName();          // 权威裸名先落袋
                    fi.name = baseName + "." + fi.declaredName;
                    fi.sourceClass = baseName;
                    fi.viaBase = baseName;
                    fi.baseFieldIndex = static_cast<int>(k);
                    decl->fields.push_back(fi);
                }

                BaseSubobject secSub;
                secSub.baseClassName = baseName;
                secSub.hasVTable = true;
                secSub.isPrimary = false;
                // 次基类子对象偏移由布局重定位统一确定；次表覆写另记录 this 调整量。
                for (auto& baseEntry : baseType->classLayout.vtableEntries) { // 次基类的vtable entry 不是放到 classLayout里面的
                    VTableEntry secEntry = baseEntry;
                    // 兜底：baseFunctionName 是权威裸名，正常路径上由条目创建点写入，必非空；
                    // 万一为空则从【句柄】回查（ 不再从符号串反推名字 —— 那是把
                    // "给人看的串"当"机器用的键"，见 docs/learn/23 的同族教训）。
                    if (secEntry.baseFunctionName.empty() && secEntry.method) {
                        secEntry.baseFunctionName = vtableSlotName(secEntry.method->name);
                    }
                    secSub.entries.push_back(secEntry);
                }
                classType->classLayout.bases.push_back(secSub);

                std::cout << std::format("    ↳ [secondary] '{}': {} fields, {} vtable entries\n",
                    baseName, baseType->classLayout.fields.size(),
                    secSub.entries.size());
            } else {
                // ═══ 非多态基类 ═══
                // 字段带限定前缀，无 vtable 贡献
                for (size_t k = 0; k < baseType->classLayout.fields.size(); ++k) {
                    FieldInfo fi = baseType->classLayout.fields[k];
                    fi.declaredName = fi.bareName();          // 权威裸名先落袋
                    fi.name = baseName + "." + fi.declaredName;
                    fi.sourceClass = baseName;
                    fi.viaBase = baseName;
                    fi.baseFieldIndex = static_cast<int>(k);
                    decl->fields.push_back(fi);
                }

                BaseSubobject nonPolySub;
                nonPolySub.baseClassName = baseName;
                nonPolySub.hasVTable = false;
                nonPolySub.isPrimary = false;
                // offset 由 [relocate] 阶段统一摆放
                classType->classLayout.bases.push_back(nonPolySub);

                std::cout << std::format("    ↳ [non-poly] '{}': {} fields\n",
                    baseName, baseType->classLayout.fields.size());
            }
        }

        // 基类子对象偏移在此唯一计算：主基类占 offset 0，其余按大小与对齐顺序摆放。
        // 先收集后重定位，避免字段和虚表各算一份偏移。
        if (!classType->classLayout.bases.empty()) {
            size_t primaryIdx = 0;
            bool anyPoly = false;
            for (size_t i = 0; i < classType->classLayout.bases.size(); ++i) {
                if (classType->classLayout.bases[i].hasVTable) { // 第一个有虚函数的类是主基类
                    primaryIdx = i;
                    anyPoly = true;
                    break;
                }
            }
            // 本类自身是否多态：无多态基类时 vtable 只可能来自本类，而"覆写基类虚函数"
            // 也要求基类先有多态 ⇒ 此刻只需看有没有带 virtual 关键字的方法。
            //  判据必须与下方方法注册循环同源（那里才最终置 hasVTable）—— 位置却被夹在
            // 前面，因为子对象偏移随后就要被 thunkAdjust 用掉。
            bool selfPoly = false;
            if (!anyPoly) {
                for (auto& m : decl->methods) {
                    if (m->isVirtual) { selfPoly = true; break; }
                }
            }
            // 本类多态但没有动态主基类时，vptr 占 offset 0，基类子对象从偏移 8 起摆放。
            bool noPrimary = (!anyPoly && selfPoly); // 如果是true 表示没有主机类，但是自己有多态

            for (size_t i = 0; i < classType->classLayout.bases.size(); ++i)
                classType->classLayout.bases[i].isPrimary = (!noPrimary && i == primaryIdx);

            // place = 已占用区间的右端：有 primary 时是 primary 子对象尾部；无 primary
            // （本类自身多态）时是 vptr 之后的 8。
            //  不能用"全非多态 ⇒ place=0"的写法：那会让第二个基类被 alignTo(0,8)=0 放到
            // offset 0，与首个基类字段重叠（x@0 与 y@0 互踩）。
            uint32_t place = noPrimary ? 8u : 0u;
            if (!noPrimary) {
                auto pIt = m_classTypes.find(
                    classType->classLayout.bases[primaryIdx].baseClassName);
                if (pIt != m_classTypes.end())
                    place = pIt->second->classLayout.totalSize; // primary 尾部
            }
            for (size_t i = 0; i < classType->classLayout.bases.size(); ++i) {
                auto& sub = classType->classLayout.bases[i];
                if (sub.isPrimary) {
                    sub.offset = 0;
                    continue;   // primary 恒 0，无需打印（保持既有日志逐字节不变）
                }
                sub.offset = alignTo(place, 8); // 这里非常关键, 这里会对结束位置再做一次偏移，彻底锁死对应的位置
                auto bIt = m_classTypes.find(sub.baseClassName);
                place = sub.offset + (bIt != m_classTypes.end()
                    ? bIt->second->classLayout.totalSize : 0);
                std::string why = noPrimary
                    ? std::string("本类自身多态 ⇒ 无主基类，_vptr 占 0")
                    : std::format("primary='{}' 占 0",
                          classType->classLayout.bases[primaryIdx].baseClassName);
                std::cout << std::format("    ↳ [relocate] '{}' → offset {} ({})\n",
                    sub.baseClassName, sub.offset, why);
            }
        }

        // 追加自身字段到最后
        for (auto& f : ownFields) {
            // 自身字段：viaBase 留空（"住在本类里"）、下标 -1；权威裸名就是解析期那个名字
            if (f.declaredName.empty()) f.declaredName = f.name;
            f.viaBase.clear();
            f.baseFieldIndex = -1;
            decl->fields.push_back(f);
        }
    }

    // 自身字段与基类字段同名时加限定前缀（避免覆写基类字段名）
    size_t inheritedCount = 0;
    for (auto& baseName : decl->baseClassNames) {
        auto baseIt = m_classTypes.find(baseName);
        if (baseIt != m_classTypes.end())
            inheritedCount += baseIt->second->classLayout.fields.size();
    }
    for (size_t i = inheritedCount; i < decl->fields.size(); ++i) {
        std::string rawName = decl->fields[i].name;
        for (auto& bfn : allBaseFieldNames) {
            if (rawName == bfn) {
                decl->fields[i].name = decl->name + "." + rawName;
                decl->fields[i].sourceClass = decl->name;
                break;
            }
        }
    }

    // ── 注册字段到符号表：遍历的是"合并后"的字段列表（基类字段已在继承处理时插到
    //    头部），逐一转成 FieldInfo 挂进布局表；offset/size 稍后由 computeClassLayout 算。
    for (auto& field : decl->fields) {
        // 字段先 resolveType 再计算大小，避免模板 id 或类名占位类型带来零大小布局。
        field.type = resolveType(field.type);

        if (field.declaredName.empty()) field.declaredName = field.bareName();
        FieldInfo fi;
        fi.name = field.name;
        fi.type = field.type;
        fi.access = field.access;
        fi.declaredName = field.declaredName;
        fi.viaBase = field.viaBase;
        fi.baseFieldIndex = field.baseFieldIndex;
        classType->classLayout.fields.push_back(fi);

        std::cout << std::format("    field: {} : {}\n",
            field.name, field.type ? field.type->toString() : "?");
    }

    // [class.static.data]：静态成员登记独立存储/常量信息，不进入对象字段布局。
    // 每个类模板实例拥有自身的静态成员身份。
    for (auto& sm : decl->staticDataMembers) {
        sm.type = resolveType(sm.type);
        const std::string fullName = decl->name + "::" + sm.name;

        // 类内重复声明（`static int count; static int count;`）当场拒收
        for (auto& other : decl->staticDataMembers) {
            if (&other != &sm && other.name == sm.name) {
                error(std::format("duplicate static member '{}'", sm.name), decl->location);
            }
        }

        Symbol sym;
        sym.name = fullName;
        sym.type = sm.type;
        sym.kind = SymbolKind::Variable;
        sym.isLocal = false;
        sym.definedAt = decl->location;
        m_symbolTable.globalScope()->define(fullName, sym);
        std::cout << std::format("    static member: {} : {}\n",
            fullName, sm.type ? sm.type->toString() : "?");

        if (sm.initializer) {
            // [class.static.data]/3：类内初始化只放行 const 整型族（constexpr 前缀
            // 在 parseType 里已 implies const）。其余（非 const / 浮点 / 类类型…）
            // 必须类外定义 —— 拒收文案与 clang 逐字对齐（探针校准，见 docs/learn/44）。
            // 判据对 Const 包裹剥一层再查家族（`const int` 是 Const(Int)，
            //   isInteger() 只看最外层 kind —— 不剥会误拒合法的 const 整型）。
            const TypePtr bare = sm.type->isConst() ? sm.type->stripConst() : sm.type;
            if (!sm.type->isConst()
                || !(bare->isInteger() || bare->isChar())) {
                // 注意 clang 原文不带成员名（"non-const static data member must
                //   be initialized out of line"），逐字对齐不加名字。
                error("non-const static data member must be initialized out of line",
                      decl->location);
            }
            sm.initializer = m_constantFolder.fold(std::move(sm.initializer));
            auto evaluated = m_constantFolder.evaluateIntegral(sm.initializer);
            if (!evaluated) {
                error(std::format(
                    "static member '{}' initializer is not a constant expression",
                    sm.name), decl->location);
            }
            const int64_t value = evaluated->value;
            // 静态常量表：isTypeAccess 折叠（`Cls<Args>::value`）按类作用域+基类链查
            decl->staticConsts[sm.name] = StaticConstMember{value, sm.type};
            // 编译期常量表：裸名 `A::limit` 走常量传播折叠（registerConstVar 的
            // 判据自带 const 整型族过滤，形态取声明类型 —— K<A::limit> 由此可用）。
            ExprPtr initRef = sm.initializer;
            registerConstVar(fullName, sm.type, initRef, sm.isConstexpr, decl->location);
            std::cout << std::format(
                "    [static] ★ 类内初始化：{} = {} : {}（编译期常量，无存储）\n",
                fullName, value, sm.type ? sm.type->toString() : "?");
        }
    }

    // ── 合成默认构造与析构（若未显式提供）──
    bool hasExplicitCtor = false;
    bool hasExplicitDtor = false;
    for (auto& method : decl->methods) {
        if (method->kind == NodeKind::Constructor || method->name == decl->name) {
            hasExplicitCtor = true;
        }
        if (method->kind == NodeKind::Destructor || method->name == "~" + decl->name) {
            hasExplicitDtor = true;
        }
    }

    if (!hasExplicitCtor) {
        auto defaultCtor = std::make_shared<ConstructorDecl>();
        defaultCtor->name = decl->name;
        defaultCtor->ownerClassName = decl->name;
        defaultCtor->returnType = Type::makeVoid();
        defaultCtor->isDefaultCtor = true;
        defaultCtor->body = std::make_shared<BlockStmt>();
        decl->methods.push_back(defaultCtor);
    }
    if (!hasExplicitDtor) {
        auto defaultDtor = std::make_shared<DestructorDecl>();
        defaultDtor->name = "~" + decl->name;
        defaultDtor->ownerClassName = decl->name;
        defaultDtor->returnType = Type::makeVoid();
        defaultDtor->isDefaultDtor = true;
        defaultDtor->body = std::make_shared<BlockStmt>();
        decl->methods.push_back(defaultDtor);
    }

    // ── 校验构造函数初始化列表 ──
    for (auto& method : decl->methods) {
        if (method->kind == NodeKind::Constructor) {
            auto ctor = std::static_pointer_cast<ConstructorDecl>(method);
            for (auto& init : ctor->initList) {
                bool found = false;
                // 检查是否匹配任一基类名
                for (auto& bn : decl->baseClassNames) {
                    if (init.memberName == bn) { found = true; break; }
                }
                // 检查是否匹配自身字段（含限定名）
                for (auto& f : decl->fields) {
                    if (f.name == init.memberName) {
                        // 去掉限定名再比
                        found = true; break;
                    }
                    // 匹配裸名
                    std::string rawName = f.name; // 有可能因为上面操作 成了 "a.name"这种结构
                    auto dot = rawName.find('.');
                    if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                    if (rawName == init.memberName) { found = true; break; }
                }
                if (!found) {
                    error(std::format("Member '{}' not found in class '{}' during initializer list",
                        init.memberName, decl->name), init.location);
                }
            }
        }
    }

    // ── 注册成员模板（[temp.mem]）──
    // 不进 methods：它不是可调用实体，只是一张"配方"（要用实参推导才产出函数）。
    // 单独存一张表，调用点在方法表查不到时来这里查。
    //  蓝图里的形参名（T）必须保留原样直到实例化 —— 与自由函数模板同理。
    for (auto& mt : decl->memberTemplates) {
        mt->funcTemplate->ownerClassName = decl->name;
        m_classMemberTemplates[decl->name].push_back(mt);
        std::cout << std::format(
            "    ↳ member template '{}::{}' registered ({} template parameter(s))\n",
            decl->name, mt->templateName(), mt->templateParams.size());
    }

    // ── 注册方法，检查虚函数 ──
    for (size_t mi = 0; mi < decl->methods.size(); ++mi) {
        auto& method = decl->methods[mi];
        method->ownerClassName = decl->name;

        std::string methodNameInVTable = vtableSlotName(method->name);

        // 虚表槽保存方法声明句柄；符号取自 registerFunction 回填的 mangledName。
        // 类型解析前后不重复定名，避免虚表引用与定义符号不一致。
        std::string selfSignature = paramTypeChain(method->parameters);

        // Override 检测：即使没有 virtual 关键字，也检查是否覆写基类虚函数
        // （C++ 标准：覆写虚函数不需要重新声明 virtual）
        bool overriddenInPrimary = false;
        bool overriddenInSecondary = false;

        // 查主表
        for (auto& entry : classType->classLayout.vtableEntries) {
            std::string entryFuncName = entry.baseFunctionName.empty()
                ? (entry.method ? vtableSlotName(entry.method->name) : std::string())
                : entry.baseFunctionName;
            if (entryFuncName == methodNameInVTable && entry.signature == selfSignature) {
                entry.method = method;   // 覆写 ⇒ 槽位改指本类的实现（索引不变，[class.virtual]/1）
                entry.baseFunctionName = methodNameInVTable;
                entry.isOverridden = true;
                overriddenInPrimary = true;
                // 覆写基类虚函数 → 自身也变成虚函数
                method->isVirtual = true;
                break;
            }
        }

        // 查次表 entries（多继承时次基类的覆写）—— 注意：即使主表已命中也要查，
        // 同名函数可能同时存在于主表和次表（如 whoAmI 同时在 A 和 B 中），两处都要更新。
        for (auto& base : classType->classLayout.bases) {
            if (base.isPrimary || !base.hasVTable) continue;
            for (auto& entry : base.entries) {
                std::string entryFuncName = entry.baseFunctionName.empty()
                    ? (entry.method ? vtableSlotName(entry.method->name) : std::string())
                    : entry.baseFunctionName;
                if (entryFuncName == methodNameInVTable && entry.signature == selfSignature) {
                    entry.method = method;   // 次表里的覆写：同样改指本类实现（thunk 负责 this 调整）
                    entry.baseFunctionName = methodNameInVTable;
                    entry.isOverridden = true;
                    overriddenInSecondary = true; // 覆写了，那么下面就不用在写了
                    // 覆写基类虚函数 → 自身也变成虚函数
                    method->isVirtual = true;
                    // thunk 调整量：-(次基类子对象偏移)
                    entry.thunkAdjust = -(int)base.offset;
                    std::cout << std::format("      ↳ override in secondary '{}': thunkAdjust={}\n",
                        base.baseClassName, entry.thunkAdjust);
                    break;
                }
            }
        }

        std::cout << std::format("    method: {}() → {}{}\n",
            method->name,
            method->returnType ? method->returnType->toString() : "?",
            method->isVirtual ? " [virtual]" :
            method->isOverride ? " [override]" : "");

        if (method->isVirtual) {
            classType->classLayout.hasVTable = true;

            if (!overriddenInPrimary && !overriddenInSecondary) { // 既没有覆盖主虚函数 又没有覆盖次虚函数
                VTableEntry entry;
                entry.method = method;       // 本类新引入的虚函数：新占一个槽位（符号由句柄给出）
                entry.baseFunctionName = methodNameInVTable;
                entry.signature = selfSignature;  // 槽位身份的另一半：形参类型链
                entry.index = static_cast<uint32_t>(
                    classType->classLayout.vtableEntries.size());
                classType->classLayout.vtableEntries.push_back(entry); // 也就是说classLayout 只会放 主基类和自己的虚函数
            }
        }
        std::cout << std::format("  ◀◀ END override check for '{}' (class '{}')\n",
            method->name, decl->name);
    }

    // ── 计算内存布局 ──
    computeClassLayout(decl);

    // ── 注入 vtable 和 RTTI ──
    if (classType->classLayout.hasVTable) {
        injectVTableAndRTTI(classType);
    }

    // 写回最终字段列表：decl->fields（基类+自身，且已被 computeClassLayout
    // 填好 offset/size）整体覆盖前面逐步 push 的版本，作为布局的权威结果。
    classType->classLayout.fields = decl->fields; // 这里时会做一个最终的回填

    // ── 注册到全局符号表：三处登记 —— m_classTypes（类型+布局）、m_classDecls
    //    （AST 声明）、符号表（kind=Type，供名字查找）。
    m_classTypes[decl->name] = classType;
    m_classDecls[decl->name] = decl;
    decl->classType = classType;

    Symbol classSym;
    classSym.name = decl->name;
    classSym.type = classType;
    classSym.kind = SymbolKind::Type;
    classSym.isLocal = false;
    classSym.definedAt = decl->location;
    m_symbolTable.define(decl->name, classSym);

    // ── 注册类内类型别名（using / typedef）──
    // [temp.alias]/[dcl.typedef]：别名不是新类型，只是一次名字替换。两处登记：
    //   ① 符号表里的 "Cls::alias"（供 `S::type x;` 这类限定名走普通名字查找）；
    //   ② 类声明的 typeAliases 表（供 resolveType 的嵌套名分支 / 类作用域回退查，
    //      也供实例化时替换）。
    // 放在类自身注册之后：目标里若引用本类（`using Self = S;`）也已可见。
    if (!decl->typeAliasOrder.empty()) {
        for (const auto& aliasName : decl->typeAliasOrder) {
            TypePtr target = resolveType(decl->typeAliases[aliasName]);
            decl->typeAliases[aliasName] = target; // 写回解析结果，实例化时直接用
            Symbol aliasSym;
            aliasSym.name = decl->name + "::" + aliasName;
            aliasSym.type = target;
            aliasSym.kind = SymbolKind::Type;
            aliasSym.isLocal = false;
            aliasSym.definedAt = decl->location;
            m_symbolTable.define(aliasSym.name, aliasSym);
            std::cout << std::format("  [sema:alias] {}::{} = {}\n",
                decl->name, aliasName, target ? target->toString() : "?");
        }
    }

    // ── 嵌套类（[class.nest]）已在【本类登记之后、字段解析之前】处理完毕 ──
    // （见本函数开头：名字前缀化 + 递归 processClassDecl。）放在那里的理由：
    //   ① 名字在本类作用域内必须【先】可见 —— 否则 `struct A { struct B{}; B b; };`
    //      的字段类型解析查不到尚未登记的 B；
    //   ② 嵌套类里可以引用外层类（`struct B { A* p; };`）—— 此时外层类已登记。
    m_currentClassName = savedClassScope;

    // ── 打印内存布局 ──
    std::cout << std::format("    ══ Memory Layout of '{}' ══\n", decl->name);
    std::cout << std::format("    Total size: {} bytes\n", classType->classLayout.totalSize);
    if (classType->classLayout.hasVTable) {
        std::cout << "    +0: _vptr (8 bytes, hidden) → vtable\n";
    }
    for (auto& f : classType->classLayout.fields) {
        std::cout << std::format("    +{}: {} : {} ({} bytes)\n",
            f.offset, f.name, f.type ? f.type->toString() : "?", f.size);
    }
    if (classType->classLayout.hasVTable) {
        std::cout << std::format("    vtable: {} entries",
            classType->classLayout.vtableEntries.size());
        std::cout << std::format(", RTTI: {}\n", classType->classLayout.rttiMangledName);
    }
    std::cout << "\n";
}

// [basic.align]：字段按 min(大小, 8) 对齐，最终对象大小补齐到最大字段对齐。
// 本实现不支持 packed 或 pragma 布局；继承字段复用基类布局结果。
void SemanticAnalyzer::computeClassLayout(ClassDeclPtr decl) {
    TypePtr classType = decl->classType;
    if (!classType) {
        classType = Type::makeClass(decl->name);
        decl->classType = classType;
    }

    uint32_t offset = 0;
    uint32_t maxAlign = 1;
    bool hasVTable = classType->classLayout.hasVTable;

    // 如果有虚函数，先放 _vptr
    if (hasVTable) {
        offset = 8;
        maxAlign = 8;
    }

    // 基类子对象位置已在 processClassDecl 统一确定；此处只叠加基类内部字段偏移。
    // 不能另按声明顺序重排主基类。
    uint32_t currentFieldOffset = offset;  // 自身字段起始偏移（动态推进）
    std::string prevSrcClass;

    // 先计算自身字段的起始偏移（所有基类子对象之后）
    if (!decl->baseClassNames.empty()) {
        for (auto& base : classType->classLayout.bases) {
            auto baseIt = m_classTypes.find(base.baseClassName);
            if (baseIt != m_classTypes.end()) {
                uint32_t end = base.offset + baseIt->second->classLayout.totalSize;
                if (end > currentFieldOffset) currentFieldOffset = end; // 这里是一种覆盖设置 offset，前面 已经设置过每个 class 的base offset ,每个layout 已经自己对齐过了
            }
        }
        currentFieldOffset = alignTo(currentFieldOffset, 8);// 最后对齐到8 就可以了
    }

    for (auto& field : decl->fields) {
        // ── 自身字段（viaBase 为空）：从基类子对象尾部继续 ──
        // ── size 与 align 分离（对照 clang LayoutField 的 TI.Width / TI.Align）──
        //  类类型字段的 align ≠ size：Five{bool×5} size=5 但 align=1，拿 size 当
        // align 会推出 alignTo(4,5)=9 这种非 2 幂的错位布局。
        if (field.viaBase.empty()) {
            uint32_t fieldAlign = alignOf(field.type);   // 对齐要求（成员 align 递归 max）
            uint32_t fieldSize = field.type->sizeInBytes(); // 实际占用字节, 这里的字节数就是实际 resolveType 解析出来的字节数
            // 对齐的精髓就是 起始地址要能够 整除 filedAlign
            currentFieldOffset = alignTo(currentFieldOffset, fieldAlign);
            field.offset = currentFieldOffset;
            field.size = fieldSize;
            currentFieldOffset += fieldSize; // 加上这个字段的长度，等于当前位置
            maxAlign = std::max(maxAlign, fieldAlign);
            continue;
        }

        // 继承字段偏移 = 直接基类子对象偏移 + 基类布局中原始字段偏移。
        // 用 baseFieldIndex 保留字段来源，避免同名字段映射到错误子对象。
        const BaseSubobject* sub = nullptr;
        for (auto& base : classType->classLayout.bases) {
            if (base.baseClassName == field.viaBase) { sub = &base; break; }
        }
        auto baseIt = m_classTypes.find(field.viaBase);
        const FieldInfo* src = nullptr;
        if (baseIt != m_classTypes.end()) {
            auto& bfields = baseIt->second->classLayout.fields;
            if (field.baseFieldIndex >= 0 &&
                static_cast<size_t>(field.baseFieldIndex) < bfields.size()) {
                src = &bfields[field.baseFieldIndex];
            }
        }
        if (sub && src) {
            field.offset = sub->offset + src->offset;
            field.size   = src->size;
            continue;
        }
        // 兜底：viaBase/下标缺失（不该发生）⇒ 保留扁平化时拷贝来的偏移
        std::cout << std::format(
            "    ⚠ [layout] 字段 '{}' 的 viaBase='{}' 无法定位（下标 {}），沿用扁平化偏移 {}\n",
            field.name, field.viaBase, field.baseFieldIndex, field.offset);
    }

    offset = currentFieldOffset;

    classType->classLayout.totalSize = alignTo(offset, maxAlign); // 最终长度也会进行一个对齐
    if (decl->fields.empty() && hasVTable) {
        classType->classLayout.totalSize = 8;
    }
}

// 固定虚表索引并绑定 RTTI；对象 vptr 指向首方法槽，type_info 位于其前一槽。
void SemanticAnalyzer::injectVTableAndRTTI(TypePtr classType) {
    classType->classLayout.rttiMangledName =
        "_ZTI" + std::to_string(classType->name.size()) + classType->name;

    for (size_t i = 0; i < classType->classLayout.vtableEntries.size(); i++) {
        classType->classLayout.vtableEntries[i].index = static_cast<uint32_t>(i);
    }

    // ── 多继承：计算次表段在 _ZTV 符号内的字节偏移 ──
    // _ZTV 布局：[主表头 16B][主表槽位...][次表头 16B][次表槽位...]...；每段表头 16B
    //   = 8B offset-to-top + 8B RTTI ptr；主表段 vtableSegmentOffset = 0
    //   （vptr 直接指主表头后 16B 处）。
    uint32_t segmentOffset = 16 + static_cast<uint32_t>(
        classType->classLayout.vtableEntries.size()) * 8;
    for (auto& base : classType->classLayout.bases) {
        if (base.isPrimary || !base.hasVTable) continue;
        base.vtableSegmentOffset = segmentOffset;
        segmentOffset += 16 + static_cast<uint32_t>(base.entries.size()) * 8;
        std::cout << std::format("    [MI] secondary '{}' vtableSegmentOffset={}\n",
            base.baseClassName, base.vtableSegmentOffset);
    }
}

// 向上取整到 alignment 的倍数：alignTo(12,8)=16，alignTo(5,4)=8。等价于位运算
// (offset+align-1) & ~(align-1) 的算术写法；alignment==0 时原样返回，防御除零。
uint32_t SemanticAnalyzer::alignTo(uint32_t offset, uint32_t alignment) {
    if (alignment == 0) return offset;
    return (offset + alignment - 1) / alignment * alignment;
}

// 类型的对齐要求（字节）。对照 clang：clang/lib/AST/ASTContext.cpp:ASTContext::getTypeInfoInChars 对 record 返回其
// ASTRecordLayout 的 Alignment（= 成员 align 的递归最大值，见
// clang/lib/AST/RecordLayoutBuilder.cpp:RecordLayoutBuilder.cpp UpdateAlignment），对标量返回 TargetInfo ABI 表值。
// 教学简化：标量 = min(size,8)（int→4 double→8 bool→1 指针/引用→8，与 ABI 表一致）；
// 类类型 = 各成员 alignOf 递归 max（封顶 8）；无成员的空类 → align 1。
//  绝不能拿 sizeInBytes 当 align：Five{bool×5} size=5 align=1，Five{int,int} size=8 align=4。
uint32_t SemanticAnalyzer::alignOf(const TypePtr& type) {
    if (!type) return 1;
    switch (type->kind) {
        case TypeKind::Array:
            return type->elementType ? alignOf(type->elementType) : 1;
        case TypeKind::Const:
            return type->innerType ? alignOf(type->innerType) : 1;
        case TypeKind::Class: {
            uint32_t a = 1;
            for (auto& f : type->classLayout.fields)
                a = std::max(a, alignOf(f.type)); // 会找到 最大 的字段
            return std::min(a, 8u);
        }
        default: {
            uint32_t s = type->sizeInBytes();
            return s == 0 ? 1u : std::min(s, 8u); // Void/未知类型兜底 1
        }
    }
}

// 函数注册统一确定 mangledName，并建立普通调用、重载与符号到声明的索引。
// 调用点、虚表和 CodeGen 均引用该声明上的终态符号。
void SemanticAnalyzer::registerFunction(FuncDeclPtr decl) {
    //  类作用域对【签名】同样可见（[basic.lookup.unqual]）：成员函数的返回类型 /
    // 形参类型可以写外层类的嵌套类或类内别名 —— `struct A { struct B{}; B make(); };`
    // 的返回类型 B 指 A::B。Pass 2 解析签名时必须带上类作用域上下文，
    // 否则这里解析不出 B（Pass 3 的函数体解析已经带上了，见 analyzeFunctionBody）。
    const std::string savedClassScope = m_currentClassName;
    m_currentClassName = decl->ownerClassName;
    decl->returnType = resolveType(decl->returnType);
    for (auto& param : decl->parameters) {
        param.type = resolveType(param.type);
    }
    m_currentClassName = savedClassScope;

    std::string fullName = decl->ownerClassName.empty()
        ? decl->name
        : decl->ownerClassName + "::" + decl->name;

    m_functionMap[fullName] = decl;
    m_functionMap[decl->name] = decl;
    m_functions.push_back(decl);

    // 设置 mangled name
    if (decl->ownerClassName.empty()) {
        decl->mangledName = decl->name;
    } else {
        // 构造函数/方法重载：同名多个时追加参数个数区分（当前方法已 push_back 进
        // m_functions，所以扫到的都是"先注册的" ⇒ 与 vtable 条目点的计数口径一致）。
        size_t sameNameCount = 0;
        if (!decl->name.starts_with("~")) {
            for (auto& f : m_functions) {
                if (f.get() != decl.get()
                    && f->ownerClassName == decl->ownerClassName
                    && f->name == decl->name) {
                    ++sameNameCount;
                }
            }
        }
        //  同一类里"同名且同个数"有多个 ⇒ 名字+个数不足以区分，再挂形参类型链。
        // 开关取自 m_classDecls（= 权威方法表），与 vtable 条目点的 decl->methods 同源。
        bool needsTypeTag = false;
        if (auto clsIt = m_classDecls.find(decl->ownerClassName); clsIt != m_classDecls.end()) {
            needsTypeTag = countSameNameSameArity(clsIt->second->methods, decl->name,
                                                  decl->parameters.size()) > 1;
        }
        // 函数定义与虚表条目共享方法身份判据，不能另写一份命名规则。
        decl->mangledName = memberMethodSymbolName(decl->ownerClassName, decl->name,
                                                   decl->parameters, sameNameCount,
                                                   needsTypeTag);
    }

    // 注册到符号表
    Symbol sym;
    sym.name = decl->name;
    sym.type = decl->returnType;
    sym.kind = SymbolKind::Function;
    sym.isLocal = false;
    sym.ownerClass = decl->ownerClassName;
    sym.definedAt = decl->location;
    m_symbolTable.define(decl->name, sym);

    std::string paramStr;
    for (size_t i = 0; i < decl->parameters.size(); i++) {
        if (i > 0) paramStr += ", ";
        paramStr += decl->parameters[i].type->toString()
                  + " " + decl->parameters[i].name;
    }
    std::cout << std::format("  [register] {}({}) → {}    [mangled: {}]\n",
        fullName, paramStr,
        decl->returnType ? decl->returnType->toString() : "void",
        decl->mangledName);
}

// 内建外部函数只有原型而无函数体，供语义检查与外部链接使用。
// 内存管理函数按实际参数/返回类型登记，CodeGen 不为其生成定义。
void SemanticAnalyzer::registerBuiltins() {
    // 登记一个原型：构造无 body 的 FunctionDecl 并写入三处
    auto declare = [this](const std::string& name, TypePtr ret,
                          std::vector<TypePtr> paramTypes) {
        auto decl = std::make_shared<FunctionDecl>();
        decl->name = name;
        decl->returnType = std::move(ret);
        decl->body = nullptr;  // 纯声明 → CodeGen 跳过发射，链接期由 libc 提供
        for (size_t i = 0; i < paramTypes.size(); i++) {
            Parameter p;
            p.name = "arg" + std::to_string(i);
            p.type = std::move(paramTypes[i]);
            decl->parameters.push_back(std::move(p));
        }
        decl->mangledName = name;          // 外部符号：不修饰，直接用原名
        m_functionMap[name] = decl;
        m_functions.push_back(decl);

        Symbol sym;
        sym.name = name;
        sym.type = decl->returnType;
        sym.kind = SymbolKind::Function;
        sym.isLocal = false;
        m_symbolTable.globalScope()->define(name, sym);  // 显式进全局作用域
    };

    TypePtr intTy  = Type::makeInt();
    TypePtr intPtr = Type::makePointer(intTy);
    TypePtr voidTy = Type::makeVoid();

    declare("malloc", intPtr, {intTy});               // malloc(int n)
    declare("free", voidTy, {intPtr});                // free(int* p)
    declare("memcpy", intPtr, {intPtr, intPtr, intTy}); // memcpy(dst, src, n)
    declare("realloc", intPtr, {intPtr, intTy});      // realloc(p, n)

    std::cout << "  [builtin] ✔ registered libc prototypes: "
                 "malloc(int)→int*, free(int*), "
                 "memcpy(int*,int*,int)→int*, realloc(int*,int)→int*    "
                 "[P4: no body — codegen skips, linker binds libc]\n";

    // 内建 std 垫片直接提供教学实现需要的类型与常量，不依赖完整标准库头文件。
    auto injectTraitClass = [this](const std::string& name, int64_t value,
                                   const std::string& base = "") {
        auto cls = std::make_shared<ClassDecl>();
        cls->name = name;
        if (!base.empty()) cls->baseClassNames.push_back(base);
        // value 的类型是 bool —— 与真 C++ 的 integral_constant<bool,v> 一致
        cls->staticConsts["value"] = StaticConstMember{value, Type::makeBool()};

        // 三处登记（与 processClassDecl 同构）：类表 + 符号表 + 布局
        m_classDecls[name] = cls;
        cls->classType = Type::makeClass(name);
        m_classTypes[name] = cls->classType;

        Symbol sym;
        sym.name = name;
        sym.type = cls->classType;
        sym.kind = SymbolKind::Type;
        sym.isLocal = false;
        m_symbolTable.globalScope()->define(name, sym);
    };

    // 真 C++ 里 false_type/true_type 是 integral_constant<bool,false/true> 的别名；
    // 这里退化成两个各带静态常量 value 的独立类，语义等价。
    injectTraitClass("std::false_type", 0);
    injectTraitClass("std::true_type",  1);

    std::cout << "  [builtin] ✔ registered std shims (主线 H): "
                 "std::false_type{value=0}, std::true_type{value=1}; "
                 "std::void_t<...> → void (按名识别，见 resolveType)    "
                 "[内建注入：绕开别名模板与类内 static 的语法缺口]\n";
}

// 函数体分析建立形参作用域，成员函数额外提供 this，并检查返回类型。
// 作用域结束后恢复外层状态，防止局部名字影响后续函数。
void SemanticAnalyzer::analyzeFunctionBody(FuncDeclPtr decl) {
    if (!decl->body) return;

    std::string funcLabel = decl->ownerClassName.empty()
        ? decl->name
        : decl->ownerClassName + "::" + decl->name;

    std::cout << std::format("\n  ╔══ Function Body: {} ══╗\n", funcLabel);

    m_symbolTable.enterScope(funcLabel);
    // ── 保存外层上下文：实例化（S5）会嵌套调用本函数分析实例体，不保存/恢复会把外层
    //    的 m_currentReturnType 等踩掉（详见 analyzeFunctionBody 的 DEMO 说明）。
    int         savedStackOffset = m_stackOffset;
    std::string savedClassName   = m_currentClassName;
    TypePtr     savedReturnType  = m_currentReturnType;
    std::string savedFuncName    = m_currentFuncName;
    m_stackOffset = 0;
    m_currentClassName = decl->ownerClassName;
    m_currentReturnType = decl->returnType;
    m_currentFuncName = decl->name;

    // ── 注册参数到符号表 ──
    for (auto& param : decl->parameters) {
        m_stackOffset -= 8;

        Symbol sym;
        sym.name = param.name;
        sym.type = param.type;
        sym.kind = SymbolKind::Parameter;
        sym.isLocal = true;
        sym.stackOffset = m_stackOffset;
        sym.definedAt = decl->location;
        m_symbolTable.define(param.name, sym);

        std::cout << std::format("  [param] {} : {}    stack@{}\n",
            param.name, param.type->toString(), m_stackOffset);
    }

    // ── 注册 this 指针（如果是成员函数）──
    if (!decl->ownerClassName.empty()) {
        auto classIt = m_classTypes.find(decl->ownerClassName);
        if (classIt != m_classTypes.end()) {
            m_stackOffset -= 8;

            Symbol thisSym;
            thisSym.name = "this";
            thisSym.type = Type::makePointer(classIt->second);
            thisSym.kind = SymbolKind::Parameter;
            thisSym.isLocal = true;
            thisSym.stackOffset = m_stackOffset;
            m_symbolTable.define("this", thisSym);

            std::cout << std::format("  [param] this : {}*    stack@{}\n",
                decl->ownerClassName, m_stackOffset);
        }
    }

    // ── 分析函数体（不创建新的 block scope，直接在函数 scope 中）──
    for (auto& stmt : decl->body->statements) {
        processStmt(stmt);
    }

    // ── dump 函数作用域符号表 ──
    std::cout << std::format("\n  ── Symbol Table after {} ──\n", funcLabel);
    m_symbolTable.dumpCurrentScope();

    m_symbolTable.exitScope();

    // ── 恢复外层上下文 ──
    m_stackOffset       = savedStackOffset;
    m_currentClassName  = savedClassName;
    m_currentReturnType = savedReturnType;
    m_currentFuncName   = savedFuncName;

    std::cout << std::format("  ╚══ End {} ══╝\n\n", funcLabel);
}

// [temp]：注册模板蓝图，按主模板、偏特化、全特化分别保存。
// 函数模板体待具体实参替换后分析，依赖表达式不得过早拒收。
void SemanticAnalyzer::processTemplateDecl(TemplateDeclPtr decl) {
    std::cout << std::format("  [register] template <");
    for (size_t i = 0; i < decl->typeParams.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << "typename " << decl->typeParams[i];
    }

    if (decl->isClassTemplate()) {
        std::cout << std::format("> {} (class blueprint stored, not analyzed)\n",
            decl->templateName());

        // ── 按 specKind 归档（[temp.class.spec] / [temp.expl.spec]）──
        // 主模板进 m_classTemplates（名字→蓝图 O(1)）；偏特化/全特化进各自的 vector，
        // 索引仍用模板名。三张表分开是因为匹配算法不同（见 selectClassTemplate）。
        if (decl->isPrimary()) {
            // ── 类模板注册表（与函数模板候选集对称）：名字→蓝图 O(1)；emplace 不覆盖，
            //    重名取先注册者。
            auto [it, inserted] =
                m_classTemplates.emplace(decl->templateName(), decl);
            std::cout << std::format("    ↳ class template '{}' registered{}\n",
                decl->templateName(),
                inserted ? "" : " (duplicate name, first registration wins)");
        }
        else if (decl->isPartialSpec()) {
            auto& list = m_partialSpecs[decl->templateName()];
            list.push_back(decl);
            std::cout << std::format(
                "    ↳ PARTIAL specialization #{} of '{}' registered\n",
                list.size(), decl->templateName());
        }
        else { // [temp.expl.spec]：全特化模式必须完全具体，不能残留待推导形参。
            for (const auto& arg : decl->specPattern) {
                // 值位（`template <> struct Flag<true>`）：值本就是具体量，
                //   不存在"名字未声明"的问题，跳过。
                if (!arg.isType()) continue;
                const TypePtr& pat = arg.type;
                if (!pat) continue;
                // 模式里挂着模板参数节点 = 形参表空却用了形参名，必是笔误
                if (pat->isTemplateParam()) {
                    error(std::format(
                        "use of undeclared template parameter '{}' in explicit "
                        "specialization of '{}' —— `template <>` 的形参表是空的，"
                        "模式里不能出现模板形参名",
                        pat->templateParamName, decl->templateName()), SourceLocation{});
                }
                // 裸标识符被建成 Class 节点：必须是已登记的类
                if (pat->isClass() && !pat->name.empty() &&
                    !m_classDecls.count(pat->name) &&
                    !m_classTemplates.count(pat->name) &&
                    pat->templateArgs.empty()) {
                    error(std::format(
                        "use of undeclared identifier '{}' in explicit "
                        "specialization pattern of '{}' —— `template <>` 的形参表是"
                        "空的，模式里每个名字都必须是【真实类型】（如 int / MyClass）；"
                        "若本意是偏特化，请把形参写进 template<...> 并改用 "
                        "`template <class {}> struct {}<...>`",
                        pat->name, decl->templateName(), pat->name,
                        decl->templateName()), SourceLocation{});
                }
            }

            auto& list = m_explicitSpecs[decl->templateName()];
            list.push_back(decl);
            std::cout << std::format(
                "    ↳ EXPLICIT (full) specialization #{} of '{}' registered\n",
                list.size(), decl->templateName());
        }
    } else if (decl->isDeductionGuide()) {
        // ── 推导指引（[temp.deduct.guide]）：只有 template<> 外壳的那种走这里；非模板
        // 形态（`Box(int) -> Box<int>;`）是顶层独立声明，由 processDecl 直接接住。
        //  它不做任何"实例化"—— 指引没有实体、没有符号，只是 CTAD 的规则表。
        std::cout << std::format("> deduction guide for '{}' (rule stored, never instantiated)\n",
            decl->templateName());
        registerDeductionGuide(decl->guide);
    }
    else if (decl->isAliasTemplate()) {
        // ── 别名模板（[temp.alias]）：注册形态与另外两种模板都不同 ——
        //   类模板   → m_classTemplates，将来按需"实例化"出新的类；
        //   函数模板 → 候选集，将来在调用点做实参推导 + "实例化"；
        //   别名模板 → m_aliasTemplates，将来在【解析类型】时"解糖"。
        // 别名不需要候选集（不能重载）、不需要特化表（偏特化别名模板必须是另一个别名
        // 模板，本项目不做）、不产生符号。
        auto [it, inserted] =
            m_aliasTemplates.emplace(decl->templateName(), decl);
        std::cout << std::format(
            "> using {} = {} (alias blueprint stored, expands by substitution only)\n",
            decl->templateName(),
            decl->aliasTemplate->underlyingType
                ? decl->aliasTemplate->underlyingType->toString() : "?");
        std::cout << std::format("    ↳ alias template '{}' registered{}\n",
            decl->templateName(),
            inserted ? "" : " (duplicate name, first registration wins)");
    } else {
        // ── 函数模板（S1）：只注册蓝图，不分析函数体 —— 模板体中的 T 是"依赖类型"，
        //    要到实例化时（S5，由调用点推导驱动）才能做类型检查 = 两阶段查找的第一阶段。
        auto& func = decl->funcTemplate;
        std::string paramStr;
        for (size_t i = 0; i < func->parameters.size(); i++) {
            if (i > 0) paramStr += ", ";
            paramStr += func->parameters[i].type->toString() + " "
                      + func->parameters[i].name;
        }
        std::cout << std::format("> {}({}) → {} (function blueprint stored, awaiting call-site deduction)\n",
            func->name, paramStr,
            func->returnType ? func->returnType->toString() : "void");

        auto& candidates = m_functionTemplateCandidates[func->name];
        candidates.push_back(decl);
        std::cout << std::format("    ↳ overload candidate set '{}' size = {}\n",
            func->name, candidates.size());
    }

    m_templates.push_back(decl);
}

// ═══ 语句处理 ══════════════════════════════════════════════════════════════
// 语句分发器：AstVisitor 虚分派（handler 只收引用、不需要所有权，判据见 processDecl）。
void SemanticAnalyzer::processStmt(const StmtPtr& stmt) {
    if (!stmt) return;
    // 一次虚表跳转落到对应 visit（零 RTTI：kind 恒等于自身类型）。
    stmt->accept(*this);
}

void SemanticAnalyzer::visit(DeleteStmt& stmt) {
    TypePtr ptrType = inferType(stmt.pointerExpr);
    if (!ptrType || !ptrType->isPointer()) {
        error("delete operand must be a pointer", stmt.location);
    }
    std::cout << std::format("  [delete] delete {}{}\n",
        stmt.isArray ? "[] " : "", ptrType->toString());
}

// 复合语句 `{ … }` → 新建块作用域（[basic.scope.block]）：块内声明的变量在 exitScope
// 后不再可见。demo: while 体内 `{ int t = 0; … }` —— t 只在块内可见，块外引用 t 会报
// Undefined variable。
void SemanticAnalyzer::visit(BlockStmt& block) {
    m_symbolTable.enterScope("block");
    for (auto& stmt : block.statements) {
        processStmt(stmt);
    }
    m_symbolTable.exitScope();
}

// [dcl.spec.auto]：从初始化式推导占位符，并保留声明中的引用/const 结构。
// `auto x = expr` 与 `auto& r = expr` 的推导结果不能简单复制为同一类型。
// 初始化后回填类型、构造目标或 sret 信息，供 CodeGen 直接消费。
bool SemanticAnalyzer::containsAuto(const TypePtr& t) const {
    if (!t) return false;
    if (t->isAuto()) return true;
    if (t->isConst())     return containsAuto(t->innerType);
    if (t->isPointer())   return containsAuto(t->pointeeType);
    if (t->isReference()) return containsAuto(t->referencedType);
    return false;
}

// auto 推导等价于相应模板形参的匹配：按值会退化，引用需保留绑定对象类型。
// `auto x = a` 得到值类型；`auto& r = a` 保留引用。
TypePtr SemanticAnalyzer::deduceAutoType(TypePtr pattern, TypePtr init,
                                         const std::string& varName, SourceLocation loc) {
    if (!pattern) return nullptr;
    if (pattern->isAuto()) return init;              //  绑定点：auto := A
    if (pattern->isConst())
        return Type::makeConst(deduceAutoType(pattern->innerType, init, varName, loc));
    if (pattern->isPointer()) {
        // `auto*` 要求初始化式确实是指针，且 A 要剥掉一层才能与内层模式对齐
        if (!init || !init->isPointer())
            error(std::format(
                "cannot deduce 'auto*' for '{}': initializer is '{}', not a pointer",
                varName, init ? init->toString() : "?"), loc);
        return Type::makePointer(
            deduceAutoType(pattern->pointeeType, init->pointeeType, varName, loc));
    }
    if (pattern->isLValueReference())
        return Type::makeLValueReference(
            deduceAutoType(pattern->referencedType, init, varName, loc));
    if (pattern->isRValueReference())
        return Type::makeRValueReference(
            deduceAutoType(pattern->referencedType, init, varName, loc));
    return pattern;  // 不含 auto 的外壳（containsAuto 为真时不可达）
}

void SemanticAnalyzer::visit(VarDeclStmt& decl) {
    // ── CTAD：`MyPtr m(7);` → `MyPtr<int> m(7);`（[dcl.type.class.deduct]）──
    //  必须在 resolveType【之前】：resolveType 见到裸类模板名会直接报 "requires
    // template arguments"，而这里正是要把它补上。不适用 CTAD 时原样返回，后路不变。
    if (!decl.ctorArgs.empty()) {
        decl.declaredType = deduceClassTemplateArgs(decl);
    }

    decl.declaredType = resolveType(decl.declaredType);
    TypePtr type = decl.declaredType;

    // 初始化式先做静态常量折叠（Cls<Args>::value → 字面量），必须在 inferType 之前。
    if (decl.initializer) decl.initializer = foldStaticConst(decl.initializer);

    // 常量折叠（foldStaticConst 管"类内静态常量"，这里管纯字面量算术）
    if (decl.initializer) decl.initializer = m_constantFolder.fold(std::move(decl.initializer));

    // const/constexpr 整型变量登记编译期常量表
    // （常量传播源 + NTTP 实参重解的查询目标；constexpr 强制可求值）
    registerConstVar(decl.name, decl.declaredType, decl.initializer,
                     decl.isConstexpr, decl.location);

    // 类性判定剥除顶层 const/引用，原类型仍保留在日志与符号表中。
    // 例如 `const Box<int>` 的构造与拷贝不能因外层 Const 包装而遗漏。
    auto bareForClass = [](const TypePtr& t) {
        return t && t->isConst() ? t->stripConst() : t;
    };

    if (decl.initializer) {
        // ── 推导初始化表达式的类型 ──
        std::cout << std::format("  [var decl] {} : {} = ",
            decl.name, type->toString());

        m_inferDepth++;
        TypePtr initType = inferType(decl.initializer);
        m_inferDepth--;

        std::cout << std::format("{}    ⟹ inferred: {}\n",
            inferIndent(), initType ? initType->toString() : "?");

        // ─── auto 类型推导（[dcl.spec.auto]/7）───
        //  判据是 containsAuto 而非 isAuto：const auto 的树是 Const(Auto)，
        // 光杆判据看不见占位符 ⇒ 落到 else 分支被当成"声明 const auto、实得 int"误报。
        if (containsAuto(type)) {
            if (!initType || initType->isAuto() || initType->isVoid()) {
                error(std::format("Cannot deduce auto type for '{}'", decl.name),
                      decl.location);
            }

            // auto 推导保留声明中的 const、指针与引用结构。
            // 日志打 pattern 原文：裸 auto 仍输出 "auto"（与旧日志一字不差），
            // `const auto` / `auto*` / `auto&` 则原样可见。
            TypePtr declaredAuto = type;
            TypePtr deduced = deduceAutoType(type, initType, decl.name, decl.location);
            decl.declaredType = deduced;
            type = deduced;

            std::cout << std::format("  [auto] ★ {} : {} ⟹ {}   ← auto 被永久替换!\n",
                decl.name, declaredAuto->toString(), type->toString());
        }
        else {
            // ── 类型检查（含 [conv.lval] 引用剥除 / 数值提升）──
            if (!type->equals(initType)) {
                if (typeCompatible(type, initType)) {
                    std::cout << std::format("  [conv] {} : {} ⟶ {} (implicit: ref-strip / promotion)\n",
                        decl.name,
                        initType ? initType->toString() : "?", type->toString());
                }
                else if (type->isPointer() && initType && initType->isPointer()) {
                    std::cout << std::format("  [poly] {} : {} → {} (polymorphic)\n",
                        decl.name, initType->toString(), type->toString());
                }
                else {
                    error(std::format(
                        "Type mismatch in '{}': declared '{}', got '{}'",
                        decl.name, type->toString(), initType->toString()),
                        decl.location);
                }
            }
        }

        // ── 表达式级构造调用升级：`Box<int> b = Box<int>(7);` / `auto b = Box<int>(7);` ──
        // inferType 已把 RHS 判成构造调用（isCtorCall）。拷贝初始化语境里临时对象是
        // 纯浪费 —— 直接升级成【直接初始化】：构造进 b 的槽（拷贝省略，
        // [class.copy.elision] 允许且 clang 默认这么做），临时对象永不物化。
        //  放在 if/else 之外：auto 变量 `auto b = Box<int>(7);` 反推完类型后
        // 同样要升级，否则 CodeGen 走"默认构造"分支发不存在的 0 参符号。
        if (bareForClass(type)->isClass() && decl.initializer->kind == NodeKind::Call) {
            auto ce = std::static_pointer_cast<CallExpr>(decl.initializer);
            if (ce->isCtorCall) {
                if (!ce->ctorClassType->equals(bareForClass(type))) {
                    error(std::format(
                        "Type mismatch in '{}': declared '{}', got '{}'",
                        decl.name, type->toString(),
                        ce->ctorClassType->toString()),
                        decl.location);
                }
                decl.ctorArgs = ce->arguments;
                decl.ctorSymbol =
                    std::static_pointer_cast<VarExpr>(ce->callee)->name;
                decl.initializer = nullptr;
                std::cout << std::format(
                    "  [ctor] ★ {} : {}({} 个实参) ⇒ 升级直接初始化（拷贝省略），构造符号 {}\n",
                    decl.name, type->toString(), decl.ctorArgs.size(),
                    decl.ctorSymbol);
            }
            // `S x = f();` 使用 x 自身的槽作为 sret 缓冲，调用结果直接落位。
            // 只有按值返回类对象的调用可走此通道，引用返回不得误判。
            else if (ce->resolvedType && bareForClass(ce->resolvedType)->isClass()) {
                if (!bareForClass(ce->resolvedType)->equals(bareForClass(type))) {
                    error(std::format(
                        "Type mismatch in '{}': declared '{}', got '{}'",
                        decl.name, type->toString(),
                        bareForClass(ce->resolvedType)->toString()),
                        decl.location);
                } else {
                    decl.initIsSretCall = true;
                    std::string calleeName = (ce->callee->kind == NodeKind::Var)
                        ? std::static_pointer_cast<VarExpr>(ce->callee)->name
                        : (ce->callee->kind == NodeKind::Member)
                            ? std::static_pointer_cast<MemberExpr>(ce->callee)->memberName
                            : "?";
                    std::cout << std::format(
                        "  [sret] ★ {} : {} = {}() ⇒ 返回缓冲落在本变量槽（拷贝省略）\n",
                        decl.name, type->toString(), calleeName);
                }
            }
        }
        // 同类左值初始化且无用户拷贝构造时执行平凡复制，如 `Box<int> b = a;`。
        // 保留初始化式，不能退回无初始化的默认构造路径。
        else if (bareForClass(type)->isClass() && initType && bareForClass(initType)->isClass()
                 && bareForClass(type)->equals(bareForClass(initType))
                 && (decl.initializer->kind == NodeKind::Var
                     || decl.initializer->kind == NodeKind::Member)) {
            decl.isTrivialCopy = true;
            std::cout << std::format(
                "  [ctor] ★ {} : {} = {} ⇒ 平凡拷贝（无用户拷贝构造，按布局 memcpy）\n",
                decl.name, type->toString(), initType->toString());
        }
    } else if (containsAuto(type)) {
        // 含占位符但无初始化式（`const auto x;` 同样要拦）
        error(std::format("auto variable '{}' must have an initializer", decl.name),
              decl.location);
    } else if (!decl.ctorArgs.empty()) {
        // 直接初始化 `T x(args);` 由 Sema 选择构造声明并回填符号。
        // CodeGen 不能依据类名及参数个数另行推算有状态的重载名字。
        m_inferDepth++;
        std::vector<TypePtr> ctorArgTypes;
        ctorArgTypes.reserve(decl.ctorArgs.size());
        for (auto& a : decl.ctorArgs) ctorArgTypes.push_back(inferType(a));
        m_inferDepth--;

        if (bareForClass(type)->isClass()) {
            auto cit = m_classDecls.find(bareForClass(type)->name);
            if (cit != m_classDecls.end()) {
                std::vector<FuncDeclPtr> ctors;
                for (auto& method : cit->second->methods) {
                    if (method->kind != NodeKind::Constructor) continue;
                    if (method->parameters.size() != decl.ctorArgs.size()) continue;
                    ctors.push_back(method);
                }
                if (auto best = pickBestByArgs(ctors, ctorArgTypes,
                        std::format("ctor {}({} args)", type->toString(), ctorArgTypes.size()))) {
                    decl.ctorSymbol = best->mangledName;
                }
            }
        }
        if (decl.ctorSymbol.empty()) {
            error(std::format(
                "no matching constructor for '{}' with {} argument(s) —— "
                "'{}' 里找不到接受 {} 个实参的构造函数",
                decl.name, decl.ctorArgs.size(),
                type->toString(), decl.ctorArgs.size()),
                decl.location);
        }
        std::cout << std::format("  [ctor] ★ {} : {}({} 个实参) ⇒ 选定构造函数符号 {}\n",
            decl.name, type->toString(), decl.ctorArgs.size(), decl.ctorSymbol);
    } else {
        // 默认构造 `A a;` 同样回填选中的构造符号，与带参构造共享定名来源。
        if (bareForClass(type)->isClass()) {
            auto cit = m_classDecls.find(bareForClass(type)->name);
            if (cit != m_classDecls.end()) {
                for (auto& method : cit->second->methods) {
                    if (method->kind != NodeKind::Constructor) continue;
                    if (!method->parameters.empty()) continue;
                    decl.ctorSymbol = method->mangledName;
                    break;
                }
            }
        }
        std::cout << std::format("  [var decl] {} : {} (no initializer)\n",
            decl.name, type->toString());
    }

    // ── 注册到符号表：分配栈槽，统一 8 字节/变量（教学简化），栈向低地址生长，
    //    该偏移与 CodeGen 的栈帧布局一一对应。
    m_stackOffset -= 8;

    Symbol sym;
    sym.name = decl.name;
    sym.type = type;
    sym.kind = SymbolKind::Variable;
    sym.isLocal = true;
    sym.stackOffset = m_stackOffset;
    sym.definedAt = decl.location;

    if (!m_symbolTable.define(decl.name, sym)) {
        error(std::format("Variable '{}' already declared in this scope", decl.name),
              decl.location);
    }

    std::cout << std::format("  [symbol] ✚ {} : {}    stack@{}   ← 加入符号表\n",
        decl.name, type->toString(), m_stackOffset);
}

// if 语句：条件必须可语境转换为 bool（[stmt.select]），教学级简化为只接受 bool | int。
// then / else 分支各自进入独立作用域（[basic.scope.block]），分支内声明的变量互不可见。
void SemanticAnalyzer::visit(IfStmt& stmt) {
    stmt.condition = m_constantFolder.fold(std::move(stmt.condition));
    std::cout << std::format("  [if] condition:\n");
    m_inferDepth++;
    TypePtr condType = inferType(stmt.condition);
    m_inferDepth--;
    std::cout << std::format("  [if] condition type: {}\n",
        condType ? condType->toString() : "?");

    // any 整型（含 bool/char/…）都可作条件（[stmt.select]/1 的"上下文转换为 bool"）
    if (condType && !condType->isInteger()) {
        error("If condition must be bool or int", stmt.location);
    }

    m_symbolTable.enterScope("if-then");
    processStmt(stmt.thenBranch);
    m_symbolTable.exitScope();

    if (stmt.elseBranch) {
        m_symbolTable.enterScope("if-else");
        processStmt(stmt.elseBranch);
        m_symbolTable.exitScope();
    }
}

// while 语句：推导条件类型（教学级未强制 bool|int，比 if 宽松），循环体进入独立作用域。
// 循环体只静态分析一遍 —— 动态的反复执行是运行期的事，语义分析只保证"每一遍都类型合法"。
void SemanticAnalyzer::visit(WhileStmt& stmt) {
    stmt.condition = m_constantFolder.fold(std::move(stmt.condition));
    std::cout << std::format("  [while] condition:\n");
    m_inferDepth++;
    TypePtr condType = inferType(stmt.condition);
    m_inferDepth--;
    std::cout << std::format("  [while] condition type: {}\n",
        condType ? condType->toString() : "?");

    m_loopDepth++;                               // break/continue 的合法性范围
    m_symbolTable.enterScope("while-body");
    processStmt(stmt.body);
    m_symbolTable.exitScope();
    m_loopDepth--;
}

// for 语句 [stmt.for]：init → cond → body → incr 的静态检查各来一遍。
// init 的【作用域贯穿整个循环体】（C++ 里 for-init 声明的名字在 cond、incr、
// body 里都可见），所以 enterScope 包全程而非只包 init；
// incr 是表达式不是语句（`i++` 写成 `i = i + 1`），走 inferType 定型即可。
void SemanticAnalyzer::visit(ForStmt& stmt) {
    std::cout << std::format("  [for] init:\n");
    m_symbolTable.enterScope("for-init");

    processStmt(stmt.init);

    if (stmt.condition) {
        stmt.condition = m_constantFolder.fold(std::move(stmt.condition));
        std::cout << std::format("  [for] condition:\n");
        m_inferDepth++;
        TypePtr condType = inferType(stmt.condition);
        m_inferDepth--;
        std::cout << std::format("  [for] condition type: {}\n",
            condType ? condType->toString() : "?");
    } else {
        std::cout << std::format("  [for] condition: <empty> (for(;;))\n");
    }

    if (stmt.increment && stmt.increment->kind != NodeKind::Empty) {
        std::cout << std::format("  [for] increment:\n");
        processStmt(stmt.increment);             // 语句（AssignStmt/ExprStmt）
    }

    m_loopDepth++;                               // break/continue 的合法性范围
    m_symbolTable.enterScope("for-body");
    processStmt(stmt.body);
    m_symbolTable.exitScope();
    m_loopDepth--;

    m_symbolTable.exitScope();
}

// ── 以下 visit 属主线 C 二批（switch/do-while，[stmt.switch]/[stmt.do]）──
// switch [stmt.switch]：条件定型 + 体访问（case 值的常量性/重复检测在 visit(CaseStmt)）。
// m_switchDepth 只管 break 合法性；continue 仍只看 m_loopDepth —— switch 里
// 写 continue 照旧是 clang 的 "'continue' statement not in loop statement"。
void SemanticAnalyzer::visit(SwitchStmt& stmt) {
    m_inferDepth++;
    TypePtr condType = inferType(stmt.condition);
    m_inferDepth--;
    std::cout << std::format("  [switch] condition: {} —— 进入 switch 体\n",
                             condType ? condType->toString() : "?");
    m_switchDepth++;
    m_caseValueSets.emplace_back();
    stmt.body->accept(*this);
    m_caseValueSets.pop_back();
    m_switchDepth--;
    std::cout << "  [switch] 退出 switch 体\n";
}

// case 标号：值必须折叠成整数字面量（clang 文案逐字），重复值响亮报错。
void SemanticAnalyzer::visit(CaseStmt& stmt) {
    if (m_caseValueSets.empty()) {
        error("'case' statement not in switch statement", stmt.location);
    }
    stmt.value = m_constantFolder.fold(std::move(stmt.value));
    auto val = m_constantFolder.evaluateIntegral(stmt.value);
    if (!val.has_value()) {
        error("case value is not a constant expression", stmt.location);
    }
    if (!m_caseValueSets.back().insert(val->value).second) {
        error(std::format("duplicate case value '{}'", val->value), stmt.location);
    }
    stmt.constValue = val->value;
    stmt.resolved   = true;
    std::cout << std::format("  [case] case {} ⇒ 标号成立\n", val->value);
    stmt.body->accept(*this);
}

void SemanticAnalyzer::visit(DefaultStmt& stmt) {
    stmt.body->accept(*this);
}

// do-while [stmt.do]：体先于条件执行一轮；continue 落条件式（同 while）。
void SemanticAnalyzer::visit(DoWhileStmt& stmt) {
    m_loopDepth++;
    stmt.body->accept(*this);
    stmt.condition = m_constantFolder.fold(std::move(stmt.condition));
    m_inferDepth++;
    TypePtr condType = inferType(stmt.condition);
    m_inferDepth--;
    m_loopDepth--;
    std::cout << std::format("  [do-while] 体先执行，条件 {} 在末尾测试\n",
                             condType ? condType->toString() : "?");
}

void SemanticAnalyzer::visit(BreakStmt& stmt) {
    if (m_loopDepth == 0 && m_switchDepth == 0) {
        // 文案逐字沿用 clang（"or switch" 现在名副其实：switch 已实装）。
        error("'break' statement not in loop or switch statement", stmt.location);
    }
    std::cout << std::format("  [break] loop-depth={} switch-depth={} ✓\n",
                             m_loopDepth, m_switchDepth);
}

// continue 语句 [stmt.cont]：同 break，跳到最近一层循环的"增量步"。
void SemanticAnalyzer::visit(ContinueStmt& stmt) {
    if (m_loopDepth == 0) {
        error("'continue' statement not in loop statement", stmt.location);
    }
    std::cout << std::format("  [continue] loop-depth={} ✓\n", m_loopDepth);
}

// [expr.cond]：条件与两分支分别定型；当前要求两分支同型，未实现完整公共类型合成。
// 生成阶段只求值被选中的分支。
TypePtr SemanticAnalyzer::inferConditional(ConditionalExpr& expr) {
    std::cout << std::format("{}[cond] condition:\n", inferIndent());
    m_inferDepth++;
    TypePtr condType = inferType(expr.condition);
    TypePtr thenType = inferType(expr.thenExpr);
    TypePtr elseType = inferType(expr.elseExpr);
    m_inferDepth--;

    if (!thenType || !elseType) return nullptr;
    if (!typeCompatible(thenType, elseType)) {
        error(std::format("incompatible operand types in conditional expression: '{}' and '{}'",
                          thenType->toString(), elseType->toString()),
              expr.location);
    }
    TypePtr t = thenType;
    std::cout << std::format("{}[cond] {} ? {} : {} → {}\n",
        inferIndent(),
        condType ? condType->toString() : "?",
        thenType->toString(), elseType->toString(),
        t->toString());
    return t;
}

// 逗号表达式 [expr.comma]：类型取【右】操作数；左操作数照常定型（副作用成立性）。
TypePtr SemanticAnalyzer::inferComma(CommaExpr& expr) {
    m_inferDepth++;
    inferType(expr.left);
    TypePtr rightType = inferType(expr.right);
    m_inferDepth--;
    std::cout << std::format("{}[comma] , {} → {}（值取右操作数）\n",
        inferIndent(), rightType ? rightType->toString() : "?",
        rightType ? rightType->toString() : "?");
    return rightType;
}

// return 语句 [stmt.return]：返回值类型必须与函数声明的返回类型兼容（typeCompatible
// 负责剥引用/顶层 const、int→double 提升）。demo: int f() 中 `return x;`（x:int&）✓；
// `return 3.14;` ✗ 报 Return type mismatch；void 函数带返回值也被拦下。
// 不做流分析："非 void 函数漏写 return / 并非所有路径都有 return"不检查（clang 在
// CheckReturnVal 之外还有 CFG 流敏感检查，此处为教学级简化）。
void SemanticAnalyzer::visit(ReturnStmt& stmt) {
    if (stmt.value) {
        stmt.value = m_constantFolder.fold(std::move(stmt.value));
        m_inferDepth++;
        TypePtr retType = inferType(stmt.value);
        m_inferDepth--;

        std::cout << std::format("  [return] type: {} (expected: {})\n",
            retType ? retType->toString() : "?",
            m_currentReturnType ? m_currentReturnType->toString() : "?");

        // 类型检查（含 [conv.lval] 引用剥除 / 数值提升）
        TypePtr expType = resolveType(m_currentReturnType);
        if (retType && expType
            && !typeCompatible(expType, retType)) {
            error(std::format(
                "Return type mismatch: expected '{}', got '{}'",
                expType->toString(), retType->toString()),
                stmt.location);
        }
    } else {
        std::cout << "  [return] void\n";
    }
}

// 赋值语句：分别推导左值（target）与右值（value）的类型并打日志。
// 教学级简化：不检查"左侧必须是可修改左值"、不检查左右类型兼容性（clang 在
// SemaExpr.cpp 的 CheckAssignmentOperands 中完成这两件事）；左值性目前只在 inferCall
// 的模板推导路径中发挥作用。
void SemanticAnalyzer::visit(AssignStmt& stmt) {
    std::cout << "  [assign] lhs:\n";
    m_inferDepth++;
    TypePtr targetType = inferType(stmt.target);
    m_inferDepth--;

    stmt.value = m_constantFolder.fold(std::move(stmt.value));
    std::cout << "  [assign] rhs:\n";
    m_inferDepth++;
    TypePtr valueType = inferType(stmt.value);
    m_inferDepth--;

    std::cout << std::format("  [assign] {} ⟵ {} \n",
        targetType ? targetType->toString() : "?",
        valueType ? valueType->toString() : "?");

    // 同类对象赋值由 Sema 标记，CodeGen 按布局逐字节复制。
    // 例如 `b = a;`、`b = Box<int>(9);`；本实现不提供 operator= 重载。
    if (targetType && valueType && targetType->isClass()
        && targetType->equals(valueType)) {
        stmt.isClassAssign = true;
        stmt.assignClassType = targetType;
    }
}

// 表达式语句在语义阶段执行符号决议与类型检查，运行结果由 CodeGen 丢弃。
// 空语句无类型可解析；裸名声明候选由此检查可见性。
void SemanticAnalyzer::visit(EmptyStmt& stmt) {
    if (stmt.typeName.empty()) return;

    // ── 第一问：它是不是【此处可见的类型】？──
    //   判据单点：与 resolveType 的兜底链、visit(ExprStmt) 的 [stmt.ambig] 折叠
    //   走的是同一个 lookupVisibleType（红线 3）。
    VisibleType hit = lookupVisibleType(stmt.typeName);
    if (hit.type) {
        std::cout << std::format(
            "  [sema:stmt] '{}' 可见的类型名 ⇒ 判为无声明符声明\n", stmt.typeName);
        return;
    }

    // 模板名单独成行如 `Box;` 也可作为无声明符声明候选，需与类型名分别查询。
    if (isVisibleTemplateName(stmt.typeName)) {
        std::cout << std::format(
            "  [sema:stmt] '{}' 可见的模板名 ⇒ 判为无声明符声明\n", stmt.typeName);
        return;
    }

    // ── 第二问：这个名字【声明过】吗？──
    //   声明过但不是类型 ⇒ 它是表达式语句（clang：warning expression result
    //   unused，rc=0）⇒ 走表达式路径，让既有的表达式 machinery 处理。
    if (isNameDeclared(stmt.typeName)) {
        auto var = std::make_shared<VarExpr>(stmt.typeName);
        var->location = stmt.location;
        auto exprStmt = std::make_shared<ExprStmt>(var);
        exprStmt->location = stmt.location;
        processStmt(exprStmt);
        return;
    }

    // ── 都没声明过 ⇒ 响亮拒收 ──
    //   对照 clang：这类程序 clang 走的是【表达式】分支（ClassifyName 判"S 不是
    //   类型名"⇒ 表达式语句 ⇒ 名字未声明），文案是 use of undeclared identifier。
    //   本实现走的是【声明】分支，但语义结论相同（这个名字此处不可用），故
    //   沿用 clang 的文案以便逐字对齐。
    error(std::format("use of undeclared identifier '{}'", stmt.typeName),
          stmt.location);
}

void SemanticAnalyzer::visit(ExprStmt& stmt) {
    // [stmt.ambig]：无声明符候选如 `N::S;` 由 lookupVisibleType 检查可见性。
    // 未知名字仍需诊断，不能当普通空语句放行。
    if (stmt.expr && stmt.expr->kind == NodeKind::Var) {
        auto& var = static_cast<VarExpr&>(*stmt.expr);
        VisibleType hit = lookupVisibleType(var.name);
        if (hit.type || isVisibleTemplateName(var.name)) {
            stmt.expr = nullptr;
            return;
        }
    }

    stmt.expr = m_constantFolder.fold(std::move(stmt.expr));
    if (stmt.expr) {
        m_inferDepth++;
        inferType(stmt.expr);
        m_inferDepth--;
    }
}

// 表达式推导回填 resolvedType，并补齐调用目标、成员偏移及值类别信息。
// 下游只消费已解析结果；依赖表达式留待模板替换后再分析。
TypePtr SemanticAnalyzer::inferType(ExprPtr expr) {
    if (!expr) return nullptr;

    // 一次 switch（跳表）分派。static_cast 安全：节点 kind 由构造函数设定，恒等于自身类型。
    // 各分支上方的 `数据事例` 行 = 「输入表达式 ⇒ 该表达式在日志里的定型行」，
    // 全部出自 demos/core/03_infer_branches.cpp 的编译输出（文件头有复现命令）。
    TypePtr type = nullptr;
    switch (expr->kind) {
        // ① 数据事例：42 ⇒ [infer] IntLiteral(42) → int
        case NodeKind::IntLiteral:
            type = inferIntLiteral(static_cast<IntLiteralExpr&>(*expr)); break;
        // ② 数据事例：'A' ⇒ [infer] CharLiteral(65) → char
        //    （括号里是 ASCII 码 —— 词法期已把 'A' 折成 65；多字符 'ab' 折成 24930）
        case NodeKind::CharLiteral:
            type = inferCharLiteral(static_cast<CharLiteralExpr&>(*expr)); break;
        // ③ 数据事例：true ⇒ [infer] BoolLiteral(true) → bool
        case NodeKind::BoolLiteral:
            type = inferBoolLiteral(static_cast<BoolLiteralExpr&>(*expr)); break;
        // ④ 数据事例："hi" ⇒ [infer] StringLiteral → char*
        case NodeKind::StringLiteral:
            type = inferStringLiteral(static_cast<StringLiteralExpr&>(*expr)); break;
        // ⑤ 数据事例：nullptr ⇒ [infer] nullptr → void*
        //    （int* ln = nullptr; 时接着走 [poly] ln : void* → int*）
        case NodeKind::NullptrLiteral:
            type = inferNullptrLiteral(static_cast<NullptrLiteralExpr&>(*expr)); break;
        // ⑥ 数据事例：x ⇒ [resolve] 'x' → int    (kind=Variable, stack@-48)
        case NodeKind::Var:
            type = inferVar(static_cast<VarExpr&>(*expr)); break;
        // ⑦ 数据事例：x + y ⇒ [infer] int op int → int
        //              x < y ⇒ [infer] int op int → bool    (comparison)
        case NodeKind::Binary:
            type = inferBinary(static_cast<BinaryExpr&>(*expr)); break;
        // ⑧ 数据事例：!cmp ⇒ [infer] !bool → bool        -x    ⇒ [infer] -int → int
        //              &x   ⇒ [infer] &int → int*（取地址）  *addr ⇒ [infer] *int* → int（解引用）
        case NodeKind::Unary:
            type = inferUnary(static_cast<UnaryExpr&>(*expr)); break;
        // ⑨ 数据事例：add(x, y) ⇒ [call] add(2 args) → int    [普通查找：形参类型精确匹配]
        //              dr.kind() ⇒ [call] Derived.kind(0 args) → int    [class-scoped member call]
        //              twice(x)  ⇒ [call] twice → _Z5twiceIiET_T_ (template resolved) → int
        case NodeKind::Call:
            type = inferCall(static_cast<CallExpr&>(*expr)); break;
        // 成员访问最终携带字段类型和布局偏移，CodeGen 不再按名字重查字段。
        case NodeKind::Member:
            type = inferMember(static_cast<MemberExpr&>(*expr)); break;
        // ⑪ 数据事例：new Base() ⇒ [new] Base → Base*    (size=16 bytes, args=0)
        case NodeKind::New:
            type = inferNew(static_cast<NewExpr&>(*expr)); break;
        // ⑫ 数据事例：this->b（Base::self 体内）⇒ [this] → Base*，随后 [member] Base.b → int
        case NodeKind::This:
            type = inferThis(static_cast<ThisExpr&>(*expr)); break;
        // ⑬ 数据事例：dynamic_cast<Derived*>(pb) ⇒ [dynamic_cast] Base* → Derived*    (runtime RTTI check)
        case NodeKind::DynamicCast:
            type = inferDynamicCast(static_cast<DynamicCastExpr&>(*expr)); break;
        // ⑭ 数据事例：v[0] ⇒ [index] IntVec[int] → int    (sugar for IntVec.at(i))
        case NodeKind::Index:
            type = inferIndex(static_cast<IndexExpr&>(*expr)); break;
        // ⑭.5 数据事例：a > b ? a : b ⇒ [cond] int ? int : int → int
        case NodeKind::Conditional:
            type = inferConditional(static_cast<ConditionalExpr&>(*expr)); break;
        case NodeKind::Comma:
            type = inferComma(static_cast<CommaExpr&>(*expr)); break;
        // ⑮ 数据事例：delete nb ⇒ 先 [resolve] 'nb' → Base*，再 [delete] delete Base*；
        //    前 14 支都满足"返回值 = 该节点的类型"（inferXxx 直传），只有本支不是。
        case NodeKind::Delete: {
            // delete 是 void 表达式：先看操作数成不成立，自身类型恒为 void
            auto& del = static_cast<DeleteExpr&>(*expr);
            inferType(del.pointerExpr);
            type = Type::makeVoid();
            break;
        }
        default:
            break;   // 不会到达：NodeKind 的表达式种类已全部覆盖
    }

    expr->resolvedType = type;
    return type;
}

// 字面量类型由字面量种类及保存的数值形态决定；字符串与 nullptr 使用专用类型。
TypePtr SemanticAnalyzer::inferIntLiteral(IntLiteralExpr& expr) {
    // 形态优先：`4L` 是 long、`4u` 是 unsigned int（[lex.icon]/2 的表）；
    // 未标注形态时按 int（既有路径不变，日志也一字不变）。
    TypePtr t = expr.literalType ? expr.literalType : Type::makeInt();
    std::cout << std::format("{}[infer] IntLiteral({}) → {}\n",
        inferIndent(), expr.value, t->toString());
    return t;
}

TypePtr SemanticAnalyzer::inferCharLiteral(CharLiteralExpr& expr) {
    // [lex.ccon]/1：普通字符字面量的类型是 char（不是 int）—— 值域 0..255 之内。
    // 这条"类型是 char"的性质要一路带到 CodeGen（按 1 字节存取）与推导/重载
    // （`f('a')` 与 `f(97)` 是不同类型）。
    std::cout << std::format("{}[infer] CharLiteral({}) → char\n",
        inferIndent(), expr.value);
    return Type::makeChar();
}

TypePtr SemanticAnalyzer::inferBoolLiteral(BoolLiteralExpr& expr) {
    std::cout << std::format("{}[infer] BoolLiteral({}) → bool\n",
        inferIndent(), expr.value ? "true" : "false");
    return Type::makeBool();
}

TypePtr SemanticAnalyzer::inferStringLiteral(StringLiteralExpr&) {
    std::cout << std::format("{}[infer] StringLiteral → char*\n", inferIndent());
    return Type::makePointer(Type::makeInt());
}

TypePtr SemanticAnalyzer::inferNullptrLiteral(NullptrLiteralExpr&) {
    std::cout << std::format("{}[infer] nullptr → void*\n", inferIndent());
    return Type::makePointer(Type::makeVoid());
}

// 变量名按作用域由内向外查找，再处理当前类成员与限定访问。
// 限定名不应退回无关作用域中的裸名。
TypePtr SemanticAnalyzer::inferVar(VarExpr& expr) {
    // [expr.prim.id.qual]：限定名字按完整路径查找；`A::x` 不能误命中其他作用域的 x。
    if (auto sep = expr.name.rfind("::"); sep != std::string::npos
        && !m_currentClassName.empty()) {
        const std::string qualifier = expr.name.substr(0, sep);
        const std::string member    = expr.name.substr(sep + 2);
        if (isClassOrBaseOf(m_currentClassName, qualifier)) {
            QualifiedLookupResult hit =
                resolveQualifiedField(m_currentClassName, qualifier, member);
            if (!hit.valid()) {
                error(std::format("no member named '{}' in '{}'", member, qualifier),
                      expr.location);
            }
            if (hit.statik) {
                // 静态数据成员不占对象布局，回填注册全名以便 CodeGen 走全局符号寻址。
                // 可折叠的类内静态常量在更早阶段替换为字面量。
                expr.name = hit.statik->name;
                std::cout << std::format(
                    "{}[resolve] '{}' → {}    (static member, 全局符号 {})\n",
                    inferIndent(), hit.statik->name,
                    hit.statik->type ? hit.statik->type->toString() : "?",
                    hit.statik->name);
                return hit.statik->type;
            }
            const FieldInfo* f = hit.field;
            // 回填位置 + 把名字还原成裸名：CodeGen 两条路径（读 / 写）优先吃回填值，
            // 未回填时才按名字查（那时裸名就是原来的语义，不会更糟）。
            expr.resolvedFieldOffset = static_cast<int>(f->offset);
            expr.resolvedFieldSize   = f->size;
            expr.name = member;
            std::cout << std::format(
                "{}[resolve] '{}::{}' → {}    (qualified member, offset={} 隐式 this)\n",
                inferIndent(), qualifier, member,
                f->type ? f->type->toString() : "?", f->offset);
            return f->type;
        }
    }

    // 1. 查符号表（从当前作用域向外搜索）
    Symbol* sym = m_symbolTable.lookup(expr.name);
    if (sym) {
        TypePtr resolvedType = sym->type;

        // 如果是类类型，返回注册表中的完整类型（包含布局信息）
        if (resolvedType && resolvedType->isClass()) {
            auto classIt = m_classTypes.find(resolvedType->name);
            if (classIt != m_classTypes.end()) {
                resolvedType = classIt->second;
            }
        }

        std::cout << std::format(
            "{}[resolve] '{}' → {}    (kind={}, {})\n",
            inferIndent(),
            expr.name,
            resolvedType ? resolvedType->toString() : "?",
            symbolKindName(sym->kind),
            sym->isLocal ? std::format("stack@{}", sym->stackOffset) : "global");

        return resolvedType;
    }

    // 2. 如果在类方法中，查类的字段
    if (!m_currentClassName.empty()) {
        auto classIt = m_classTypes.find(m_currentClassName);
        if (classIt != m_classTypes.end()) {
            auto field = classIt->second->classLayout.findField(expr.name);
            if (field) {
                std::cout << std::format(
                    "{}[resolve] '{}' → {}    (class field, offset={})\n",
                    inferIndent(), expr.name,
                    field->type ? field->type->toString() : "?",
                    field->offset);
                return field->type;
            }
        }

        // [class.static.data]：成员函数内的裸静态成员名解析到所属类的独立存储或常量。
        {
            std::vector<std::string> work = {m_currentClassName};
            std::set<std::string> visited;
            while (!work.empty()) {
                std::string cur = work.back();
                work.pop_back();
                if (!visited.insert(cur).second) continue;
                if (Symbol* s = m_symbolTable.lookup(cur + "::" + expr.name)) {
                    expr.name = s->name;
                    std::cout << std::format(
                        "{}[resolve] '{}' → {}    (static member, 全局符号 {})\n",
                        inferIndent(), s->name,
                        s->type ? s->type->toString() : "?", s->name);
                    return s->type;
                }
                auto it = m_classDecls.find(cur);
                if (it == m_classDecls.end()) continue;
                for (auto& bn : it->second->baseClassNames) work.push_back(bn);
                //  外层类（类作用域链的上一环，嵌套类用）：嵌套类是外层类的成员，
                // 它的作用域包含外层类的作用域 ⇒ 嵌套类里能看见外层类的 static 成员
                // （[class.nest]/1 + [class.member.lookup]）。与基类链【并列】推进 ——
                // 两条链都是"名字查找要走的路"，但含义不同（继承 vs 嵌套）。
                if (!it->second->ownerClassName.empty()) {
                    work.push_back(it->second->ownerClassName);
                }
            }
        }
    }

    // 3. 检查是否是函数名
    auto funcIt = m_functionMap.find(expr.name);
    if (funcIt != m_functionMap.end()) {
        std::cout << std::format("{}[resolve] '{}' → function\n",
            inferIndent(), expr.name);
        return funcIt->second->returnType;
    }

    // 裸名未声明时使用 `use of undeclared identifier` 诊断。
    // 参考：clang/lib/Sema/SemaExpr.cpp:Sema::DiagnoseEmptyLookup。
    if (expr.name.find("::") == std::string::npos) {
        error(std::format("use of undeclared identifier '{}'", expr.name),
              expr.location);
    } else {
        error(std::format("Undefined variable '{}'", expr.name), expr.location);
    }
}

// [expr.arith.conv]：比较与逻辑运算得到 bool，算术运算按教学版数值兼容规则合并类型。
// 操作数类型检查与结果类型推导在此统一处理。
TypePtr SemanticAnalyzer::inferBinary(BinaryExpr& expr) {
    std::cout << std::format("{}[infer] BinaryExpr(op) left:\n", inferIndent());

    m_inferDepth++;
    TypePtr leftType = inferType(expr.left);
    std::cout << std::format("{}[infer] BinaryExpr(op) right:\n", inferIndent());
    TypePtr rightType = inferType(expr.right);
    m_inferDepth--;

    // 比较运算符返回 bool
    if (expr.op == BinaryOp::Eq || expr.op == BinaryOp::Neq
        || expr.op == BinaryOp::Lt || expr.op == BinaryOp::Gt
        || expr.op == BinaryOp::Le || expr.op == BinaryOp::Ge
        || expr.op == BinaryOp::And || expr.op == BinaryOp::Or) {

        std::cout << std::format(
            "{}[infer] {} {} {} → bool    (comparison)\n",
            inferIndent(),
            leftType ? leftType->toString() : "?",
            "op",
            rightType ? rightType->toString() : "?");
        return Type::makeBool();
    }

    // 算术运算符
    if (leftType && rightType) {
        TypePtr resultType;
        if (leftType->isDouble() || rightType->isDouble()) {
            resultType = Type::makeDouble();
        } else if (leftType->isInteger() && rightType->isInteger()) {
            //  整型提升（[conv.prom]/1）：比 int 窄的（bool/char/short）先提升到 int，
            // 两个操作数同为整型时结果一律按 int 取。本项目不做完整的
            // usual arithmetic conversions（`unsigned + int` 该取 unsigned）——
            // 那是 [expr.arith.conv] 的整张表，属后续项，此处保持"整型 ⇒ int"的
            // 教学口径，与既有行为一致。
            resultType = Type::makeInt();
        } else {
            resultType = Type::makeInt(); // 默认
        }

        std::cout << std::format(
            "{}[infer] {} op {} → {}\n",
            inferIndent(),
            leftType->toString(),
            rightType->toString(),
            resultType->toString());
        return resultType;
    }

    error("Invalid binary operation types", expr.location);
}

// [expr.unary.op]：! 得 bool，& 生成指针，* 取所指类型；负号保留数值类型。
TypePtr SemanticAnalyzer::inferUnary(UnaryExpr& expr) {
    m_inferDepth++;
    TypePtr operandType = inferType(expr.operand);
    m_inferDepth--;

    if (expr.op == UnaryOp::Not) {
        std::cout << std::format("{}[infer] !{} → bool\n",
            inferIndent(), operandType ? operandType->toString() : "?");
        return Type::makeBool();
    }

    // ── 解引用 *p（[expr.unary.op]/1）──
    // 结果类型 = 指针的 pointee（被指类型），且结果是【左值】—— 故 `*p = v` 合法。
    // 操作数必须是指针：C++ 里没有指针不做隐式转换（C 也没有），非指针即错误。
    // demo: int a; int* p = &a;  *p → int（左值）
    // 对照 clang：clang/lib/Sema/SemaExpr.cpp:CheckIndirectionOperand
    //   （文件级 static 函数，不是 Sema 成员）
    if (expr.op == UnaryOp::Deref) {
        if (!operandType) {
            error("Cannot dereference a null-typed operand", expr.location);
        }
        if (!operandType->isPointer()) {
            // 文案对齐 clang：indirection requires pointer operand ('X' invalid)
            error(std::format(
                "Indirection requires pointer operand ('{}' invalid)",
                operandType->toString()), expr.location);
        }
        TypePtr result = operandType->pointeeType;
        std::cout << std::format(
            "{}[infer] *{} → {}    (解引用 [expr.unary.op]/1)\n",
            inferIndent(), operandType->toString(),
            result ? result->toString() : "?");
        return result;
    }

    // ── 取地址 &x（[expr.unary.op]/3）──
    // 结果类型 = Pointer(操作数类型)，操作数必须是左值。
    // demo: int a;  &a → int*  —— 也就是 Box<decltype(&a)> 里的实参类型。
    if (expr.op == UnaryOp::Addr) {
        if (!operandType) {
            error("Cannot take the address of a null-typed operand", expr.location);
        }
        if (operandType->isVoid()) {
            error("Cannot take the address of a void expression", expr.location);
        }
        TypePtr result = Type::makePointer(operandType);
        std::cout << std::format("{}[infer] &{} → {}    (取地址 [expr.unary.op]/3)\n",
            inferIndent(), operandType->toString(), result->toString());
        return result;
    }

    std::cout << std::format("{}[infer] -{} → {}\n",
        inferIndent(),
        operandType ? operandType->toString() : "?",
        operandType ? operandType->toString() : "?");
    return operandType;
}

// [over.match]：调用先收集候选，再检查实参与引用绑定，最后选择最佳可行函数。
// 选中的声明同时决定返回类型和调用符号，避免两者取自不同候选。
// 参考：clang/lib/Sema/SemaOverload.cpp。
TypePtr SemanticAnalyzer::inferCall(CallExpr& expr) {
    // ── 确定 callee 名字与形态 ──
    std::string funcName;
    std::shared_ptr<VarExpr> calleeVar;
    if (expr.callee->kind == NodeKind::Var) {
        calleeVar = std::static_pointer_cast<VarExpr>(expr.callee);
        funcName = calleeVar->name;
    } else if (expr.callee->kind == NodeKind::Member) {
        auto mem = std::static_pointer_cast<MemberExpr>(expr.callee);
        funcName = mem->memberName;
        mem->isMethodCall = true;
        // 先推导 callee 成员表达式，给 mem->object 标注 resolvedType（如 shape → Shape*）。
        // CodeGen::emitCall 依赖该类型解引用出类名、查 vtable 条目，从而把 shape->area()
        // 降级为经 _vptr 的间接调用；否则下方 m_functionMap 命中方法名后提前返回，对象
        // 类型永远不会被推导，虚调用退化为直接 callq。
        //  只对 MemberExpr 补推导 —— 普通函数/模板的 VarExpr callee 若在此推导，会因
        // 符号表查无此"变量"而误报 Undefined variable。
        if (mem->isTypeAccess) {
            resolveTypeQualifier(mem->object);
        } else {
            m_inferDepth++;
            inferType(mem);
            m_inferDepth--;
        }
    }

    // [basic.lookup.qual]：限定调用先解析限定者的可见类型，再使用登记名查询方法。
    // 外层 A 内的 `B::sm()` 可解析到登记键 `A::B::sm`。
    if (auto sep = funcName.rfind("::"); sep != std::string::npos) {
        const std::string qualifier = funcName.substr(0, sep);
        const std::string member    = funcName.substr(sep + 2);
        VisibleType qt = lookupVisibleType(qualifier);
        if (qt.type && qt.type->isClass()) {
            const std::string registered = qt.type->name + "::" + member;
            if (registered != funcName) {
                std::cout << std::format(
                    "{}[call] 限定名 '{}' ⇒ 登记名 '{}'（嵌套类的可见名 ≠ 登记名）\n",
                    inferIndent(), funcName, registered);
                funcName = registered;
                if (calleeVar) calleeVar->name = registered;
            }
        }
    }

    // [declval]：内建 std::declval<T>() 提供 T&& 类型，只用于不求值语境的类型推导。
    if (calleeVar && (funcName == "std::declval" || funcName == "declval")) {
        if (!calleeVar->explicitTemplateArgs.empty() &&
            calleeVar->explicitTemplateArgs[0].type) {
            TypePtr t = calleeVar->explicitTemplateArgs[0].type;
            TypePtr r = Type::makeRValueReference(t);
            std::cout << std::format(
                "{}[declval] std::declval<{}>() → {}    [内建：按名识别，[declval]/1]\n",
                inferIndent(), t->toString(), r->toString());
            return r;
        }
    }

    // ── 推导实参类型（并记录左值性：变量/成员访问是左值）──
    // 左值性是推导的输入：T& 绑定检查、万能引用折叠（S2）
    std::vector<TypePtr> argTypes;
    std::vector<bool>    argIsLValue;
    for (auto& arg : expr.arguments) {
        m_inferDepth++;
        TypePtr t = inferType(arg);
        m_inferDepth--;
        argTypes.push_back(t);
        // 实参值类别随类型一起传入推导与绑定检查；不能只比较剥除引用后的基础类型。
        bool isLValue = arg->kind == NodeKind::Var || arg->kind == NodeKind::Member
                     || arg->kind == NodeKind::Index
                     || (arg->kind == NodeKind::Unary
                         && std::static_pointer_cast<UnaryExpr>(arg)->op == UnaryOp::Deref);
        argIsLValue.push_back(isLValue);
        std::cout << std::format("{}  arg: {}{}\n", inferIndent(),
            t ? t->toString() : "?", isLValue ? " (lvalue)" : " (rvalue)");
    }

    // [dcl.init.ref]：普通函数与模板调用均检查引用绑定，不能仅用 typeCompatible 放行。
    auto argsBindTo = [&](const FuncDeclPtr& f, std::string* why, size_t* badIdx) {
        for (size_t i = 0; i < f->parameters.size() && i < argIsLValue.size(); ++i) {
            if (!referenceBindsValueCategory(f->parameters[i].type, argIsLValue[i], why)) {
                if (badIdx) *badIdx = i;
                return false;
            }
        }
        return true;
    };
    auto checkArgBinding = [&](const FuncDeclPtr& f) {
        std::string why;
        size_t badIdx = 0;
        if (!argsBindTo(f, &why, &badIdx)) {
            error(std::format("no matching function for call to '{}' — {} ('{}')",
                              funcName, why,
                              badIdx < argTypes.size() && argTypes[badIdx]
                                  ? argTypes[badIdx]->toString() : "?"),
                  expr.location);
        }
    };

    // ── 成员方法调用：按"对象类 → 方法表"查，不走全局名字表 ──
    // Box_int::get 与 Box_double::get 这类同名方法（模板实例或多类同名成员）在全局
    // m_functionMap 里互相覆盖（registerFunction 以裸名作键），直接查名字表会随机命中
    // 别的类的方法，返回类型/参数全错。对照 clang：成员调用走 UnqualifiedIdExpr 的
    // 类作用域限定查找（BuildMemberCallExpr → LookupMember），普通名字查找只是兜底。
    // 前提：先补推导 callee 成员表达式，拿到 mem->object 的 resolvedType。
    if (expr.callee->kind == NodeKind::Member) {
        auto mem = std::static_pointer_cast<MemberExpr>(expr.callee);
        if (!mem->object->resolvedType) {
            m_inferDepth++;
            inferType(mem->object);
            m_inferDepth--;
        }
        TypePtr objType = mem->object->resolvedType;
        if (objType && objType->isPointer()) objType = objType->pointeeType;
        if (objType && objType->isClass()) {
            // 搜索对象类及其所有基类（BFS）的方法
            std::vector<std::string> searchQueue = {objType->name};
            std::set<std::string> searched;
            while (!searchQueue.empty()) {
                std::string clsName = searchQueue.back(); searchQueue.pop_back();
                if (!searched.insert(clsName).second) continue;
                auto classIt = m_classDecls.find(clsName);
                if (classIt != m_classDecls.end()) {
                    if (auto method = findMethodInClass(clsName, funcName,
                                                        static_cast<int>(argTypes.size()),
                                                        &argTypes)) {
                        //  回填符号：CodeGen 硬拼 `类名_方法名` 拼不出带参方法的
                        // 真符号 —— Sema 给带参成员方法名加了"参数个数"后缀
                        // （IntVec_at_1 / IntVec_set_2），硬拼得到的是 IntVec_at。
                        // 虚调用不受影响：CodeGen 在前面的 vtable 分支就 return 了。
                        if (mem->isTypeAccess && !method->isStatic)
                            error("call to non-static member function without an object argument",
                                  expr.location);
                        checkArgBinding(method);
                        mem->resolvedCalleeSymbol = method->mangledName;
                        std::cout << std::format(
                            "{}[call] {}.{}({} args) → {}    [class-scoped member call{}]\n",
                            inferIndent(), objType->name, funcName, argTypes.size(),
                            method->returnType ? method->returnType->toString() : "?",
                            clsName != objType->name ? std::format(" via '{}'", clsName) : "");
                        return method->returnType;
                    }
                    // ── 成员模板（[temp.mem]）：普通方法表里没有，查成员的模板表 ──
                    //  位置刻意放在"方法表查完之后"：普通成员函数优先（重载决议里
                    // 非模板候选本来就优先于模板候选，[over.match.best]）。
                    if (auto mtIt = m_classMemberTemplates.find(clsName);
                        mtIt != m_classMemberTemplates.end()) {
                        for (auto& mt : mtIt->second) {
                            if (mt->templateName() != funcName) continue;
                            FuncDeclPtr inst = getOrInstantiateMemberFunction(
                                mt, argTypes, argIsLValue, clsName, expr.location);
                            if (!inst) continue;   // 推导失败 ⇒ 试下一个同名成员模板
                            // 回填符号：CodeGen 的成员调用硬拼 `类名_方法名`，
                            // 而成员模板的多个实例各有符号，必须由这里指定。
                            if (mem->isTypeAccess && !inst->isStatic)
                                error("call to non-static member function without an object argument",
                                      expr.location);
                            mem->resolvedCalleeSymbol = inst->mangledName;
                            std::cout << std::format(
                                "{}[call] {}.{}({} args) → {}    "
                                "[member template instantiated]\n",
                                inferIndent(), objType->name, funcName, argTypes.size(),
                                inst->returnType ? inst->returnType->toString() : "?");
                            return inst->returnType;
                        }
                    }
                    // [class.member.lookup]：当前类找到同名成员后即停止，匹配失败也不回退基类或普通函数。
                    // 隐藏规则按名字生效，不取决于本次实参能否匹配。
                    std::vector<const std::vector<Parameter>*> sameName;
                    if (auto declIt = m_classDecls.find(clsName); declIt != m_classDecls.end()) {
                        for (auto& m : declIt->second->methods) {
                            if (m->name == funcName) sameName.push_back(&m->parameters);
                        }
                    }
                    if (auto mtAll = m_classMemberTemplates.find(clsName);
                        mtAll != m_classMemberTemplates.end()) {
                        for (auto& mt : mtAll->second) {
                            if (mt->templateName() != funcName) continue;
                            if (mt->isFunctionTemplate())
                                sameName.push_back(&mt->funcTemplate->parameters);
                        }
                    }
                    if (!sameName.empty()) {
                        size_t have = argTypes.size();
                        size_t lo = SIZE_MAX, hi = 0;
                        const std::vector<Parameter>* narrowest = nullptr;  // 供"单个形参"文案
                        for (auto* ps : sameName) {
                            if (ps->size() < lo) { lo = ps->size(); narrowest = ps; }
                            if (ps->size() > hi) hi = ps->size();
                        }
                        auto paramName = [](const std::vector<Parameter>* ps, size_t i) {
                            return (ps && i < ps->size() && !(*ps)[i].name.empty())
                                 ? (*ps)[i].name : std::string("x");
                        };
                        if (have < lo) {
                            if (lo == 1) {
                                error(std::format(
                                    "too few arguments to function call, single argument '{}' "
                                    "was not specified", paramName(narrowest, 0)),
                                    expr.location);
                            }
                            error(std::format(
                                "too few arguments to function call, expected {}, have {}",
                                lo, have), expr.location);
                        }
                        if (have > hi) {
                            if (hi == 1) {
                                error(std::format(
                                    "too many arguments to function call, expected single "
                                    "argument '{}', have {} arguments",
                                    paramName(narrowest, 0), have), expr.location);
                            }
                            error(std::format(
                                "too many arguments to function call, expected {}, have {}",
                                hi, have), expr.location);
                        }
                        // 个数在范围内但没有任何候选可行 ⇒ [overload.match]/1 的非良构
                        error(std::format(
                            "no matching member function for call to '{}' in class '{}' "
                            "({} argument(s) given; 实参类型无法按 [overload.viable] 转换)",
                            funcName, clsName, have),
                            expr.location);
                    }
                    // 搜基类
                    for (auto& bn : classIt->second->baseClassNames)
                        searchQueue.insert(searchQueue.begin(), bn);
                }
            }
        }
        // 类里没查到 → 落入下方既有路径（兼容既有行为与报错）
    }

    // [basic.lookup.unqual]/[basic.lookup.argdep]：普通查找与实参关联命名空间的候选合并后统一择优。
    // ADL 增加候选，不是普通查找失败后的备用入口。
    if (calleeVar) {
        std::string tplName = funcName;
        if (!m_classTemplates.count(tplName) && !m_aliasTemplates.count(tplName)
            && !m_currentNamespace.empty()) {
            std::string qualified = m_currentNamespace + "::" + funcName;
            if (m_classTemplates.count(qualified) || m_aliasTemplates.count(qualified)) {
                tplName = qualified;
            }
        }
        if (m_classTemplates.count(tplName) || m_aliasTemplates.count(tplName)) {
            // 显式实参逐个解析：类型实参递归 resolveType，NTTP 值实参原样带过
            // （类模板支持 NTTP —— `Buf<4>(...)`，与函数模板路径"见值即拒"不同）。
            std::vector<TemplateArg> targs;
            targs.reserve(calleeVar->explicitTemplateArgs.size());
            for (auto& a : calleeVar->explicitTemplateArgs) {
                targs.push_back(a.isType() && a.type
                    ? TemplateArg::ofType(resolveType(a.type)) : a);
            }
            TypePtr tid = Type::makeClass(tplName);
            tid->templateArgs = std::move(targs);
            // 裸模板名 + 实参表 ⇒ CTAD；否则实例化/解糖。别名解糖的产物可能还是
            // 模板 id，再过一次 resolveType 落地（与 learn/27 的"替换≠实例化"同款）。
            TypePtr instType = tid->templateArgs.empty()
                ? deduceCtadCore(tid, argTypes, funcName)
                : resolveType(tid);
            instType = resolveType(instType);
            if (!instType || !instType->isClass()) {
                error(std::format(
                    "construction target '{}' is not a class type",
                    instType ? instType->toString() : "?"), expr.location);
            }
            auto cit = m_classDecls.find(instType->name);
            if (cit != m_classDecls.end()) {
                std::vector<FuncDeclPtr> ctors;
                for (auto& method : cit->second->methods) {
                    if (method->kind != NodeKind::Constructor) continue;
                    if (method->parameters.size() != argTypes.size()) continue;
                    ctors.push_back(method);
                }
                if (auto best = pickBestByArgs(ctors, argTypes,
                        std::format("ctor-expr {}({} args)",
                            instType->toString(), argTypes.size()))) {
                    expr.isCtorCall = true;
                    expr.ctorClassType = instType;
                    calleeVar->name = best->mangledName;  // CodeGen callq 目标
                    std::cout << std::format(
                        "{}[ctor-expr] ★ {}({} args) ⇒ 选定构造函数符号 {}    [临时对象构造]\n",
                        inferIndent(), instType->toString(), argTypes.size(),
                        best->mangledName);
                    return instType;
                }
            }
            error(std::format(
                "no matching constructor for '{}' with {} argument(s)",
                instType->toString(), argTypes.size()), expr.location);
        }
    }

    auto it = m_functionMap.find(funcName);

    // ── ① 关联命名空间：从实参类型反推（[basic.lookup.argdep]/2）──
    // 本实现把命名空间成员的名字前缀化成 "N::S"，故从类名反推即可：
    //   N::S ⇒ {N}；A::B::S ⇒ {A, A::B}
    // 标准还包含基类与模板实参的关联命名空间，本实现只做"类名自身的前缀"。
    std::vector<std::string> assocNs;
    for (const auto& t : argTypes) {
        TypePtr core = t;
        while (core && (core->isPointer() || core->isLValueReference() ||
                        core->isRValueReference() || core->isConst())) {
            core = core->isPointer()  ? core->pointeeType
                 : core->isConst()    ? core->innerType
                                      : core->referencedType;
        }
        if (!core || !core->isClass()) continue;
        const std::string& n = core->name;
        for (size_t pos = n.find("::"); pos != std::string::npos; pos = n.find("::", pos + 2)) {
            assocNs.push_back(n.substr(0, pos));
        }
    }

    // ── ② 合并候选：普通查找（裸名）+ ADL（限定名）──
    // 剥引用与顶层 const：调用 `get(s)` 时形参写 S、实参类型是 S 的左值，两者在精确匹配
    // 判据里应当看作同一个类型（[dcl.init.ref] 的绑定规则不改变"是不是同一个类型"）。
    auto stripRefConst = [](TypePtr t) {
        while (t && (t->isLValueReference() || t->isRValueReference() || t->isConst())) {
            t = t->isConst() ? t->innerType : t->referencedType;
        }
        return t;
    };
    auto arityOk = [&](const FuncDeclPtr& f) {
        return f->parameters.size() == argTypes.size();
    };
    auto exactMatch = [&](const FuncDeclPtr& f) {
        if (!arityOk(f)) return false;
        for (size_t i = 0; i < argTypes.size(); i++) {
            TypePtr pt = stripRefConst(f->parameters[i].type);
            TypePtr at = stripRefConst(argTypes[i]);
            if (!pt || !at || !pt->equals(at)) return false;
        }
        return true;
    };

    struct Candidate { FuncDeclPtr decl; std::string via; };  // ADL 仅收集普通自由函数，排除成员方法和模板实例产物，避免重复或越界候选。
    auto isPlainFreeFunction = [](const FuncDeclPtr& f) {
        return f->ownerClassName.empty() && f->mangledName == f->name;
    };
    std::vector<Candidate> pool;
    for (auto& f : m_functions) {
        if (!isPlainFreeFunction(f)) continue;
        if (f->name == funcName) {
            pool.push_back({f, "普通查找"});
        }
    }
    for (const auto& ns : assocNs) {
        std::string qualified = ns + "::" + funcName;
        for (auto& f : m_functions) {
            if (!isPlainFreeFunction(f)) continue;
            if (f->name == qualified) pool.push_back({f, std::format("ADL({})", ns)});
        }
    }

    // ── ③ 裁决：精确匹配优先；无精确匹配时退回既有行为 ──
    //  可行性含**引用绑定**（[overload.viable]）：不可绑定的候选在这里就出局，
    // 而不是等选完冠军再被 checkArgBinding 拦下 —— `f(int&)` 与 `f(const int&)`
    // 并存时 `f(5)` 该选后者（clang 的答案），先选后退会把合法的程序拒掉。
    Candidate* winner = nullptr;
    for (auto& c : pool) {
        if (!arityOk(c.decl)) continue;
        if (exactMatch(c.decl) && argsBindTo(c.decl, nullptr, nullptr)) {
            winner = &c; break;   // 平手取注册顺序靠前者
        }
    }
    if (winner) {
        // 冠军必然已过引用绑定（上面的循环里 argsBindTo 是入选条件之一），
        //   故此处不再复查 —— 判据只留一处，避免"两处判据各自演化"的老毛病。
        bool isAdl = winner->via.rfind("ADL", 0) == 0;
        std::cout << std::format(
            "{}[call] {}({} args) → {}    [{}{}：形参类型精确匹配]\n",
            inferIndent(), funcName, argTypes.size(),
            winner->decl->returnType ? winner->decl->returnType->toString() : "?",
            winner->via, isAdl ? "，" + funcName + " 走 ADL 限定查找" : "");
        //  名字原地改写：CodeGen 按名字发射 callq，ADL 命中的是
        // "N::get" 而不是 "get"（再由 asmSymbol 净化成 N__get）。
        // 与 resolveTemplateCall 把名字改成 mangled 符号是同一手法。
        if (isAdl && calleeVar) calleeVar->name = winner->decl->ownerClassName.empty()
            ? winner->decl->name : winner->decl->ownerClassName + "::" + winner->decl->name;
        return winner->decl->returnType;
    }

    // ── ④ 退回既有路径：精确匹配一个都没有 ──
    if (it != m_functionMap.end()) {
        if (it->second->parameters.size() == argTypes.size()) {
            checkArgBinding(it->second);   // 引用绑定检查后，限定调用也须改写为选定声明的发射符号，不能保留源码拼写。
            if (calleeVar && !it->second->mangledName.empty()
                && it->second->mangledName != funcName) {
                std::cout << std::format(
                    "{}[call] 限定名 '{}' → 发射符号 '{}'（成员函数 mangled）\n",
                    inferIndent(), funcName, it->second->mangledName);
                calleeVar->name = it->second->mangledName;
            }
            std::cout << std::format(
                "{}[call] {}({} args) → {}    [symbol resolved, non-template preferred]\n",
                inferIndent(), funcName, argTypes.size(),
                it->second->returnType ? it->second->returnType->toString() : "?");
            return it->second->returnType;
        }
        std::cout << std::format(
            "{}[call] non-template '{}' arg count mismatch ({} vs {}), keep searching\n",
            inferIndent(), funcName, it->second->parameters.size(), argTypes.size());
    }

    // ── ⑤ ADL 单独命中（普通查找完全没有这个裸名）──
    // 这是最常见的情形：`get(s)` 里 get 在全局根本不存在，只在 N 里。
    if (it == m_functionMap.end()) {
        for (const auto& ns : assocNs) {
            std::string qualified = ns + "::" + funcName;
            for (auto& f : m_functions) {
                if (!isPlainFreeFunction(f)) continue;
                if (f->name != qualified || !arityOk(f)) continue;
                std::cout << std::format(
                    "{}[call] {}({} args) → {}    [ADL: 实参关联命名空间 '{}' 命中]\n",
                    inferIndent(), funcName, argTypes.size(),
                    f->returnType ? f->returnType->toString() : "?", ns);
                checkArgBinding(f);   // 引用绑定（[dcl.init.ref]）
                if (calleeVar) calleeVar->name = qualified;
                return f->returnType;
            }
        }
    }

    // ── 函数模板路径（S2~S6）：仅对非成员调用 ──
    if (calleeVar) {
        // 显式模板实参的类型/值分流：推导引擎（S3）目前只吃类型实参表，故此处把
        // TemplateArg 拆回 TypePtr 列表；值为空的 Integral 实参说明用户写了 foo<4>(x)
        // —— 函数模板的 NTTP 尚未实现，明确报错而不是把空 TypePtr 塞进推导引擎引发崩溃。
        // （类模板的 NTTP 已实现，见 checkTemplateArguments。）
        std::vector<TypePtr> explicitTypeArgs;
        explicitTypeArgs.reserve(calleeVar->explicitTemplateArgs.size());
        for (const auto& a : calleeVar->explicitTemplateArgs) {
            if (a.isValue()) {
                error(std::format(
                    "explicit non-type template argument '{}' for function "
                    "template '{}' is not supported yet (only class templates "
                    "support non-type parameters)", a.toString(), funcName),
                    expr.location);
            }
            explicitTypeArgs.push_back(a.type);
        }

        TypePtr resolved = resolveTemplateCall(
            funcName, calleeVar, argTypes, argIsLValue,
            explicitTypeArgs, expr.location);
        if (resolved) return resolved;
    }

    // ── 兜底：原有 callee 推导路径（可能报 Undefined variable）──
    m_inferDepth++;
    TypePtr calleeType = inferType(expr.callee);
    m_inferDepth--;
    std::cout << std::format("{}[call] {}() → {} (callee type)\n",
        inferIndent(), funcName,
        calleeType ? calleeType->toString() : "?");
    return calleeType;
}

// 函数模板调用进行实参推导、约束/替换检查与候选择优，再实例化选中的函数。
// 缓存实例并回填调用符号与返回类型，避免重复实例化或重复定名。
// 参考：clang/lib/Sema/SemaTemplateDeduction.cpp、SemaOverload.cpp。
TypePtr SemanticAnalyzer::resolveTemplateCall(
    const std::string& funcName,
    std::shared_ptr<VarExpr> calleeVar,
    const std::vector<TypePtr>& argTypes,
    const std::vector<bool>& argIsLValue,
    const std::vector<TypePtr>& explicitArgs,
    SourceLocation loc) {

    auto candIt = m_functionTemplateCandidates.find(funcName);
    if (candIt == m_functionTemplateCandidates.end() || candIt->second.empty())
        return nullptr;

    auto& candidates = candIt->second;
    std::cout << std::format(
        "{}[call] {} — {} function template candidate(s), overload resolution begins\n",
        inferIndent(), funcName, candidates.size());

    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);
    struct Viable { TemplateDeclPtr decl; DeductionResult result; };
    std::vector<Viable> viables;

    for (auto& cand : candidates) {
        std::string plist;
        for (size_t i = 0; i < cand->typeParams.size(); i++) {
            if (i > 0) plist += ", ";
            plist += cand->typeParams[i];
        }
        std::cout << std::format("{}  candidate: template <{}> {}\n",
            inferIndent(), plist, funcName);
        auto r = deducer.deduce(cand, argTypes, argIsLValue, explicitArgs);
        if (r.success) {
            viables.push_back({cand, r});
        } else {
            //  SFINAE 吸收点 ②（全项目三处之一，见 include/sfinae.h 的收口点一览）
            // 本处失败以【推导返回值】表达（deduce 返回 success=false），不像 ① 那样
            // 靠异常 —— 两种写法并存，但"候选被移除"的语义与日志出口已统一到
            // Sfinae::rejected。
            Sfinae::rejected(std::format("函数模板重载 '{}'", funcName),
                             r.failureReason, inferIndent());
        }
    }

    if (viables.empty()) {
        error(std::format(
            "no matching function for call to '{}' (no template candidate deduces successfully)",
            funcName), loc);
    }

    // ── S6：偏序 —— 选最特化的可行候选 ──
    size_t winnerIdx = 0;
    for (size_t i = 1; i < viables.size(); i++) {
        if (isAtLeastAsSpecialized(viables[i].decl, viables[winnerIdx].decl)) {
            winnerIdx = i;
        }
    }
    if (viables.size() > 1) {
        std::cout << std::format(
            "{}  [overload] {} viable candidate(s), partial ordering picks #{}\n",
            inferIndent(), viables.size(), winnerIdx);
    }

    auto& winner = viables[winnerIdx];

    // ── S5：实例化（带缓存）──
    FuncDeclPtr instance =
        getOrInstantiateFunction(winner.decl, winner.result.deducedArgs);

    // 把 callee 重写为 mangled 符号，codegen 直接 callq
    calleeVar->name = instance->mangledName;
    std::cout << std::format("{}[call] {} → {} (template resolved) → {}\n",
        inferIndent(), funcName, instance->mangledName,
        instance->returnType ? instance->returnType->toString() : "void");

    return instance->returnType;
}

// 模板实例化先校验实参，替换蓝图后解析残留依赖类型，再注册可供 CodeGen 使用的声明。
// 参考：clang/lib/Sema/SemaTemplateInstantiate.cpp:Sema::InstantiateClass。
namespace {

/// 模板形参 kind 的可读名（诊断文案用）
std::string ttpKindName(TemplateParamKind k) {
    switch (k) {
        case TemplateParamKind::Type:     return "a type parameter";
        case TemplateParamKind::NonType:  return "a non-type parameter";
        case TemplateParamKind::Template: return "a template template parameter";
    }
    return "a parameter";
}

// [temp.arg.template]：模板模板实参逐位匹配内层签名，比较形参种类及值形参类型。
// 不能只比较参数个数：`template<class>` 与 `template<int>` 不可互换。
std::string ttpSignatureMismatch(const TemplateParam& p,
                                 const std::vector<TemplateParamPtr>& a) {
    // ① 位数（对照 clang 的两条 note：too few / too many template parameters
    //    in template template argument）
    if (a.size() != p.innerParams.size()) {
        return std::format("{} template parameters: '{}' declares {} parameter(s), "
                           "but the template argument provides {}",
                           a.size() < p.innerParams.size() ? "too few" : "too many",
                           p.name, p.innerParams.size(), a.size());
    }
    // ②③ 逐位形态（对照 clang 的 note：template parameter has a different kind /
    //    template non-type parameter has a different type）
    for (size_t i = 0; i < p.innerParams.size(); i++) {
        const TemplateParam& wp = *p.innerParams[i];   // want：形参位声明的
        const TemplateParam& ap = *a[i];               // given：实参模板声明的
        if (wp.kind != ap.kind) {
            return std::format("parameter {} has a different kind: '{}' declares {}, "
                               "but the template argument declares {}",
                               i + 1, p.name, ttpKindName(wp.kind), ttpKindName(ap.kind));
        }
        if (wp.kind == TemplateParamKind::NonType && wp.nonType && ap.nonType
            && !wp.nonType->equals(ap.nonType)) {
            return std::format(
                "non-type parameter {} has a different type: '{}' declares '{}', "
                "but the template argument declares '{}'",
                i + 1, p.name, wp.nonType->toString(), ap.nonType->toString());
        }
    }
    return {};
}

}  // namespace

void SemanticAnalyzer::checkTemplateArguments(
    const TemplateDeclPtr& blueprint, const TypePtr& templateIdType,
    SourceLocation loc) {

    const auto& params = blueprint->templateParams;
    const std::string& tname = templateIdType->name;
    //  取【非 const】引用：下面的 ②③ 循环除校验外还做形态归一（③-c），
    // 归一结果要写回 templateIdType —— 调用方随后按它建缓存键与生成符号。
    auto& args = templateIdType->templateArgs;

    // ── ① 个数（[temp.arg.explicit]/1 + [temp.param]/12）──
    // 实参可以【少于】形参，差额由默认实参补齐；但不能多于形参，且每位缺失的形参都必须
    // 带默认值，否则无法补全。
    // demo：template<class T, class U = void> + Box<int> → 少 1 位，U 有默认 ✓
    //       template<class T, class U = void> + Box<int,double,char> → 多了 ✗
    if (args.size() > params.size()) {
        error(std::format(
            "class template '{}' expects at most {} argument(s), but {} given",
            tname, params.size(), args.size()), loc);
    }
    if (args.size() < params.size()) {
        for (size_t i = args.size(); i < params.size(); i++) {
            if (!params[i]->hasDefault) {
                error(std::format(
                    "class template '{}' expects at least {} argument(s) "
                    "(parameter '{}' has no default), but {} given",
                    tname, i + 1, params[i]->name, args.size()), loc);
            }
        }
    }

    // 仅校验实际给出的实参位置；省略位置由默认实参和前面的个数检查约束。
    for (size_t i = 0; i < std::min(params.size(), args.size()); i++) {
        const TemplateParam& p = *params[i];
        TemplateArg&         a = args[i];

        // `template<auto V>` 的形参：类型由【实参】反推（[temp.param]/6 的 deduced
        //   non-type parameter）。形态自检、值域检查都不适用 —— 实参是什么形态，
        //   V 就是什么类型。
        const bool isAutoParam = p.kind == TemplateParamKind::NonType
                              && p.nonType && p.nonType->isAuto();

        // 该位"期望什么"的可读描述（供两向报错复用）
        const std::string want =
            (p.kind == TemplateParamKind::Type)
                ? "a type argument"
                : (p.kind == TemplateParamKind::Template
                       ? "a template argument"
                       : (isAutoParam
                              ? std::string("a non-type argument (parameter is 'auto')")
                              : std::format("a non-type argument of type '{}'",
                                            p.nonType ? p.nonType->toString() : "?")));

        // ②-a 类型形参 收到 值实参：Box<4> 而 T 是类型形参
        if (p.kind == TemplateParamKind::Type && a.isValue()) {
            error(std::format(
                "template argument {} for '{}' ('{}') must be {}, but '{}' is a value",
                i + 1, tname, p.name, want, a.toString()), loc);
        }
        // ②-b 非类型形参 收到 类型实参：Buf<int> 而 N 是 NTTP
        if (p.kind == TemplateParamKind::NonType && a.isType()) {
            error(std::format(
                "template argument {} for '{}' ('{}') must be {}, but '{}' is a type",
                i + 1, tname, p.name, want, a.toString()), loc);
        }
        // ③ NTTP 的形参类型必须受支持。对照 clang：[temp.param]/6 允许整型（含 bool）/
        //    枚举/指针/左值引用/字面量类类型等 —— 本项目只取整型家族 + auto。
        if (p.kind == TemplateParamKind::NonType && p.nonType
            && !p.nonType->isInteger() && !p.nonType->isAuto()) {
            error(std::format(
                "non-type template parameter '{}' of '{}' has unsupported type '{}' "
                "(only integral types and 'auto' are supported)",
                p.name, tname, p.nonType->toString()), loc);
        }
        // [temp.arg.nontype]：值实参须能转换成形参类型的常量表达式。
        // 窄化仅在值可表示时允许，例如目标 bool 不能无条件接受任意整数。
        if (p.kind == TemplateParamKind::NonType && a.isValue()) {
            if (isAutoParam) {
                // auto 形参：形态就是实参自己的形态（推导结果），无相容性可言。
                // 形如 int64_t 的值一律只留在这一个槽位 —— 值本身已在 a.value 里。
                if (!a.valueType) {
                    error(std::format(
                        "cannot deduce the type of non-type template parameter '{}' "
                        "of '{}' from an untyped value argument", p.name, tname), loc);
                }
                std::cout << std::format(
                    "  [sema:targ]   ⤷ auto 形参推导（[temp.param]/6）：'{}' := {} "
                    "（类型由实参反推）\n",
                    p.name, a.valueType->toString());
            }
            else if (!a.valueType) {
                error(std::format(
                    "non-type template argument for '{}' ('{}') has no type information",
                    tname, p.name), loc);
            }
            else if (!a.valueType->isInteger()) {
                error(std::format(
                    "non-type template argument for '{}' ('{}') has type '{}', "
                    "which is not an integral type (the parameter type is '{}')",
                    tname, p.name, a.valueType->toString(), p.nonType->toString()), loc);
            }
            else if (p.nonType && !p.nonType->canRepresentValue(a.value)) {
                // clang 原文：non-type template argument evaluates to N, which cannot be
                // narrowed to type 'T' [-Wc++11-narrowing]（本项目按错误处理，不降级为警告）
                error(std::format(
                    "non-type template argument evaluates to {}, which cannot be "
                    "narrowed to type '{}'",
                    a.value, p.nonType->toString()), loc);
            }
            else if (!a.valueType->equals(p.nonType)) {
                // ③-c 形态【归一】：把实参形态改写成形参类型。
                //  这一步不是"修饰"而是语义必需 —— `Buf<4>` 与 `Buf<4L>`
                // 在 `template<int N>` 下是【同一个】实例，归一后二者
                // 的可读串/缓存键/mangling 全都一致，不会各建一份。
                std::cout << std::format(
                    "  [sema:targ]   ⤷ 值位整型转换（[temp.arg.nontype]/1）："
                    "'{}'({}) ⇒ '{}'（转换后常量表达式）\n",
                    a.value, a.valueType->toString(), p.nonType->toString());
                a.valueType = p.nonType;
            }
        }

        // ②-c 模板模板形参 [temp.arg.template]/2：实参模板的形参表要逐位对上内层表。
        //     走到这儿时实参已被 resolveType 改写成【模板名】形态（sema 的
        //     `⤷ 第 N 位期望模板` 那一步）；若还没改写（别处直接造 id 的路径），
        //     这里兜底报"这一位要的是模板"。
        if (p.kind == TemplateParamKind::Template && a.isTemplate()) {
            // 实参模板自己的形参表：类模板 / 别名模板都从注册表取
            const std::vector<TemplateParamPtr>* argParams = nullptr;
            if (auto it = m_classTemplates.find(a.templateName);
                it != m_classTemplates.end()) {
                argParams = &it->second->templateParams;
            } else if (auto it = m_aliasTemplates.find(a.templateName);
                       it != m_aliasTemplates.end()) {
                argParams = &it->second->templateParams;
            }
            if (argParams == nullptr) {
                error(std::format(
                    "template argument {} for '{}' ('{}') must be {}, but '{}' is not "
                    "a template", i + 1, tname, p.name, want, a.templateName), loc);
            }
            if (const std::string why = ttpSignatureMismatch(p, *argParams); !why.empty()) {
                error(std::format(
                    "template template argument has different template parameters than "
                    "its corresponding template template parameter ('{}'): {}",
                    p.name, why), loc);
            }
            std::cout << std::format(
                "  [sema:targ]   ✓ 签名匹配: '{}' ← '{}'（{} 位逐位一致，"
                "[temp.arg.template]/2）\n",
                p.name, a.templateName, p.innerParams.size());
        }
        else if (p.kind == TemplateParamKind::Template) {
            error(std::format(
                "template argument {} for '{}' ('{}') must be {}, but '{}' is {}",
                i + 1, tname, p.name, want, a.toString(),
                a.isType() ? "a type" : "a value"), loc);
        }

        std::cout << std::format(
            "  [sema:targ]   ✓ param {}: '{}' ({}) ← {}\n",
            i + 1, p.name,
            p.kind == TemplateParamKind::Type
                ? "type"
                : (p.kind == TemplateParamKind::Template ? "template" : "non-type"),
            a.toString());
    }

    std::cout << std::format(
        "  [sema:targ] ✓ template arguments OK: {}<{}>\n", tname, [&] {
            std::string s;
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) s += ", ";
                s += args[i].toString();
            }
            return s;
        }());
}

// [temp.class.spec.match]/[temp.expl.spec]：全特化精确命中优先，其次匹配偏特化，否则使用主模板。
// 偏特化通过模式推导决定是否匹配，不能只按名字选择。
std::tuple<TemplateDeclPtr, TemplateInstantiator::TypeSubstitution, bool>
// 特化示例：`Box<int*>` 可命中指针偏特化，`Box<int>` 在无匹配特化时使用主模板。
SemanticAnalyzer::selectClassTemplate(
    const TemplateDeclPtr& primary,
    const std::vector<TemplateArg>& args,
    SourceLocation loc) {

    const std::string& name = primary->templateName();

    // 特化匹配保留类型与 NTTP 值实参，例如 `enable_if<true, T>` 由 matchPattern 逐位匹配。

    std::cout << std::format("  [spec:select] ★ selecting class template '{}' for <{}>\n",
        name, [&] { std::string s;
                   for (size_t i = 0; i < args.size(); i++) {
                       if (i > 0) s += ", "; s += args[i].toString();
                   } return s; }());

    {
        // ── ① 全特化：逐位实参相等 ──
        // 对照 clang：clang/lib/Sema/SemaTemplate.cpp:Sema::CheckTemplateArgumentList 对已知特化的精确匹配路径
        if (auto it = m_explicitSpecs.find(name); it != m_explicitSpecs.end()) {
            for (const auto& spec : it->second) {
                if (spec->specPattern.size() != args.size()) continue;
                bool same = true;
                for (size_t i = 0; i < args.size(); i++) {
                    if (!spec->specPattern[i].equals(args[i])) { same = false; break; }
                }
                if (same) {
                    std::cout << std::format(
                        "  [spec:select]   ├─ ① explicit specialization matched "
                        "(exact argument equality) → USING IT\n");
                    return {spec, {}, true};
                }
            }
            std::cout << "  [spec:select]   ├─ ① no explicit specialization matched\n";
        }

        // ── ② 偏特化：用实参推导模式 ──
        // 对照 clang：clang/include/clang/Sema/Sema.h:Sema::CheckTemplatePartialSpecializationArgs
        //              + clang/lib/Sema/SemaTemplateDeduction.cpp:DeduceTemplateArguments（与函数模板同一套合一算法）
        if (auto it = m_partialSpecs.find(name); it != m_partialSpecs.end()) {
            // ── 第一轮：收集【全部】匹配的候选（不再一匹配就返回）——
            // 收集是偏序裁决的前提：只有一个候选时无从比较，两个以上才谈得上"谁更特化"。
            struct Candidate {
                TemplateDeclPtr spec;
                TemplateInstantiator::TypeSubstitution taggedSubst;
            };
            std::vector<Candidate> matched;

            for (const auto& spec : it->second) {
                // 偏特化的形参名列表（用于识别模式里的可绑定未知量）
                std::vector<std::string> paramNames;
                for (const auto& p : spec->templateParams) paramNames.push_back(p->name);

                std::unordered_map<std::string, TypePtr> subst;
                std::string reason;
                //  必须把求值器挂上：偏特化模式里的 void_t<decltype(...)>
                // 要靠它做"替换 + 求值"探测。缺了这步，void_t 不会被归约成
                // void，模式第 2 位就永远匹配不上 —— SFINAE 探测整条链断掉。
                TemplateDeducer deducer; // 模板推导
                deducer.setDecltypeEvaluator(this);
                deducer.setMemberTypeResolver(this);   // void_t<typename T::type> 探测要用
                deducer.setAliasTemplateResolver(this);
                if (deducer.matchPattern(spec->specPattern, args,
                                         paramNames, subst, reason)) {
                    std::cout << std::format(
                        "  [spec:select]   ├─ ② 候选：'{}<{}>' 匹配成功\n",
                        name, patternToString(spec->specPattern));

                    TemplateInstantiator::TypeSubstitution tagged;
                    for (auto& [k, v] : subst) tagged[k] = TemplateArg::ofType(v);
                    matched.push_back({spec, tagged});
                }
                else {
                    std::cout << std::format(
                        "  [spec:select]   ├─ ② 候选不匹配：{}\n", reason);
                }
            }

            // ── 第二轮：偏序裁决（[temp.class.order]）──
            // 只有一个候选 → 直接胜出，无需比较。
            if (matched.size() == 1) {
                std::cout << std::format(
                    "  [spec:select]   │  唯一候选 → 直接选中 '{}<{}>'\n",
                    name, patternToString(matched[0].spec->specPattern));
                return {matched[0].spec, matched[0].taggedSubst, true};
            }

            if (matched.size() > 1) {
                std::cout << std::format(
                    "  [spec:select]   │  ★ {} 个偏特化同时匹配 → 进入 [temp.class.order] 偏序裁决\n",
                    matched.size());
                int winner = -1;
                {
                    std::vector<int> best;
                    for (size_t i = 0; i < matched.size(); i++) {
                        bool dominated = false;
                        for (size_t j = 0; j < matched.size() && !dominated; j++) {
                            if (i == j) continue;
                            // j 严格比 i 更特化（j 至少和 i 一样特化，且 i 不比 j 更特化）
                            if (classSpecAtLeastAsSpecialized(matched[j].spec, matched[i].spec) &&
                                !classSpecAtLeastAsSpecialized(matched[i].spec, matched[j].spec)) {
                                dominated = true;
                            }
                        }
                        if (!dominated) best.push_back(static_cast<int>(i));
                    }
                    if (best.size() == 1) {
                        winner = best[0];
                    } else {
                        // 多个候选互不支配 → 歧义，标准要求报错
                        // 对照 clang：clang/include/clang/Basic/DiagnosticSemaKinds.td:err_partial_spec_ordering_ambiguous
                        std::string cands;
                        for (int b : best) {
                            if (!cands.empty()) cands += " / ";
                            cands += patternToString(matched[b].spec->specPattern);
                        }
                        error(std::format(
                            "ambiguous partial specializations of '{}' for <{}>: "
                            "{} are equally specialized, none is more specialized "
                            "than the others",
                            name, typeListToString(args), cands), loc);
                    }
                }
                std::cout << std::format(
                    "  [spec:select]   │  ⇒ 最特化者：'{}<{}>' → USING IT\n",
                    name, patternToString(matched[winner].spec->specPattern));
                return {matched[winner].spec, matched[winner].taggedSubst, true};
            }
        }
    }

    // ── ③ 主模板 ──
    std::cout << "  [spec:select]   └─ ③ falling back to PRIMARY template\n";
    (void)loc;
    return {primary, {}, false};
}

// `Box<int>` 与 `Box<double>` 各自缓存实例；同一实参列表重复使用已有实例。
TypePtr SemanticAnalyzer::getOrInstantiateClass(
    TypePtr templateIdType, SourceLocation loc) {

    // ── 查主模板（类模板注册表 O(1)；重名取先注册者，emplace 不覆盖）──
    //  必须在算缓存键【之前】：键的构成要看形参表（auto 位的形态要进键），
    // 理由见 NameMangler::losslessArgumentsKey。注册表查询是 O(1)，提前无代价。
    TemplateDeclPtr primary;
    if (auto it = m_classTemplates.find(templateIdType->name);
        it != m_classTemplates.end()) {
        primary = it->second;
    }
    if (!primary) {
        error(std::format("'{}' is not a class template", templateIdType->name), loc);
    }

    // 常量表达式 NTTP 在计算缓存键前归一化，使 `Buf<2+2>` 与 `Buf<4>` 复用同一实例。
    for (auto& a : templateIdType->templateArgs) {
        if (a.kind != TemplateArgKind::Integral || !a.valueExpr) continue;
        auto folded = m_constantFolder.evaluateIntegral(a.valueExpr);
        if (!folded.has_value()) {
            error(std::format(
                "non-type template argument is not a constant "
                "expression"), loc);
        }
        std::cout << std::format(
            "  [sema:targ] ⤷ 常量表达式实参求值（[temp.arg.nontype]/1）：{} ⇒ {}（{}）\n",
            a.valueExpr ? "<expr>" : "?", folded->value, folded->type->toString());
        a.value     = folded->value;
        a.valueType = folded->type;
        a.valueExpr = nullptr;
    }

    // 缓存键共用 NameMangler::losslessArgumentsKey，包含实参值及必要的类型形态。
    // `template<auto V>` 的 `K<4>` 与 `K<4L>` 不得合并为同一实例。
    const std::string key = NameMangler::losslessArgumentsKey(
        templateIdType->name, templateIdType->templateArgs, primary->templateParams);

    auto cached = m_classInstanceCache.find(key);
    if (cached != m_classInstanceCache.end()) {
        std::cout << std::format(
            "  [instantiate:class] cache hit: {} (skip re-instantiation)\n", key);
        return cached->second;
    }

    // ── 实参校验（[temp.arg]）：个数 + 每位形态（类型 vs 值）都要对上 ──
    //  这里是 NTTP 的关键校验点：template<class T> 收到值实参、template<int N> 收到类型
    // 实参，都必须在此拦下。注意用 templateParams（带 kind 的结构化形参表）而不是
    // typeParams（退化的名字列表）—— 后者根本不知道哪位是 NTTP。
    checkTemplateArguments(primary, templateIdType, loc);

    // ── 用默认实参把实参表补全（[temp.param]/12）──
    // demo：template<class T, class U = void> + 使用点 Box<int>
    // ⇒ 补成 [Type:int, Type:void]，此后一切按"实参已完整"处理。
    //  补全必须在【选择特化之前】做：偏特化/全特化的匹配都是对完整实参表做的
    // （Box<int*, int> 的全特化要有 2 位才能匹配上）。
    std::vector<TemplateArg> fullArgs = templateIdType->templateArgs;
    if (fullArgs.size() < primary->templateParams.size()) {
        for (size_t i = fullArgs.size(); i < primary->templateParams.size(); i++) {
            const TemplateParam& p = *primary->templateParams[i];

            // 默认实参可依赖前面的形参，填充前先应用已绑定实参。
            // `template<class T, class U = T>` 配 T=int 时，默认 U 同为 int。
            TemplateArg filled = p.defaultArg;
            if (i > 0 && filled.isType() && filled.type) {
                TemplateInstantiator::TypeSubstitution prior;
                for (size_t j = 0; j < i; j++) {
                    prior[primary->templateParams[j]->name] = fullArgs[j];
                }
                TypePtr resolved = m_instantiator.substituteType(filled.type, prior);
                if (resolved != filled.type) {
                    std::cout << std::format(
                        "  [sema:targ] ⤷ default argument '{}' is dependent: {} → {}\n",
                        p.name, filled.type->toString(), resolved->toString());
                    filled = TemplateArg::ofType(resolved);
                }
            }

            fullArgs.push_back(filled);
            std::cout << std::format(
                "  [sema:targ] ⤷ default argument filled: '{}' := {}\n",
                p.name, filled.toString());
        }
    }

    // ── 择优：全特化 → 偏特化 → 主模板（[temp.class.spec.match]）──
    TemplateDeclPtr blueprint = primary;
    TemplateInstantiator::TypeSubstitution specSubst; // 仅特化路径非空
    bool pickedSpec = false;
    std::tie(blueprint, specSubst, pickedSpec) =
        // 三路择优（全特化 / 偏特化 + 偏序裁决 / 主模板兜底），偏特化路径内部用 SFINAE 剔除候选
        selectClassTemplate(primary, fullArgs, loc);

    std::cout << std::format(
        "\n  [instantiate:class] ★ on-demand instantiation: {}\n", key);

    // ── ① 深拷贝蓝图 + 结构化替换 ──
    // 特化路径传 &specSubst（由 selectClassTemplate 的模式匹配推导）；主模板路径传
    // nullptr ⇒ instantiate 内部按 templateParams 逐位分派。两条路径的 args 都只用于
    // 实例名与 mangling。
    ClassDeclPtr instance = m_instantiator.instantiate(
        blueprint, fullArgs, pickedSpec ? &specSubst : nullptr);

    // ── ② 实例类完整注册（与源码中手写的类一视同仁）──
    processClassDecl(instance);

    TypePtr instanceType = m_classTypes[instance->name];

    // ── 记下实例"出身"（哪个模板 + 哪些实参）──
    // 实例类型名叫 MyPtr_int，改名的瞬间模板 id 的信息就没了；
    // 而函数模板实参推导要拿 `MyPtr_int` 去匹配形参模式 `MyPtr<T>`，
    // 必须能回答"你由哪个模板、用哪些实参实例化而来"。
    // 见 include/type.h 的 templateOriginName 注释。
    if (instanceType) {
        instanceType->templateOriginName = templateIdType->name;
        instanceType->templateOriginArgs = fullArgs;
    }

    // 实例类符号同时登记进全局作用域：实例化可能在某函数体分析中途触发，
    // processClassDecl 会把符号 define 进该函数作用域 —— 兄弟函数不可见。
    // 全局作用域冗余登记一份，保证任何位置 lookup("Box_int") 都命中。
    Symbol globalSym;
    globalSym.name = instance->name;
    globalSym.type = instanceType;
    globalSym.kind = SymbolKind::Type;
    globalSym.isLocal = false;
    globalSym.definedAt = instance->location;
    m_symbolTable.globalScope()->define(instance->name, globalSym);

    // 缓存先行写入（方法体分析可能再次引用本实例——递归/互用场景）
    m_classInstanceCache[key] = instanceType;

    // ── ③ 实例方法：注册（等价 Pass 2）+ 函数体分析（等价 Pass 3）──
    //  走 forEachClassMethod（递归进嵌套类）：嵌套类的方法不在外层类的 methods 表里，
    // 只遍历 instance->methods ⇒ Pass 2 不注册、Pass 3 不分析 ⇒ CodeGen 从没收到
    // 那些方法 ⇒ 链接期 undefined reference（与命名空间递归同款：DeclContext 树
    // 本身就是递归的 —— 见 forEachClassMethod 的注释）。
    forEachClassMethod(instance, [&](FuncDeclPtr m) { registerFunction(m); });
    forEachClassMethod(instance, [&](FuncDeclPtr m) { analyzeFunctionBody(m); });

    std::cout << std::format(
        "  [instantiate:class] ✔ {} ready ({} bytes, {} method(s))\n",
        instance->name, instanceType->classLayout.totalSize,
        instance->methods.size());
    return instanceType;
}

FuncDeclPtr SemanticAnalyzer::getOrInstantiateFunction(
    TemplateDeclPtr tmpl, const std::vector<TypePtr>& args) {
    // 函数模板的实参由推导引擎产出，全是类型（函数模板 NTTP 未实现），
    // 故包成 TemplateArg::ofType 送进通用的 mangler。
    std::vector<TemplateArg> targs;
    targs.reserve(args.size());
    for (auto& a : args) targs.push_back(TemplateArg::ofType(a));

    // 缓存键与 TemplateInstantiator 共用完整签名，包含返回类型和参数表。
    // 同为 T=int 的 `f(1)` 与 `f(1,2)` 仍是不同重载，不能仅按模板实参合并。
    std::string mangled = NameMangler::mangleFunctionTemplateInstance(
        tmpl->funcTemplate->name, targs,
        tmpl->funcTemplate->returnType, tmpl->funcTemplate->parameters,
        tmpl->typeParams);

    auto it = m_templateInstanceCache.find(mangled);
    if (it != m_templateInstanceCache.end()) {
        std::cout << std::format("  [instantiate] cache hit: {} (skip re-instantiation)\n",
            mangled);
        return it->second;
    }

    FuncDeclPtr instance = m_instantiator.instantiateFunction(tmpl, args);
    m_templateInstanceCache[mangled] = instance;

    // 结构化替换不等于实例化；残留模板 id 需继续 resolveType 才能取得真实布局与方法。
    if (instance->returnType) instance->returnType = resolveType(instance->returnType);
    for (auto& param : instance->parameters) {
        param.type = resolveType(param.type);
    }

    // 注册进 codegen 函数列表；函数体在具体类型下做两阶段查找的第二阶段
    m_functionMap[mangled] = instance;
    m_functions.push_back(instance);
    analyzeFunctionBody(instance);
    return instance;
}

// [temp.mem]：成员模板复用自由模板的推导、缓存与实例注册，另保留所属类身份。
FuncDeclPtr SemanticAnalyzer::getOrInstantiateMemberFunction(
    const TemplateDeclPtr& tmpl,
    const std::vector<TypePtr>& argTypes,
    const std::vector<bool>& argIsLValue,
    const std::string& ownerClassName,
    SourceLocation loc) {

    std::cout << std::format(
        "{}[call] {}::{} — 成员模板候选，开始推导（[temp.mem]）\n",
        inferIndent(), ownerClassName, tmpl->templateName());

    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);
    auto r = deducer.deduce(tmpl, argTypes, argIsLValue);
    if (!r.success) {
        // SFINAE 性质：这位成员模板推不出来【不一定是错误】—— 调用方可能还有别的
        // 重载可选。故只报推导失败原因，由调用方决定是继续找还是报 no matching。
        std::cout << std::format("{}[call] {}::{} 推导失败：{}\n",
            inferIndent(), ownerClassName, tmpl->templateName(), r.failureReason);
        return nullptr;
    }

    // 缓存键 = 实例的符号名。与 Instantiator 用同一个 sanitizeSymbolChars ——
    // 两处各写一份的话，键与符号名会悄悄对不上（命中失败或误命中，都不报错）。
    std::string mangled = sanitizeSymbolChars(
        ownerClassName + "_" + tmpl->funcTemplate->name);
    for (auto& a : r.deducedArgs) {
        mangled += "_" + sanitizeSymbolChars(a ? a->toString() : "?");
    }

    auto it = m_templateInstanceCache.find(mangled);
    if (it != m_templateInstanceCache.end()) {
        std::cout << std::format("  [instantiate] cache hit: {} (skip re-instantiation)\n",
            mangled);
        return it->second;
    }

    FuncDeclPtr instance =
        m_instantiator.instantiateFunction(tmpl, r.deducedArgs, ownerClassName);
    m_templateInstanceCache[mangled] = instance;

    // 落地解析（同自由函数模板）：替换只做【替换】，残留的半成品模板 id 要靠
    // resolveType 兑现成具体实例类型，否则函数体里 p.value 查不到类。
    if (instance->returnType) instance->returnType = resolveType(instance->returnType);
    for (auto& param : instance->parameters) {
        param.type = resolveType(param.type);
    }

    std::cout << std::format("{}[call] {}::{} → {} (member template instantiated) → {}\n",
        inferIndent(), ownerClassName, tmpl->templateName(), mangled,
        instance->returnType ? instance->returnType->toString() : "void");

    m_functionMap[mangled] = instance;
    m_functions.push_back(instance);
    analyzeFunctionBody(instance);
    (void)loc;
    return instance;
}

// ─── S6 偏序：a 是否"至少与 b 同样特化"（[temp.func.order] 简化，docs/learn/21）──
//  方向易错：用 a 自己的参数类型当合成实参，去推导 b。b 能推导成功 ⇒ b 覆盖 a 的全部
// 输入域 ⇒ a 接受的类型更少 ⇒ a 更特化。
// demo: po(T) vs po(T*)：用 T* 当实参推导 po(T) ⇒ T := T* 成功 ⇒ po(T*) 更特化 ✓
// 对照 clang：lib/Sema/SemaOverload.cpp → IsAtLeastAsSpecialized
bool SemanticAnalyzer::isAtLeastAsSpecialized(TemplateDeclPtr a, TemplateDeclPtr b) {
    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);
    std::vector<TypePtr> synthArgs;
    std::vector<bool>    synthLValue;
    for (auto& p : a->funcTemplate->parameters) {
        synthArgs.push_back(p.type);   // a 的模板参数名在此扮演"唯一合成类型"
        synthLValue.push_back(true);
    }

    std::cout << std::format("  [overload] partial ordering check (deduction-based)...\n");
    auto r = deducer.deduce(b, synthArgs, synthLValue);
    std::cout << std::format("  [overload]   ⇒ {}\n",
        r.success ? "前者至少与后者同样特化（前者胜）"
                  : "无特化关系（保留原候选）");
    return r.success;
}

// [expr.ref]：成员访问将名字解析为字段类型/偏移或方法声明。
// `.` 使用对象类型，`->` 先取指针所指类型；继承查找保留字段来源子对象。
FuncDeclPtr SemanticAnalyzer::findMethodInClass(const std::string& className,
                                                const std::string& methodName,
                                                int arity,
                                                const std::vector<TypePtr>* argTypes) const {
    auto it = m_classDecls.find(className);
    if (it == m_classDecls.end()) return nullptr;

    // [class.member.lookup] 产出完整同名候选集，再由 pickBestByArgs 按实参择优。
    // 查找阶段不能只返回首个同名同参数个数的方法。
    std::vector<FuncDeclPtr> candidates;
    for (auto& method : it->second->methods) {
        if (method->name != methodName) continue;
        // arity < 0 ⇒ 不看参数个数（`d.g` 这种没有实参可数，无法据此筛选）
        if (arity >= 0 && method->parameters.size() != static_cast<size_t>(arity)) continue;
        candidates.push_back(method);
    }
    if (!argTypes) return candidates.empty() ? nullptr : candidates.front();
    return pickBestByArgs(candidates, *argTypes,
                          std::format("member {}.{}", className, methodName));
}

// ── "是不是基类"的判据（[class.derived]）──────────────────────────────────────
// BFS 沿 baseClassNames 上溯。 只此一处：inferDynamicCast 里那个 hasCommonAncestor
// 是**另一个**关系（"存在公共祖先"，兄弟类互转也算），不要合并 —— 合并会把
// dynamic_cast 的可达性放宽成"同一条继承链"。
bool SemanticAnalyzer::isClassOrBaseOf(const std::string& derived,
                                       const std::string& base) const {
    std::vector<std::string> work = {derived};
    std::set<std::string> visited;
    while (!work.empty()) {
        std::string cur = work.back(); work.pop_back();
        if (!visited.insert(cur).second) continue;
        if (cur == base) return true;
        auto it = m_classDecls.find(cur);
        if (it == m_classDecls.end()) continue;
        for (auto& bn : it->second->baseClassNames) work.push_back(bn);
    }
    return false;
}

// 限定字段 `obj.Q::m` 与类内裸名共用定位规则，按声明来源确定归属基类子对象。
// 只按字段名匹配会混淆多继承中的同名成员。
SemanticAnalyzer::QualifiedLookupResult
SemanticAnalyzer::resolveQualifiedField(const std::string& ownerClass,
                                        const std::string& qualifier,
                                        const std::string& member) {
    QualifiedLookupResult result;
    auto classIt = m_classTypes.find(ownerClass);
    if (classIt != m_classTypes.end()) {
        for (auto& f : classIt->second->classLayout.fields) {
            if (f.bareName() != member) continue;          //  权威裸名，不是显示名
            bool belongs = f.sourceClass == qualifier
                || (f.sourceClass.empty() && qualifier == ownerClass)
                || (!f.sourceClass.empty() && isClassOrBaseOf(f.sourceClass, qualifier));
            if (belongs) { result.field = &f; return result; }
        }
    }
    // ② static 数据成员：符号表注册的就是 "类名::成员" 全名（processClassDecl）。
    //    沿 qualifier 及其基类链查（BFS + visited 防环，与 isClassOrBaseOf 同形状）：
    //    `d.Derived::limit`（limit 声明在 Base）由此命中 —— 实例字段的继承查找
    //    由扁平布局天然覆盖，static 成员没有布局，继承只能靠这条链。
    {
        std::vector<std::string> work = {qualifier};
        std::set<std::string> visited;
        while (!work.empty()) {
            std::string cur = work.back();
            work.pop_back();
            if (!visited.insert(cur).second) continue;
            if (Symbol* sym = m_symbolTable.lookup(cur + "::" + member)) {
                result.statik = sym;
                return result;
            }
            auto it = m_classDecls.find(cur);
            if (it == m_classDecls.end()) continue;
            for (auto& bn : it->second->baseClassNames) work.push_back(bn);
        }
    }
    return result;
}

FuncDeclPtr SemanticAnalyzer::findMethodInHierarchy(const std::string& className,
                                                    const std::string& methodName,
                                                    std::string* declaringClass) const {
    // 遍历顺序刻意与 inferCall 的成员方法搜索保持逐字一致（基类名 insert 到队首、
    // 从队尾取）—— 否则"两个基类都有同名方法"时两条通路会选中不同的那一个。
    std::vector<std::string> searchQueue = {className};
    std::set<std::string> searched;
    while (!searchQueue.empty()) {
        std::string clsName = searchQueue.back();
        searchQueue.pop_back();
        if (!searched.insert(clsName).second) continue;
        if (auto method = findMethodInClass(clsName, methodName, -1)) {
            if (declaringClass) *declaringClass = clsName;
            return method;
        }
        auto it = m_classDecls.find(clsName);
        if (it == m_classDecls.end()) continue;
        for (auto& bn : it->second->baseClassNames)
            searchQueue.insert(searchQueue.begin(), bn);
    }
    return nullptr;
}

// 成员字段访问回填类型和字节偏移；成员方法访问保留声明，供调用阶段继续择优。
TypePtr SemanticAnalyzer::inferMember(MemberExpr& expr) {
    if (expr.isTypeAccess) {
        TypePtr owner = resolveTypeQualifier(expr.object);
        int64_t value = 0;
        if (lookupStaticConst(owner->name, expr.memberName, value)) {
            expr.staticConstFolded = true;
            expr.staticConstValue = value;
            return m_lastStaticConstType ? m_lastStaticConstType : Type::makeInt();
        }
        if (Symbol* member = m_symbolTable.lookup(owner->name + "::" + expr.memberName)) {
            expr.resolvedStaticSymbol = member->name;
            return member->type;
        }
        error(std::format("no static member '{}' in class '{}'", expr.memberName,
                          owner->name), expr.location);
    }
    m_inferDepth++;
    TypePtr objType = inferType(expr.object);
    m_inferDepth--;

    if (!objType) {
        error("Cannot access member of null type", expr.location);
    }

    // 解引用指针
    TypePtr actualType = objType;
    if (expr.isArrow && objType->isPointer()) {
        actualType = objType->pointeeType;
    }

    // 成员访问前剥除引用包装，以被绑定对象的类类型进行字段与方法查找。
    if (actualType && actualType->isReference()) {
        actualType = actualType->referencedType;
    }

    // ── 剥顶层 const：成员【查找】不受对象 cv 限定影响（[class.mfct.non-static]
    //   里 const 只管写检查，写检查未做 —— ROADMAP 已列）；`const ClassA a; a.v`
    //   查的是 ClassA 的 v。不剥 ⇒ Const(Class) 的 isClass() 为假，误报
    //   "Cannot access member on non-class type 'const ClassA'"（拒收合法程序）。
    if (actualType && actualType->isConst()) {
        actualType = actualType->stripConst();
    }

    if (!actualType || !actualType->isClass()) {
        error(std::format("Cannot access member '{}' on non-class type '{}'",
            expr.memberName, actualType ? actualType->toString() : "?"),
            expr.location);
    }

    // ── 限定名成员 `obj.Q::m` / `this->Q::m`（[expr.ref] 的嵌套名限定形式）──
    // 由 Parser 把 `Q::m` 拼成一整个成员名（与 `A::B` 名字串同源）。与 inferVar 的
    // 类内裸名形式共用 resolveQualifiedField 一个判据；这里多一件事：把权威偏移
    // 回填到 MemberExpr 上 —— 按裸名查会命中派生类隐藏字段那条，偏移是错的。
    if (auto sep = expr.memberName.rfind("::"); sep != std::string::npos) {
        const std::string qualifier = expr.memberName.substr(0, sep);
        const std::string member    = expr.memberName.substr(sep + 2);
        if (isClassOrBaseOf(actualType->name, qualifier)) {
            QualifiedLookupResult hit =
                resolveQualifiedField(actualType->name, qualifier, member);
            if (!hit.valid()) {
                error(std::format("no member named '{}' in '{}'", member, qualifier),
                      expr.location);
            }
            if (hit.statik) {
                // 静态成员回填全局符号，CodeGen 走 RIP 寻址；读取时对象表达式仍求值并保留副作用。
                expr.resolvedStaticSymbol = hit.statik->name;
                expr.memberName = member;
                // 类内初始化的 static const（常量表命中）没有存储 —— 读取折叠成
                // 字面量；lookupStaticConst 沿基类链走，故从限定类起查。
                int64_t folded = 0;
                if (lookupStaticConst(qualifier, member, folded)) {
                    expr.staticConstFolded = true;
                    expr.staticConstValue  = folded;
                }
                std::cout << std::format(
                    "{}[member] {}.{}::{} → {}    (static member, 全局符号 {})\n",
                    inferIndent(), actualType->name, qualifier, member,
                    hit.statik->type ? hit.statik->type->toString() : "?",
                    hit.statik->name);
                return hit.statik->type;
            }
            const FieldInfo* f = hit.field;
            expr.resolvedFieldOffset = static_cast<int>(f->offset);
            expr.resolvedFieldSize   = f->size;
            expr.memberName = member;
            std::cout << std::format(
                "{}[member] {}.{}::{} → {}    (qualified member, offset={})\n",
                inferIndent(), actualType->name, qualifier, member,
                f->type ? f->type->toString() : "?", f->offset);
            return f->type;
        }
    }

    // [class.member.lookup]：先检查来自不同基类子对象的同名字段歧义，再选择字段。
    // 不能以遍历顺序任取第一条。
    auto fieldMatches = actualType->classLayout.findFields(expr.memberName);
    if (fieldMatches.size() > 1) {
        std::string where;   // 每条命中都点出"经哪个子对象、显示名是什么"
        for (auto* f : fieldMatches) {
            if (!where.empty()) where += "、";
            where += std::format("经 '{}' 的 '{}'",
                f->viaBase.empty() ? std::string("(本类)") : f->viaBase, f->name);
        }
        error(std::format(
            "Member '{}' is ambiguous in class '{}': found in {} base-class subobjects ({})",
            expr.memberName, actualType->name, fieldMatches.size(), where), expr.location);
    }
    auto fieldInfo = fieldMatches.empty() ? nullptr : fieldMatches.front();
    if (fieldInfo) {
        std::cout << std::format(
            "{}[member] {}.{} → {}    (offset={}, size={})\n",
            inferIndent(),
            actualType->name, expr.memberName,
            fieldInfo->type ? fieldInfo->type->toString() : "?",
            fieldInfo->offset, fieldInfo->size);
        return fieldInfo->type;
    }

    // `obj.count` 与 `obj.A::count` 可指向同一静态成员，均回填全局符号。
    // 成员来源沿基类查找，读取保留对象表达式副作用；当前模型先查数据再查方法。
    {
        std::vector<std::string> work = {actualType->name};
        std::set<std::string> visited;
        while (!work.empty()) {
            std::string cur = work.back();
            work.pop_back();
            if (!visited.insert(cur).second) continue;
            if (Symbol* sym = m_symbolTable.lookup(cur + "::" + expr.memberName)) {
                expr.resolvedStaticSymbol = sym->name;
                // 类内初始化的 static const（常量表命中）没有存储 —— 读取折叠成
                // 字面量；从对象实际类起查（lookupStaticConst 沿基类链走）。
                int64_t folded = 0;
                if (lookupStaticConst(actualType->name, expr.memberName, folded)) {
                    expr.staticConstFolded = true;
                    expr.staticConstValue  = folded;
                }
                std::cout << std::format(
                    "{}[member] {}.{} → {}    (static member, 全局符号 {})\n",
                    inferIndent(), actualType->name, expr.memberName,
                    sym->type ? sym->type->toString() : "?", sym->name);
                return sym->type;
            }
            auto it = m_classDecls.find(cur);
            if (it == m_classDecls.end()) continue;
            for (auto& bn : it->second->baseClassNames) work.push_back(bn);
        }
    }

    // [class.member.lookup]：方法查找沿当前类及基类链进行，与调用候选查找共用规则。
    std::string declCls;
    if (auto method = findMethodInHierarchy(actualType->name, expr.memberName, &declCls)) {
        std::cout << std::format(
            "{}[member] {}.{}() → {}    (method{}{})\n",
            inferIndent(),
            actualType->name, expr.memberName,
            method->returnType ? method->returnType->toString() : "?",
            method->isVirtual ? ", virtual" : "",
            declCls != actualType->name ? std::format(" via '{}'", declCls) : "");
        return method->returnType;
    }

    // 成员模板名存在时保留候选，具体函数由调用实参推导；此处不能提前报无成员。
    if (expr.isMethodCall) {
        if (auto mtIt = m_classMemberTemplates.find(actualType->name);
            mtIt != m_classMemberTemplates.end()) {
            for (auto& mt : mtIt->second) {
                if (mt->templateName() == expr.memberName) {
                    std::cout << std::format(
                        "{}[member] {}.{} —— 成员模板（待实参推导，[temp.mem]）\n",
                        inferIndent(), actualType->name, expr.memberName);
                    return Type::makeVoid();
                }
            }
        }
    }

    error(std::format("No member '{}' in class '{}'",
        expr.memberName, actualType->name), expr.location);
}

// [expr.sub]：原生数组按元素类型推导；容器下标读取按 at(i) 成员调用约定处理。
// 赋值下标另使用 set(i, value)，不等同于完整 operator[] 重载支持。
TypePtr SemanticAnalyzer::inferIndex(IndexExpr& expr) {
    m_inferDepth++;
    TypePtr objType = inferType(expr.object);
    m_inferDepth--;

    if (!objType) {
        error("Cannot subscript expression of null type", expr.location);
    }

    TypePtr actualType = objType;
    if (actualType && actualType->isReference()) {
        actualType = actualType->referencedType;
    }

    // ── 原生数组与指针下标运算（[expr.sub]）──
    // a[i] ≡ *(a + i) —— 如果目标是真数组（Array）或指针（Pointer），直接寻址！
    if (actualType && (actualType->isArray() || actualType->isPointer())) {
        m_inferDepth++;
        TypePtr idxType = inferType(expr.index);
        m_inferDepth--;
        if (!idxType || !idxType->isInteger()) {
            error(std::format("array subscript is not an integer (got '{}')",
                              idxType ? idxType->toString() : "?"), expr.location);
        }
        TypePtr elemType = actualType->isArray() ? actualType->elementType
                                                 : actualType->pointeeType;
        std::cout << std::format("  [index] raw array/pointer subscript: {}[{}] → {}\n",
            actualType->toString(), idxType ? idxType->toString() : "?",
            elemType ? elemType->toString() : "?");
        return elemType;
    }

    // 支持容器指针：p[i] ≡ (*p)[i]（与 MemberExpr 的 -> 解引用同一约定）
    if (objType->isPointer()) {
        actualType = objType->pointeeType;
    }

    if (!actualType || !actualType->isClass()) {
        error(std::format("Cannot apply subscript to non-class type '{}'",
            actualType ? actualType->toString() : "?"), expr.location);
    }

    // 查约定方法 at()：必须存在且恰好接收 1 个形参
    auto classIt = m_classDecls.find(actualType->name);
    if (classIt == m_classDecls.end()) {
        error(std::format("Class '{}' not declared", actualType->name),
              expr.location);
    }

    std::shared_ptr<FunctionDecl> atMethod;  // 形参校验与结果类型共用
    for (auto& method : classIt->second->methods) {
        if (method->name == "at" && method->parameters.size() == 1) {
            atMethod = method;
            break;
        }
    }
    if (!atMethod) {
        error(std::format(
            "Class '{}' has no at() method —— subscript requires the "
            "at()/set() convention (see docs/learn/12)", actualType->name),
            expr.location);
    }

    //  回填两个约定方法的【符号】：CodeGen 不能硬拼 `类名_at` / `类名_set`
    // —— Sema 给带参成员方法名加了"参数个数"后缀（IntVec_at_1 / IntVec_set_2），
    // 硬拼得到的是 IntVec_at，链接期 undefined reference。
    // set 只回填不强制：类里没有 set 时留空串，CodeGen 退回硬拼，
    // 报错仍推迟到链接期（与既有行为一致）。
    expr.atSymbol = atMethod->mangledName;
    for (auto& method : classIt->second->methods) {
        if (method->name == "set" && method->parameters.size() == 2) {
            expr.setSymbol = method->mangledName;
            break;
        }
    }

    // 下标表达式推导 + 与 at() 形参类型校验
    TypePtr idxType = inferType(expr.index);
    if (idxType && !typeCompatible(atMethod->parameters[0].type, idxType)) {
        error(std::format(
            "Subscript type '{}' does not match {}.at() parameter '{}'",
            idxType->toString(), actualType->name,
            atMethod->parameters[0].type->toString()), expr.location);
    }

    std::cout << std::format("{}[index] {}[{}] → {}    (sugar for {}.at(i))\n",
        inferIndent(), actualType->name,
        idxType ? idxType->toString() : "?",
        atMethod->returnType ? atMethod->returnType->toString() : "?",
        actualType->name);

    return atMethod->returnType;
}

// [expr.new]：解析类类型与构造实参，回填布局大小及选定的构造符号。
// 结果为类指针，CodeGen 据此分配并初始化对象。
TypePtr SemanticAnalyzer::inferNew(NewExpr& expr) {
    // P3 —— new Box<int>()：先按需实例化，className 原地改写为实例名（如 Box_int）。
    // 之后全管线（本函数查表、CodeGen::emitNew）只认实例名，模板痕迹在语义阶段一次性抹平。
    if (!expr.templateArgs.empty()) {
        TypePtr tid = Type::makeClass(expr.className);
        for (auto& arg : expr.templateArgs) {
            // 与 resolveType 的模板 id 分支同一套分派：
            // 类型实参递归 resolveType，非类型实参（NTTP 值）原样带过。
            tid->templateArgs.push_back(
                arg.isType() ? TemplateArg::ofType(resolveType(arg.type)) : arg);
        }
        TypePtr instance = getOrInstantiateClass(tid, expr.location);
        std::cout << std::format("  [new] template-id new {} → new {} (instantiated)\n",
            expr.className, instance->name);
        expr.className = instance->name;
        expr.templateArgs.clear();
    }

    auto it = m_classTypes.find(expr.className);
    if (it == m_classTypes.end()) {
        error(std::format("Unknown class '{}'", expr.className), expr.location);
    }

    // 推导构造函数实参类型
    std::vector<TypePtr> argTypes;
    for (auto& arg : expr.constructorArgs) {
        argTypes.push_back(inferType(arg));
    }

    // 查找匹配的构造函数
    auto classIt = m_classDecls.find(expr.className);
    if (classIt != m_classDecls.end()) {
        bool foundMatch = false;
        for (auto& method : classIt->second->methods) {
            // 构造函数按 kind 认；非构造的同名方法（罕见）也一并纳入候选
            auto ctor = method->kind == NodeKind::Constructor
                      ? std::static_pointer_cast<ConstructorDecl>(method) : nullptr;
            if (!ctor && method->name != expr.className) continue;
            if (method->parameters.size() == argTypes.size()) {
                bool match = true;
                for (size_t i = 0; i < argTypes.size(); ++i) {
                    if (!typeCompatible(method->parameters[i].type, argTypes[i])) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    foundMatch = true;
                    // 构造符号由 Sema 选定并回填，CodeGen 使用该符号，不按类名另行推算。
                    // 参考：clang::Sema::BuildCXXConstructExpr。
                    expr.ctorSymbol = method->mangledName;
                    break;
                }
            }
        }
        if (!foundMatch && (!argTypes.empty() || !classIt->second->methods.empty())) {
            if (!argTypes.empty()) {
                error(std::format("No matching constructor for class '{}' with {} arguments",
                    expr.className, argTypes.size()), expr.location);
            }
        }
    }

    std::cout << std::format("{}[new] {} → {}*    (size={} bytes, args={})\n",
        inferIndent(), expr.className, expr.className,
        it->second->classLayout.totalSize, argTypes.size());

    return Type::makePointer(it->second);
}

// [class.this]：this 只允许在非静态成员语境使用，类型为所属类指针。
TypePtr SemanticAnalyzer::inferThis(ThisExpr&) {
    if (m_currentClassName.empty()) {
        error("'this' used outside of class method", SourceLocation{});
    }

    auto it = m_classTypes.find(m_currentClassName);
    if (it == m_classTypes.end()) {
        error(std::format("Unknown class '{}'", m_currentClassName), SourceLocation{});
    }

    std::cout << std::format("{}[this] → {}*\n",
        inferIndent(), m_currentClassName);
    return Type::makePointer(it->second);
}

// [expr.dynamic.cast]：当前支持类指针到类指针的运行时转换。
// 检查源/目标类型后回填 RTTI 信息，由运行时助手查找并调整子对象地址。
TypePtr SemanticAnalyzer::inferDynamicCast(DynamicCastExpr& expr) {
    // 1. 目标类必须已声明
    auto targetIt = m_classTypes.find(expr.targetClassName);
    if (targetIt == m_classTypes.end()) {
        error(std::format("Unknown class '{}' in dynamic_cast", expr.targetClassName),
            expr.location);
    }

    // 2. 源表达式必须是"指向类的指针"
    TypePtr srcType = inferType(expr.operand);
    if (!srcType || !srcType->isPointer()
        || !srcType->pointeeType || !srcType->pointeeType->isClass()) {
        error("dynamic_cast operand must be a pointer to a class",
            expr.location);
    }
    std::string srcClass = srcType->pointeeType->name;

    // 3. 静态可达性检查：真 C++ 只要求两类"同属一个继承体系"（[expr.dynamic.cast]：源与
    //    目标须关联，兄弟类互转也合法，成败交由运行时的实际类型裁决）⇒ 编译期判据是
    //    "存在公共祖先"（含自身等同：a == b）。
    //    hasCommonAncestor(a, b)：收集 a 的祖先集，沿 b 链上溯查找交集。
    auto hasCommonAncestor = [this](const std::string& a, const std::string& b) {
        // BFS 收集 a 的全部祖先（遍历所有 baseClassNames，不再只走 firstBase）
        std::set<std::string> anc;
        std::vector<std::string> work = {a};
        while (!work.empty()) {
            std::string cur = work.back(); work.pop_back();
            if (!anc.insert(cur).second) continue;  // 已访问
            auto declIt = m_classDecls.find(cur);
            if (declIt != m_classDecls.end()) {
                for (auto& bn : declIt->second->baseClassNames)
                    work.push_back(bn);
            }
        }
        // BFS 检查 b 的祖先是否有交集
        work.push_back(b);
        std::set<std::string> visited;
        while (!work.empty()) {
            std::string cur = work.back(); work.pop_back();
            if (!visited.insert(cur).second) continue;
            if (anc.count(cur)) return true;
            auto declIt = m_classDecls.find(cur);
            if (declIt != m_classDecls.end()) {
                for (auto& bn : declIt->second->baseClassNames)
                    work.push_back(bn);
            }
        }
        return false;
    };
    if (!hasCommonAncestor(expr.targetClassName, srcClass)) {
        error(std::format("Cannot dynamic_cast '{}*' to '{}*': unrelated class types",
            srcClass, expr.targetClassName), expr.location);
    }

    std::cout << std::format("{}[dynamic_cast] {}* → {}*    (runtime RTTI check)\n",
        inferIndent(), srcClass, expr.targetClassName);
    return Type::makePointer(targetIt->second);
}

// ─── 错误处理 ────────────────────────────────────────────────────────────────
// 统一错误出口：携带源码位置（行/列）抛出异常，由 main() 捕获打印。
// 教学级采用 fail-fast 策略：遇到第一个语义错误即终止，不做错误恢复/继续收集
// （clang 的 DiagnosticsEngine 支持跳过错误继续）。
[[noreturn]] void SemanticAnalyzer::error(const std::string& msg, SourceLocation loc) {
    throw std::runtime_error(
        std::format("[Semantic Error] {}: {}", loc.toString(), msg));
}

// ═══ 诊断可视化：--dump-hierarchy / --dump-layout ═══════════════════════════
// 由 main.cpp 根据命令行标志调用，正常编译路径不受影响。
// typeinfo 形态判断规则（Itanium ABI 三种形态）：
//   bases.empty()     → 'C' (__class_type_info,     无基类, 链终点)
//   bases.size() == 1 → 'S' (__si_class_type_info,  单继承, base 指针)
//   bases.size() > 1  → 'V' (__vmi_class_type_info, 多继承, base 数组)

static char typeinfoForm(const ClassLayout& layout) {
    if (layout.bases.empty()) return 'C';
    if (layout.bases.size() == 1) return 'S';
    return 'V';
}

static const char* typeinfoName(char form) {
    switch (form) {
        case 'C': return "__class_type_info";
        case 'S': return "__si_class_type_info";
        case 'V': return "__vmi_class_type_info";
        default:  return "?";
    }
}

// 递归打印 typeinfo 链：当前类 → base → base.base → ...
static void printRTTIChain(
    const std::string& className,
    const std::unordered_map<std::string, TypePtr>& classTypes,
    const std::string& indent,
    std::set<std::string>& visited)
{
    std::string mangled = std::format("_ZTI{}{}", className.length(), className);

    if (visited.count(className)) {
        std::cout << std::format("{}{} → (已访问，跳过)\n", indent, mangled);
        return;
    }
    visited.insert(className);

    auto it = classTypes.find(className);
    if (it == classTypes.end()) {
        std::cout << std::format("{}{} → (未知类)\n", indent, mangled);
        return;
    }

    auto& layout = it->second->classLayout;
    char form = typeinfoForm(layout);

    std::cout << std::format("{}{} ['{}' {}]", indent, mangled, form, typeinfoName(form));

    if (layout.bases.empty()) {
        std::cout << " → 终止\n";
        return;
    }

    if (layout.bases.size() == 1) {
        auto& base = layout.bases[0];
        std::cout << std::format(" ──base──→\n");
        printRTTIChain(base.baseClassName, classTypes, indent, visited);
    } else {
        std::cout << std::format(" (base_count={})\n", layout.bases.size());
        for (size_t i = 0; i < layout.bases.size(); ++i) {
            auto& base = layout.bases[i];
            bool last = (i + 1 == layout.bases.size());
            std::string branch = last ? "└── " : "├── ";
            std::string childIndent = indent + (last ? "    " : "│   ");
            std::cout << std::format("{}{}bases[{}] @offset={}: ",
                indent, branch, i, base.offset);
            printRTTIChain(base.baseClassName, classTypes, childIndent, visited);
        }
    }
}

void SemanticAnalyzer::dumpHierarchy(
    const std::unordered_map<std::string, TypePtr>& classTypes)
{
    if (classTypes.empty()) return;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  类层次结构图 (Class Hierarchy Diagram)                         ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";

    // ── ① 继承树 ──
    std::cout << "\n  ┌─ 继承树 (Inheritance Tree) ──────────────────────────────────\n";

    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> roots;
    for (auto& [name, type] : classTypes) {
        if (type->classLayout.bases.empty()) {
            roots.push_back(name);
        } else {
            for (auto& base : type->classLayout.bases)
                children[base.baseClassName].push_back(name);
        }
    }

    auto isPoly = [](const TypePtr& t) { return t->classLayout.hasVTable; };
    auto getSize = [](const TypePtr& t) { return t->classLayout.totalSize; };

    std::set<std::string> treeVisited;
    std::function<void(const std::string&, const std::string&, const std::string&)> dfs =
        [&](const std::string& cls, const std::string& prefix, const std::string& branch) {
            auto it = classTypes.find(cls);
            if (it == classTypes.end()) return;
            auto& t = it->second;
            bool dup = !treeVisited.insert(cls).second;
            if (dup) {
                std::cout << std::format("  │ {}{}{}  [{}{}B]  ↑ (已展开)\n",
                    prefix, branch, cls,
                    isPoly(t) ? "polymorphic, " : "",
                    getSize(t));
                return;
            }
            std::cout << std::format("  │ {}{}{}  [{}{}B]\n",
                prefix, branch, cls,
                isPoly(t) ? "polymorphic, " : "",
                getSize(t));
            auto ci = children.find(cls);
            if (ci == children.end()) return;
            auto& kids = ci->second;
            std::sort(kids.begin(), kids.end());
            for (size_t i = 0; i < kids.size(); ++i) {
                bool last = (i + 1 == kids.size());
                dfs(kids[i],
                    prefix + (branch.empty() ? "" : (last ? "    " : "│   ")),
                    last ? "└── " : "├── ");
            }
        };

    std::sort(roots.begin(), roots.end());
    for (auto& r : roots) {
        dfs(r, "", "");
    }
    std::cout << "  └───────────────────────────────────────────────────────────────\n";

    // ── ② 每个类的详情框：按类名字母顺序打印（确保每个类只打印一次）──
    std::vector<std::string> ordered;
    for (auto& [name, type] : classTypes) {
        ordered.push_back(name);
    }
    std::sort(ordered.begin(), ordered.end());

    for (auto& name : ordered) {
        auto it = classTypes.find(name);
        if (it == classTypes.end()) continue;
        auto& layout = it->second->classLayout;
        char form = typeinfoForm(layout);

        std::cout << std::format("\n  ┌─ {} ─────────────────────────────────────────────────\n", name);

        // typeinfo 形态
        std::cout << std::format("  │  typeinfo : {} ['{}'", typeinfoName(form), form);
        if (form == 'C') std::cout << " 无基类";
        else if (form == 'S') std::cout << " 单继承";
        else if (form == 'V') std::cout << " 多继承";
        std::cout << "]\n";

        // size
        std::cout << std::format("  │  size     : {} bytes\n", layout.totalSize);

        // 基类
        if (!layout.bases.empty()) {
            for (auto& base : layout.bases) {
                std::cout << std::format("  │  基类     : {} ({}{}, offset={})\n",
                    base.baseClassName,
                    base.isPrimary ? "主基类" : "次基类",
                    base.hasVTable ? ", 多态" : "",
                    base.offset);
            }
        }

        // 字段（区分继承 vs 自有）
        if (!layout.fields.empty()) {
            bool hasInherited = false, hasOwn = false;
            for (auto& f : layout.fields) {
                if (f.sourceClass.empty() || f.sourceClass == name) hasOwn = true;
                else hasInherited = true;
            }
            if (hasInherited) {
                std::cout << "  │  继承字段 :\n";
                for (auto& f : layout.fields) {
                    if (!f.sourceClass.empty() && f.sourceClass != name) {
                        std::cout << std::format("  │    +{:<4} {:<12} : {} ({})  ← {}\n",
                            f.offset, f.name,
                            f.type ? f.type->toString() : "?",
                            f.size, f.sourceClass);
                    }
                }
            }
            if (hasOwn) {
                std::cout << "  │  自有字段 :\n";
                for (auto& f : layout.fields) {
                    if (f.sourceClass.empty() || f.sourceClass == name) {
                        std::cout << std::format("  │    +{:<4} {:<12} : {} ({})\n",
                            f.offset, f.name,
                            f.type ? f.type->toString() : "?",
                            f.size);
                    }
                }
            }
        }

        // 虚函数
        if (layout.hasVTable && !layout.vtableEntries.empty()) {
            std::cout << "  │  虚函数   :\n";
            for (auto& entry : layout.vtableEntries) {
                std::cout << std::format("  │    [{}] {} {}",
                    entry.index, vtableEntrySymbol(entry),
                    entry.isOverridden ? "(override)" : "");
                if (entry.thunkAdjust != 0)
                    std::cout << std::format("  thunk={}", entry.thunkAdjust);
                std::cout << "\n";
            }
            std::cout << std::format("  │  RTTI     : {}\n", layout.rttiMangledName);
        }

        // RTTI 链
        std::cout << "  │  RTTI 链  :\n";
        std::set<std::string> visited;
        printRTTIChain(name, classTypes, "  │    ", visited);

        std::cout << "  └───────────────────────────────────────────────────────────────\n";
    }
}

void SemanticAnalyzer::dumpLayout(
    const std::unordered_map<std::string, TypePtr>& classTypes)
{
    if (classTypes.empty()) return;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  类内存布局详图 (Memory Layout Detail)                          ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";

    // 按继承顺序：先打印无基类的，再打印有基类的
    std::vector<std::string> ordered;
    for (auto& [name, type] : classTypes) {
        if (type->classLayout.bases.empty())
            ordered.push_back(name);
    }
    for (auto& [name, type] : classTypes) {
        if (!type->classLayout.bases.empty())
            ordered.push_back(name);
    }
    std::sort(ordered.begin(), ordered.end());

    for (auto& name : ordered) {
        auto it = classTypes.find(name);
        if (it == classTypes.end()) continue;
        auto& layout = it->second->classLayout;

        std::cout << std::format("\n  ━━ {} ({} bytes) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n",
            name, layout.totalSize);

        // ── 第一层：对象内存 ──
        std::cout << std::format("  ┌─ {} 对象 ({}B) ─────────────────────────────────┐\n",
            name, layout.totalSize);
        if (layout.hasVTable) {
            std::cout << std::format("  │  +{:<3} _vptr ────────────────────────┐\n", 0);
        }
        for (auto& f : layout.fields) {
            std::string src = "";
            if (!f.sourceClass.empty() && f.sourceClass != name)
                src = std::format("  ← {}", f.sourceClass);
            std::cout << std::format("  │  +{:<3} {:<10} : {} ({}B){}\n",
                f.offset, f.name,
                f.type ? f.type->toString() : "?",
                f.size, src);
        }
        std::cout << "  └──────────────────────────────────────────────────────┘\n";

        if (!layout.hasVTable) continue;

        // ── 第二层：vtable ──
        std::string vtblName = std::format("_ZTV{}{}", name.length(), name);
        std::string rttiName = std::format("_ZTI{}{}", name.length(), name);
        std::cout << std::format("                                        │\n");
        std::cout << std::format("                                        ▼\n");
        std::cout << std::format("  ┌─ {} ──────────────────────────────────────────┐\n", vtblName);
        std::cout << std::format("  │  [-2]  offset-to-top = 0    ─→ 归顶: top = obj + 0\n");
        std::cout << std::format("  │  [-1]  typeinfo ptr ──────────┐  ─→ {} (RTTI)\n", rttiName);
        std::cout << std::format("  │  ── ↑ vptr 指向此处 ───────── │ ──\n");
        for (auto& entry : layout.vtableEntries) {
            std::cout << std::format("  │  [{:<2}]  {}{}\n",
                entry.index, vtableEntrySymbol(entry),
                entry.isOverridden ? "  (override)" : "");
        }
        std::cout << std::format("  └────────────────────────────── │ ──────────────────────────┘\n");

        // ── 第三层：typeinfo 链 ──
        std::cout << std::format("                                  │\n");
        std::cout << std::format("                                  ▼\n");

        char form = typeinfoForm(layout);
        std::string rttiBoxName = std::format("_ZTI{}{}", name.length(), name);
        std::cout << std::format("  ┌─ {} ({}) ────────────────────────────────────┐\n",
            rttiBoxName, form);
        std::cout << std::format("  │  +0   vptr  → 形态标记 '{}'\n", form);
        std::cout << std::format("  │  +8   name  → \"{}\" (mangled: {}{})\n",
            name, name.length(), name);

        if (form == 'V') {
            // VMI 类型：只列出 bases 数组，不递归展开（避免深层嵌套太复杂）
            std::cout << std::format("  │  +16  base_count = {}\n", layout.bases.size());
            for (size_t i = 0; i < layout.bases.size(); ++i) {
                auto& base = layout.bases[i];
                std::cout << std::format("  │  bases[{}]: {} @offset={} [{}]\n",
                    i, base.baseClassName, base.offset,
                    base.isPrimary ? "primary" : "secondary");
            }
            std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
        } else {
            // 'S'（单继承）或 'C'（无基类）：沿主基类链递归展开
            std::set<std::string> layoutVisited;
            layoutVisited.insert(name);
            std::string currentName = name;
            const ClassLayout* currentLayout = &layout;

            while (true) {
                if (currentLayout->bases.empty()) {
                    // 'C' 类型：链终止
                    std::cout << std::format("  │  (无 +16 字段 — 链终止)\n");
                    std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
                    break;
                }
                // 'S' 类型：有 +16 base 指针
                std::cout << std::format("  │  +16  base  ──────────────────────┐\n");
                std::cout << std::format("  └───────────────────────────────────── │ ─────┘\n");
                std::cout << std::format("                                        │\n");
                std::cout << std::format("                                        ▼\n");

                auto& base = currentLayout->bases[0];
                auto baseIt = classTypes.find(base.baseClassName);
                if (baseIt == classTypes.end()) {
                    std::cout << std::format("  ┌─ _ZTI{}{} (?) ──────────────────────────────────┐\n",
                        base.baseClassName.length(), base.baseClassName);
                    std::cout << std::format("  │  (基类不在当前翻译单元中)\n");
                    std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
                    break;
                }
                if (!layoutVisited.insert(base.baseClassName).second) {
                    std::cout << std::format("  ┌─ _ZTI{}{} → (已访问，跳过) ──────────────────────┐\n",
                        base.baseClassName.length(), base.baseClassName);
                    std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
                    break;
                }

                auto& baseLayout = baseIt->second->classLayout;
                char baseForm = typeinfoForm(baseLayout);
                std::string baseRttiName = std::format("_ZTI{}{}",
                    base.baseClassName.length(), base.baseClassName);
                std::cout << std::format("  ┌─ {} ({}) ────────────────────────────────────┐\n",
                    baseRttiName, baseForm);
                std::cout << std::format("  │  +0   vptr  → 形态标记 '{}'\n", baseForm);
                std::cout << std::format("  │  +8   name  → \"{}\"\n", base.baseClassName);

                currentName = base.baseClassName;
                currentLayout = &baseLayout;
            }
        }
    }
}

} // namespace minicc
