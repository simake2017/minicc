// 模板实参推导：结构合一、显式实参注入与不可推导位置检查。
// 参考 clang/Sema/SemaTemplateDeduction.cpp: DeduceTemplateArguments。
// 例：f(T*) 收到 int* 推出 T=int；f(T,T) 收到 int 与 double 因绑定冲突失败。

#include "template_deduction.h"
#include "sfinae.h"
#include <format>
#include <iostream>

namespace minicc {

// 偏特化复用逐对合一，但模式的值位须精确匹配，引用不做调用表达式调整。
// 例：Box<T*> 匹配 Box<int*>；Box<int,4> 不匹配 Box<int,5>。
bool TemplateDeducer::matchPattern(
    const std::vector<TemplateArg>& pattern,
    const std::vector<TemplateArg>& args,
    const std::vector<std::string>& paramNames,
    std::unordered_map<std::string, TypePtr>& subst,
    std::string& failReason) {

    if (pattern.size() != args.size()) {
        failReason = std::format("pattern has {} argument(s) but {} given",
                                 pattern.size(), args.size());
        return false;
    }

    DeductionResult out;
    for (size_t i = 0; i < pattern.size(); i++) {
        // 常量值模式位按值比较，不引入新绑定。
        // 例：Box<int,4> 只匹配对应值 4；待推导值名尚不支持。
        if (pattern[i].isValue()) {
            const TemplateArg& A = args[i];
            if (!A.isValue() || !pattern[i].equals(A)) {
                failReason = std::format(
                    "non-type pattern '{}' does not match argument '{}'",
                    pattern[i].toString(), A.toString());
                std::cout << std::format("  [deduction]   ✗ 值位不匹配：{}\n", failReason);
                return false;
            }
            std::cout << std::format(
                "  [deduction]   P={}  A={}  ⇒ 值位相等 ✓（NTTP 模式位，无绑定）\n",
                pattern[i].toString(), A.toString());
            continue;
        }

        // 类型位配到值实参 ⇒ 形态不符，直接判不匹配（避免下面按裸 TypePtr 取用时踩空）
        if (!args[i].isType()) {
            failReason = std::format(
                "type pattern '{}' does not match non-type argument '{}'",
                pattern[i].toString(), args[i].toString());
            std::cout << std::format("  [deduction]   ✗ 形态不匹配：{}\n", failReason);
            return false;
        }
        // ── SFINAE 的关键一步：先把模式位"归约"成具体类型 ──
        // void_t<...> / decltype(...) 这类模式位待求值，归约用的替换表正是前面各位
        // 已推出的绑定 —— 这就是从左到右逐位匹配的价值。
        // ★ SFINAE 吸收点 ①（全项目三处之一，见 include/sfinae.h 的收口点一览）：
        //   归约抛 SubstitutionFailure ⇒ 该偏特化不成立 ⇒ 移出候选集，试下一个。
        TypePtr P;
        std::string substReason;
        if (!Sfinae::attempt(
                std::format("偏特化模式第 {} 位 '{}'", i + 1, pattern[i].toString()),
                [&] { P = reducePattern(pattern[i].type, subst); },
                &substReason)) {
            failReason = std::format("substitution failed in pattern position {}: {}",
                                     i + 1, substReason);
            return false;
        }

        // 偏特化按类型结构匹配，不能使用函数调用的引用绑定宽容规则。
        // 例：S<int&> 不匹配 S<int>，即使 int 左值能绑定 int&。
        {
            // 剥掉顶层 const 再判引用性（const T& 的 const 是外层修饰）
            TypePtr pc = P;
            while (pc && pc->isConst() && pc->innerType) pc = pc->innerType;

            auto refKind = [](const TypePtr& t) -> int {
                if (!t) return 0;
                if (t->isLValueReference())  return 1;
                if (t->isRValueReference())  return 2;
                return 0;
            };
            int pr = refKind(pc);
            int ar = refKind(args[i].type);
            if (pr != 0 && pr != ar) {
                failReason = std::format(
                    "reference structure mismatch: pattern '{}' requires {} but "
                    "argument '{}' is {}",
                    P->toString(),
                    pr == 1 ? "an lvalue reference" : "an rvalue reference",
                    args[i].type ? args[i].type->toString() : "?",
                    ar == 0 ? "not a reference"
                            : (ar == 1 ? "an lvalue reference" : "an rvalue reference"));
                std::cout << std::format("  [deduction]   ✗ 引用结构不匹配：{}\n", failReason);
                return false;
            }
        }

        // 结构匹配的引用形态已在上方验证，此处按可绑定左值继续递归。
        if (!deducePair(P, args[i].type, /*argIsLValue=*/true, paramNames, subst, out,
                        /*structuralMatch=*/true)) {
            failReason = out.failureReason;
            return false;
        }
    }
    return true;
}

// 别名不引入新类型，模式与实参均解糖后再匹配。
// 例：Vec<T> 是 MyPtr<T> 的别名时，可与 MyPtr<int> 合一得到 T=int。
TypePtr TemplateDeducer::desugarAlias(
    const TypePtr& P,
    const std::unordered_map<std::string, TypePtr>& subst) {

    if (!P || !m_aliasResolver) return P;
    if (!P->isClass() || P->templateArgs.empty()) return P;
    if (!m_aliasResolver->isAliasTemplate(P->name)) return P;

    // 推导器的替换表是裸 TypePtr，替换引擎要 tagged —— 在这条缝上做转换
    TemplateInstantiator::TypeSubstitution tagged;
    for (auto& [k, v] : subst) tagged[k] = TemplateArg::ofType(v);

    TemplateInstantiator inst;
    inst.setDecltypeEvaluator(m_decltypeEval);
    inst.setMemberTypeResolver(m_memberResolver);
    inst.setAliasTemplateResolver(m_aliasResolver);

    TypePtr desugared = inst.substituteType(P, tagged);
    return desugared ? desugared : P;
}

// 用已有绑定归约依赖模式，求值失败移除候选。
// 例：void_t<typename T::type> 只有 T 提供该成员类型时才归约为 void。
TypePtr TemplateDeducer::reducePattern(
    const TypePtr& P,
    const std::unordered_map<std::string, TypePtr>& subst) {

    if (!P) return P;

    // ── 别名模板 id：先解糖成底层类型再匹配（[temp.alias]/1，理据见 desugarAlias）──
    // ★ P 侧不解糖则 `Vec<T>` 与 `MyPtr<int>` 永远不同名 ⇒ 合不上。
    //   解糖后 P = MyPtr<T>，T 仍是形参、保持依赖，正好交给逐位合一去绑定。
    // 对照 clang：比较前先 getCanonicalType，别名在这一步被剥掉。
    if (TypePtr desugared = desugarAlias(P, subst); desugared != P) {
        std::cout << std::format("  [deduce:alias] 模式位 {} 解糖 ⇒ {}\n",
            P->toString(), desugared->toString());
        // 递归：别名可能指向另一个别名，逐层剥到底
        return reducePattern(desugared, subst);
    }

    // ── void_t<...>：归约为 void（前提是各实参替换后都合法）──
    bool isVoidT = P->isClass() &&
                   (P->name == "std::void_t" || P->name == "void_t");
    if (!isVoidT) return P;

    if (P->templateArgs.empty()) {
        // 空实参表的 void_t<> 直接合法 → void
        Sfinae::probeNoArgs();
        return Type::makeVoid();
    }

    if (!m_decltypeEval) {
        Sfinae::probeNoEvaluator();
        return P;
    }

    // 替换表从 deducer 的 Subst（名字→TypePtr）转成 instantiator 的 tagged 表
    TemplateInstantiator::TypeSubstitution tagged;
    for (auto& [k, v] : subst) tagged[k] = TemplateArg::ofType(v);

    TemplateInstantiator inst;
    inst.setDecltypeEvaluator(m_decltypeEval);
    inst.setMemberTypeResolver(m_memberResolver);
    inst.setAliasTemplateResolver(m_aliasResolver);

    Sfinae::probeBegin(P->templateArgs.size());
    for (size_t i = 0; i < P->templateArgs.size(); i++) {
        if (!P->templateArgs[i].isType() || !P->templateArgs[i].type) continue;
        // ★ 关键：substituteType 内部遇到 decltype 会真的求值；
        //   表达式不合法则抛 SubstitutionFailure，向上冒泡给 matchPattern。
        TypePtr t = inst.substituteType(P->templateArgs[i].type, tagged);
        Sfinae::probeStep(i + 1, t ? t->toString() : "?");
    }
    Sfinae::probeAllOk();
    return Type::makeVoid();
}

// 取出类型节点对应的"模板参数名"，作为替换表的键。
// demo: TemplateParam("T") ⇒ "T" │ Class("T")（parseType 的产物）⇒ "T" │ 其余 ⇒ "?"
std::string TemplateDeducer::paramKey(const TypePtr& t) const {
    if (!t) return "?";
    if (t->isTemplateParam()) return t->templateParamName;
    if (t->isClass()) return t->name;
    return "?";
}

// 判定类型节点是否是"可绑定的变量"（本次模板参数表中的某个名字）。
// 【理论】合一算法前提：区分模式中的"变量"（进 bind）与"常量结构"（走恒等比较）。
// ★ 必须同时认 TemplateParam("T") 与 Class("T")：parseType 尚不知道 T 是模板参数，
//   会先建成 Class 节点。
bool TemplateDeducer::isTemplateParamName(const TypePtr& t,
                                          const std::vector<std::string>& paramNames) const {
    if (!t) return false;
    if (!t->isTemplateParam() && !t->isClass()) return false;
    std::string n = t->isTemplateParam() ? t->templateParamName : t->name;
    for (auto& p : paramNames) if (p == n) return true;
    return false;
}

// 同一形参的重复绑定必须一致。
// 例：f(T,T) 收到 int,int 成功；收到 int,double 失败。
bool TemplateDeducer::bind(const std::string& paramName, const TypePtr& type,
                           Subst& subst, DeductionResult& out,
                           const std::string& patternStr, const std::string& argStr) {
    auto it = subst.find(paramName);
    if (it == subst.end()) {
        subst[paramName] = type;
        std::string msg = std::format("{} := {}", paramName, type->toString());
        out.trace.push_back({patternStr, argStr, msg, true});
        std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ {}\n",
            patternStr, argStr, msg);
        return true;
    }
    if (it->second->equals(type)) {
        std::string msg = std::format("{} := {}（一致 ✓）", paramName, type->toString());
        out.trace.push_back({patternStr, argStr, msg, true});
        std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ {}\n",
            patternStr, argStr, msg);
        return true;
    }
    out.failureReason = std::format(
        "conflicting types for deduction of '{}': previously deduced '{}', now '{}'",
        paramName, it->second->toString(), type->toString());
    out.trace.push_back({patternStr, argStr, out.failureReason, false});
    std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
    return false;
}

// 单个 (P, A) 配对的结构化合一：自外向内递归剥壳（[temp.deduct.call]/[temp.deduct.type]）。
//   P=const X → 剥顶层 const │ P=T& → 左值检查后深入 │ P=T&& → 万能引用折叠/深入
//   P=T* → 对 pointee 深入    │ P=T → 值传递调整后绑定 │ P 非依赖 → 恒等比较
// demo: f(const T& x) 配 const int& 左值 ⇒ 剥 const ⇒ 左值 ✓ 深入 ⇒ (T, int) ⇒ T := int ✓
bool TemplateDeducer::deducePair(const TypePtr& P, TypePtr A, bool argIsLValue,
                                 const std::vector<std::string>& paramNames,
                                 Subst& subst, DeductionResult& out, bool structuralMatch) {
    if (!P || !A) {
        out.failureReason = "internal: null type in deduction";
        return false;
    }
    std::string pStr = P->toString();
    std::string aStr = A->toString();

    // 调用实参传入的是声明类型，须剥引用以还原表达式类型（[expr.type]）。
    // 偏特化的实参本身就是类型，必须保留引用层。
    // 例：int& r 作为调用实参按 int 推导；模板实参 S<int&> 仍保留 int&。
    if (!structuralMatch && A->isReference()) {
        A = A->referencedType;
        aStr = A->toString();
    }

    // ── 别名模板 id 先解糖（[temp.alias]/1）──
    // ★ 放在【每一个递归层】而不只是入口：别名常被引用/const 包着（`const Vec<T>&`
    //   要剥两层壳才露出 Vec<T>），只在入口解一次就漏网了。desugarAlias 对非别名
    //   只是查一下名字，开销可忽略。
    if (TypePtr desugared = desugarAlias(P, subst); desugared != P) {
        std::cout << std::format("  [deduction]   P={:<12} ⇒ 别名解糖为 {}\n",
            pStr, desugared->toString());
        return deducePair(desugared, A, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // ── P = const X：剥 P 的顶层 const ──
    // [temp.deduct.type]：顶层 cv 不影响推导（本项目 parseType 把 const T& 建成
    // Const(LRef(T))，剥掉外层 const 后走引用分支）。内层 const（const T*）不剥，
    // 随递归继续参与推导。
    if (P->isConst()) {
        std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ 剥顶层 const\n", pStr, aStr);
        return deducePair(P->innerType, A, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // ── P = T&：左值引用参数 ──
    // [temp.deduct.call]：P 是引用类型时以被引用类型参与推导，但绑定要过 [dcl.init.ref]。
    // ★ 判据不在本处写死，走 Type 的 referenceBindsValueCategory（判据单点）——
    //   这里原先写的是"右值一律拒"，于是 `const T&` 收右值被误拒（已知边界）：
    //   `const T&` 建成 LValueRef(Const(T))，外层不是 const ⇒ 剥 const 那支救不了它。
    if (P->isLValueReference()) {
        std::string why;
        if (!referenceBindsValueCategory(P, argIsLValue, &why)) {
            out.failureReason = std::format("{} (argument '{}')", why, aStr);
            out.trace.push_back({pStr, aStr, out.failureReason, false});
            std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
            return false;
        }
        // ★ structuralMatch：两侧【对称】剥壳 —— 偏特化匹配是模板实参表的类型对类型
        //   结构等价（[temp.class.spec.match]/2），模板实参可以是引用类型，具体引用位
        //   （如 Duo<T, int&> 的 int&）配 int& 实参必须命中。若拿 A=int& 原样递归，
        //   内层落到"非依赖恒等"会比成 int vs int& 误拒（clang 判匹配，已知边界）。
        //   安全性：structuralMatch 仅由 matchPattern 传入，其 refKind 预检已保证
        //   P 是引用 ⇒ A 必是同种引用。
        const TypePtr& innerA = structuralMatch ? A->referencedType : A;
        return deducePair(P->referencedType, innerA, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // 转发引用只在裸模板形参 T 的 T&& 上成立（[temp.deduct.call]）。
    // 例：左值 int 得 T=int&；右值 int 得 T=int，替换后再由引用折叠归一。
    if (P->isRValueReference()) {
        TypePtr inner = P->referencedType;
        if (!structuralMatch && isTemplateParamName(inner, paramNames) && argIsLValue) {
            std::cout << std::format(
                "  [deduction]   P={:<12} A={:<12} (lvalue) ⇒ 万能引用折叠路径\n", pStr, aStr);
            return bind(paramKey(inner), Type::makeLValueReference(A),
                        subst, out, pStr, aStr);
        }
        // structuralMatch：同左值引用分支对称剥壳（refKind 预检保证 A 是引用）。
        const TypePtr& innerA = structuralMatch ? A->referencedType : A;
        return deducePair(inner, innerA, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // ── P = T*：实参必须是指针，递归 pointee ──
    // [temp.deduct.type] 结构化等价：复合类型逐层拆解后递归合一。
    // demo: P=T* A=int* ⇒ 递归到 (T, int) ⇒ T := int ✓
    if (P->isPointer()) {
        if (!A->isPointer()) {
            out.failureReason = std::format(
                "parameter pattern '{}' expects pointer argument, got '{}'", pStr, aStr);
            out.trace.push_back({pStr, aStr, out.failureReason, false});
            std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
            return false;
        }
        return deducePair(P->pointeeType, A->pointeeType, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // ── P = T（裸模板参数，值传递）──
    // [temp.deduct.call]：值传递调整 —— 实参的顶层引用/const 不参与推导。
    // demo: identity(x) 中 x 是 const int& ⇒ 剥顶层引用、再剥 const ⇒ T := int
    // ★ 调整仅适用于【调用推导】：[expr.type]/1 说"表达式永远没有引用类型"，而偏特化
    //   匹配（structuralMatch）里 A 是【模板实参类型】，本来就可以是引用 —— 剥了反而错：
    //   Same<T,T> 配 Same<int&,int&>，剥壳后 T := int（clang 是 T := int&）；更糟的是
    //   Same<T,T> 配 Same<int&,int>，逐位剥壳后"一致 ✓"误命中偏特化（clang 判 T 推导
    //   冲突、不命中，回落主模板）—— 静默选错特化（已知边界）。
    if (isTemplateParamName(P, paramNames)) {
        TypePtr adjusted = A;
        if (!structuralMatch) {
            if (adjusted->isReference()) adjusted = adjusted->referencedType; // ① 剥顶层引用：值传递拷贝的是对象本身
            if (adjusted->isConst())     adjusted = adjusted->innerType;      // ② 剥顶层 const：拷贝后副本可写，顶层 cv 丢失
        }
        if (adjusted != A) {
            std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ 值传递调整 A'={}\n",
                pStr, aStr, adjusted->toString());
        }
        return bind(paramKey(P), adjusted, subst, out, pStr, aStr);
    }

    // 类模板 id 逐位合一，实例类型通过出身信息还原原模板及实参。
    // 例：MyPtr<T> 与 MyPtr<int> 得 T=int；模板名不同则不能合一。
    if (P->isClass() && !P->templateArgs.empty()) {
        bool sameTemplate =
            A->isClass() &&
            (A->isTemplateInstance() ? A->templateOriginName == P->name
                                     : A->name == P->name);
        const std::vector<TemplateArg>& aArgs =
            A->isTemplateInstance() ? A->templateOriginArgs : A->templateArgs;

        if (!sameTemplate || aArgs.size() != P->templateArgs.size()) {
            out.failureReason = std::format(
                "class template pattern '{}' does not match argument '{}'",
                pStr, aStr);
            out.trace.push_back({pStr, aStr, out.failureReason, false});
            std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
            return false;
        }

        std::cout << std::format(
            "  [deduction]   P={:<12} A={:<12} ⇒ 同类模板 {}（出身 {}），逐位合一\n",
            pStr, aStr, P->name, A->templateOriginName);

        for (size_t i = 0; i < P->templateArgs.size(); i++) {
            const TemplateArg& pa = P->templateArgs[i];
            const TemplateArg& aa = aArgs[i];

            // 模式位是类型 ⇒ 实参位也必须是类型，递归合一
            if (pa.isType() && pa.type) {
                if (!aa.isType() || !aa.type) {
                    out.failureReason = std::format(
                        "template argument {} of '{}': pattern expects a type, "
                        "argument is a value '{}'", i + 1, P->name, aa.toString());
                    out.trace.push_back({pStr, aStr, out.failureReason, false});
                    return false;
                }
                if (!deducePair(pa.type, aa.type, argIsLValue, paramNames, subst, out, structuralMatch))
                    return false;
                continue;
            }

            // 模式位是值（NTTP）⇒ 要求逐位相等（教学简化：
            // 不做 `Buf<N>` 从值反推 N 的算式推导，见 docs/learn/18 的边界说明）
            if (pa.toString() != aa.toString()) {
                out.failureReason = std::format(
                    "non-type template argument {} of '{}' mismatches: pattern "
                    "'{}' vs argument '{}'", i + 1, P->name, pa.toString(), aa.toString());
                out.trace.push_back({pStr, aStr, out.failureReason, false});
                return false;
            }
        }
        out.trace.push_back({pStr, aStr, "类模板结构合一 ✓", true});
        return true;
    }

    // ── P 非依赖：要求恒等（[temp.deduct.type]）──
    // P 中不含模板参数的部分必须与 A 结构一致，否则该候选在重载决议 S6 中被淘汰。
    if (P->equals(A)) {
        out.trace.push_back({pStr, aStr, "恒等 ✓", true});
        std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ 恒等 ✓\n", pStr, aStr);
        return true;
    }
    out.failureReason = std::format(
        "no match for non-dependent parameter: P='{}' vs A='{}'", pStr, aStr);
    out.trace.push_back({pStr, aStr, out.failureReason, false});
    std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
    return false;
}

// 完整调用推导先注入显式实参，再合一形参/实参，最后检查未推导位置。
DeductionResult TemplateDeducer::deduce(
    const TemplateDeclPtr& tmpl,
    const std::vector<TypePtr>& argTypes,
    const std::vector<bool>& argIsLValue,
    const std::vector<TypePtr>& explicitArgs) {

    DeductionResult result;
    Subst subst;
    auto& func   = tmpl->funcTemplate;
    auto& params = tmpl->typeParams;

    std::string paramList;
    for (size_t i = 0; i < params.size(); i++) {
        if (i > 0) paramList += ", ";
        paramList += params[i];
    }
    std::cout << std::format("  [deduction] ▶ {} — 模板参数 <{}>，实参 {} 个\n",
        func->name, paramList, argTypes.size());

    // ── S3：显式实参占模板参数表前缀 [temp.arg.explicit] ──
    // 显式实参按位置绑定前缀，剩余参数靠调用推导补全（如 make<int>(3.14) 只给第一个）。
    for (size_t i = 0; i < explicitArgs.size() && i < params.size(); i++) {
        subst[params[i]] = explicitArgs[i];
        std::cout << std::format("  [deduction]   显式给定: {} := {}\n",
            params[i], explicitArgs[i]->toString());
    }
    if (!explicitArgs.empty()) {
        std::cout << std::format("  [deduction]   ── 显式 {} 个 / 推导补全剩余 {} 个 ──\n",
            explicitArgs.size(),
            params.size() > explicitArgs.size() ? params.size() - explicitArgs.size() : 0);
    }

    // ── 函数参数个数检查（[expr.call]）：不符则该候选直接出局 ──
    if (argTypes.size() != func->parameters.size()) {
        result.failureReason = std::format(
            "argument count mismatch: {} expects {}, got {}",
            func->name, func->parameters.size(), argTypes.size());
        std::cout << std::format("  [deduction]   ✗ {}\n", result.failureReason);
        return result;
    }

    // ── S2：逐对 P/A 推导 [temp.deduct.call] ──
    // 每个 P_i 与 A_i 递归合一，任一对失败立即返回（failureReason 已由 deducePair 填好）。
    for (size_t i = 0; i < func->parameters.size(); i++) {
        // 形参模式若写着别名模板 id（`Vec<T>`）先解糖：实参那侧（`MyPtr<int>`）
        // 早在调用点解过糖，两侧不同名就永远合不上。
        TypePtr P = desugarAlias(func->parameters[i].type, subst);
        if (P != func->parameters[i].type) {
            std::cout << std::format("  [deduction]   形参位 {} 解糖 ⇒ {}\n",
                func->parameters[i].type->toString(), P->toString());
        }
        if (!deducePair(P, argTypes[i], argIsLValue[i], params, subst, result)) {
            return result; // failureReason 已置
        }
    }

    // ── S4：不可推导上下文 —— 走完实参仍有未绑定参数 ──
    // [temp.deduct.type]：只出现在返回类型等"不可推导位置"的参数无法从调用推出，
    // 必须显式给定（如 make<T>()）；诊断文案提示用户写出显式实参。
    for (auto& p : params) {
        if (!subst.count(p)) {
            result.failureReason = std::format(
                "non-deduced context: template parameter '{}' cannot be deduced "
                "(only appears in non-deduced positions such as the return type; "
                "specify it explicitly, e.g. {}<...>())", p, func->name);
            std::cout << std::format("  [deduction]   ✗ {}\n", result.failureReason);
            return result;
        }
    }

    // ── 成功：按模板参数表顺序输出最终实参（显式 + 推导补全），交给 S5 实例化 ──
    result.success = true;
    std::string final;
    for (auto& p : params) {
        result.deducedArgs.push_back(subst[p]);
        if (!final.empty()) final += ", ";
        final += p + "=" + subst[p]->toString();
    }
    std::cout << std::format("  [deduction] ◀ 推导成功: <{}>\n", final);
    return result;
}

} // namespace minicc
