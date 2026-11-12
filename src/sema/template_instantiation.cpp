// 模板实例化与符号编码实现。
// 参考 clang/Sema/SemaTemplateInstantiateDecl.cpp、TreeTransform.h；标准 [temp.subst]。
// 例：Box<T>{T v;} 代入 int 得独立字段；T&& 代入 int& 得 int&。

#include "template_instantiation.h"
#include <format>
#include <iostream>

namespace minicc {

// ═════════════════════════════════════════════════════════════════════════════
// NameMangler 实现
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// 类型编码：类型树 → Itanium ABI mangling 片段（理论见 docs/learn/13）
// ─────────────────────────────────────────────────────────────────────────────
// 【编码表】v=void b=bool i=int d=double P=指针 R=左值引用 O=右值引用 K=const
//           类名 = <长度><名字>（如 7MyClass）；模板参数名原样输出（实例化后不应出现）
// demo: int* ⇒ Pi │ const int ⇒ Ki │ MyClass ⇒ 7MyClass
std::string NameMangler::encodeType(TypePtr type,
                                    const std::vector<std::string>& typeParams) {
    if (!type) return "v"; // void

    switch (type->kind) {
        case TypeKind::Void:   return "v";
        case TypeKind::Bool:   return "b";
        case TypeKind::Double: return "d";
        // ── 整型家族（Itanium ABI §5.1.2 <builtin-type>）──
        // ★ 无符号版各有独立编码，最易漏：把 `unsigned int` 编成 "i" 会让
        //   `Buf<4u>` 与 `Buf<4>` 生成同一个汇编符号（一个是 unsigned 形参、
        //   一个是 int 形参，本就是不同模板，撞名即静默错链）。
        // demo: char ⇒ c │ signed char ⇒ a │ unsigned char ⇒ h │ short ⇒ s
        //       unsigned short ⇒ t │ int ⇒ i │ unsigned int ⇒ j │ long ⇒ l
        //       unsigned long ⇒ m │ long long ⇒ x │ unsigned long long ⇒ y
        case TypeKind::Char:  return "c";
        case TypeKind::SChar: return "a";
        case TypeKind::UChar: return "h";
        case TypeKind::Short: return "s";
        case TypeKind::UShort: return "t";
        case TypeKind::Int:   return "i";
        case TypeKind::UInt:  return "j";
        case TypeKind::Long:  return "l";
        case TypeKind::ULong: return "m";
        case TypeKind::LongLong:  return "x";
        case TypeKind::ULongLong: return "y";
        case TypeKind::Pointer:
            return "P" + encodeType(type->pointeeType, typeParams);
        case TypeKind::Array:
            return std::format("A{}_{}", type->arraySize, encodeType(type->elementType, typeParams));
        case TypeKind::LValueReference:
            return "R" + encodeType(type->referencedType, typeParams); // GCC ABI: R = lvalue ref
        case TypeKind::RValueReference:
            return "O" + encodeType(type->referencedType, typeParams); // GCC ABI: O = rvalue ref
        case TypeKind::Const:
            return "K" + encodeType(type->innerType, typeParams);      // GCC ABI: K = const
        case TypeKind::Class:
            return std::format("{}{}", type->name.size(), type->name);
        case TypeKind::TemplateParam:
            // ★ 仅在函数模板实例的签名里走这段（typeParams 非空）：
            //   形参编成 Itanium <template-param>，它【引用】模板实参表的第 n 项
            //   —— 序号 0 ⇒ T_，序号 1 ⇒ T0_，序号 2 ⇒ T1_（编码值比序号少一）。
            //   类模板路径传的是空表 ⇒ 落到下面原样输出，既有符号不变。
            for (size_t i = 0; i < typeParams.size(); i++) {
                if (typeParams[i] == type->templateParamName) {
                    return i == 0 ? std::string("T_")
                                  : std::format("T{}_", i - 1);
                }
            }
            return type->templateParamName;
        case TypeKind::Auto:
            return "Da"; // 不应该出现（auto 应该在阶段3已被消除）
        case TypeKind::Decltype:
            // ★ 不应该出现：decltype 必须已在替换阶段（Case 1.5）求值成具体类型。
            //   漏求值会产出形如 `_ZN…7decltype…` 的非法符号，链接期才炸、极难排查，
            //   故就地报出挡在编译期（clang 不会有此状态，Sema 层必已解析完毕）。
            std::cerr << std::format(
                "[内部错误] decltype 类型节点未在替换阶段求值就进入了 mangling：{}\n",
                type->toString());
            return "Dt";   // Itanium ABI 的 decltype 编码（正常路径不会走到）
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// 模板实例化符号名（理论见 docs/learn/13）
// 格式: _Z + 模板名长度 + 模板名 + I + 参数编码... + E
// demo: MyPtr<int> ⇒ _Z5MyPtrIiE │ twice<int> ⇒ _Z5twiceIiE（类/函数模板实例共用）
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// 单实参在「无损键 / 实例名清洗」里的渲染
// ─────────────────────────────────────────────────────────────────────────────
// 与 TemplateArg::toString() 的唯一区别：当该位形参声明为 `auto` 时补上实参带来的
// 形态（`4:int` / `4:long` / `1:bool`）。见 losslessArgumentsKey 的说明。
// ★ 键与实例名必须出自同一份渲染 —— 只改一处会让"缓存键分开了、汇编符号还撞着"，
//   或者反过来。故两者共用本函数，是唯一来源。
static std::string renderArgLossless(const TemplateArg& a, const TemplateParamPtr& p) {
    std::string s = a.toString();
    if (p && p->kind == TemplateParamKind::NonType && p->nonType && p->nonType->isAuto()
        && a.isValue() && a.valueType) {
        s += ":" + a.valueType->toString();
    }
    return s;
}

static const TemplateParamPtr& paramAt(const std::vector<TemplateParamPtr>& params, size_t i) {
    static const TemplateParamPtr kNone;
    return i < params.size() ? params[i] : kNone;
}

std::string NameMangler::losslessArgumentsKey(
    const std::string& templateName,
    const std::vector<TemplateArg>& args,
    const std::vector<TemplateParamPtr>& params) {

    std::string key = templateName + "<";
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) key += ",";
        key += renderArgLossless(args[i], paramAt(params, i));
    }
    key += ">";
    return key;
}

// ─────────────────────────────────────────────────────────────────────────────
// 符号名净化（产生端与使用端同源 ⇒ 天然一致）
// ─────────────────────────────────────────────────────────────────────────────
// ★ 命名空间里的类/模板名带 "::"（"M::B" / "N::C"），直接拼进符号标签会让
//   汇编器报 "junk at end of line"，故统一净化。⚠ 净化后 "N::S" 与 "N_S"
//   会撞名 —— 教学实现接受（见 docs/learn/26）。
// mangleTemplateInstance 也用它（模板名带作用域 ⇒ "M::B"）。
static std::string symbolSafe(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '$') ? c : '_';
    }
    return out;
}

std::string NameMangler::mangleTemplateInstance(
    const std::string& templateName,
    const std::vector<TemplateArg>& args) {

    // 作用域名先清洗，再按清洗结果计算名称长度；此处不是完整 Itanium 嵌套名编码。
    const std::string safeName = symbolSafe(templateName);
    std::string result = std::format("_Z{}{}I", safeName.size(), safeName);

    for (auto& arg : args) {
        // 按 [temp.arg] 的形态分派：类型实参 → 直接编码类型（Box<int> ⇒ …IiE）；
        // 非类型实参（NTTP）→ <expr-primary> L<类型编码><值>E（Buf<4> ⇒ …ILi4EE）。
        if (arg.isType()) {
            result += encodeType(arg.type);
        }
        else if (arg.isTemplate()) {
            // ── 模板模板实参 [temp.arg.template] ──
            // 实参是【模板名】：Itanium 按 <name> 直接编码（与类类型同形）。
            // demo: Wrap<Box, int> ⇒ _Z4WrapI3BoxiE
            // ★ 不能落到下面的 NTTP 分支 —— 那会把它编成 Li0E（值 0 的 expr-primary），
            //   产出一个"看着像模像样"却与 clang 完全不同的符号。
            // 对照 clang：clang/include/clang/AST/Mangle.h:AbstractManglingParser::mangleName
            //   —— TemplateTemplateArg 走它。
            result += encodeType(Type::makeClass(arg.templateName));
        }
        else {
            // <expr-primary>：L 开头 E 收尾，中间是「类型编码 + 值」。
            // ★ 负数按 Itanium 规则编成 n<绝对值>（-4 ⇒ "n4"）—— '-' 不是合法的
            //   mangling 字符。
            result += "L";
            // 按实参的【形态】编码：bool ⇒ b、int ⇒ i（缺失时按 int 兜底）
            result += arg.valueType ? encodeType(arg.valueType) : "i";
            result += (arg.value < 0)
                          ? std::format("n{}", -arg.value)
                          : std::to_string(arg.value);
            result += "E";                       // 收 <expr-primary>
        }
    }

    result += "E";
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数模板实例符号名（[temp]，Itanium ABI §5.1.8 <bare-function-type>）
// 格式: _Z + 名 + I<模板实参>E + <返回类型> + <各参数类型>
// demo: template<class T> T twice(T x)      以 T=int 实例化 ⇒ _Z5twiceIiET_T_
//                                                             └┬┘ └┬┘
//                                                         返回 T_  参数 T_
//       template<class T> T pick(T a, int n)              ⇒ _Z4pickIiET_i
//       template<class T> void f(T a, T b)                ⇒ _Z1fIiEvT_T0_
// ★ 与类模板实例唯一的差别就是这个末尾段：函数模板的返回类型也是签名的一部分
//   （无法从名字反推），必须编进去；参数表同理。少了它，`template<class T>
//   T f(T)` 与 `template<class T> T f(T, int)` 在同一次实例化下会撞成同一符号。
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::mangleFunctionTemplateInstance(
    const std::string& funcName,
    const std::vector<TemplateArg>& args,
    const TypePtr& returnType,
    const std::vector<Parameter>& params,
    const std::vector<std::string>& typeParams) {

    std::string result = mangleTemplateInstance(funcName, args);

    // <bare-function-type>：返回类型在前，各参数类型依次跟上。
    result += encodeType(returnType, typeParams);
    for (auto& param : params) {
        result += encodeType(param.type, typeParams);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数符号名（理论见 docs/learn/13）
// 格式: _Z + [N + 类名长度 + 类名] + 函数名长度 + 函数名 + 参数编码... [+ E]
// demo: MyClass::foo(int) ⇒ _ZN7MyClass3fooEi │ twice(int) ⇒ _Z5twicei
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::mangleFunction(
    const std::string& funcName,
    const std::string& className,
    const std::vector<Parameter>& params) {

    std::string result = "_Z";

    if (!className.empty()) {
        result += "N";
        result += std::format("{}{}", className.size(), className);
    }

    result += std::format("{}{}", funcName.size(), funcName);

    for (auto& param : params) {
        result += encodeType(param.type);
    }

    if (!className.empty()) {
        result += "E";
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// RTTI 符号名（[class.rtti]，理论见 docs/learn/11）
// ─────────────────────────────────────────────────────────────────────────────
// 格式 _ZTI + 类名；demo: MyClass ⇒ _ZTI7MyClass
std::string NameMangler::mangleRTTI(const std::string& className) {
    const std::string safe = symbolSafe(className);   // 先净化再算长度，自洽
    return std::format("_ZTI{}{}", safe.size(), safe);
}

// ─────────────────────────────────────────────────────────────────────────────
// vtable 符号名（理论见 docs/learn/11）
// ─────────────────────────────────────────────────────────────────────────────
// 格式 _ZTV + 类名（只有虚函数的类才生成）；demo: MyClass ⇒ _ZTV7MyClass
std::string NameMangler::mangleVTable(const std::string& className) {
    const std::string safe = symbolSafe(className);
    return std::format("_ZTV{}{}", safe.size(), safe);
}

// ═════════════════════════════════════════════════════════════════════════════
// TemplateInstantiator 实现
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// 类模板实例化 [temp.inst]（理论见 docs/learn/19、27）
// ─────────────────────────────────────────────────────────────────────────────
// 实例化 = 结构化替换：深拷贝蓝图 AST，逐【类型位置】替换；流程对应函数体内的
// 0 → 6 编号步骤（含 2a 撞名守卫 / 5.5 类内别名替换）。
// 实例名清洗：把实参的【人读串】洗成合法的汇编符号字符。
// ★ 必须尽量【单射】：'*' 与 '&' 各配一个字母（Box<int*>→Box_intP、Box<int&>→Box_intR），
//   否则两种不同实参洗出同一个符号。',' '<' '>' → '_' 仍非单射，
//   由调用点的撞名守卫兜底（宁可报错，也绝不产出重复符号）。
// 类模板实例与成员模板实例共用本函数。
// 对照 clang：Itanium ABI 的 <substitution> 是另一种思路（去重而非清洗），
//   本实现只求"符号合法且够用"，人读名与真正的编码（NameMangler）是两件事。
std::string sanitizeSymbolChars(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (c == ' ') continue;                       // 空格剔除
        switch (c) {
            case '*': out += 'P'; break;              // Pointer   Box<int*>  → Box_intP
            case '&': out += 'R'; break;              // Reference Box<int&>  → Box_intR
            case '-': out += 'N'; break;              // Negative  Buf<-3>    → Buf_N3
            case '+': out += 'A'; break;              // 目前进不来（实参只认整数字面量）
            // ★ auto 形参位的「值:形态」分隔符（K<4:long>）。':' 不是合法的汇编符号
            //   字符，漏了这条会让 .s 里出现 `K_4:long:` 这样的行，as 直接报语句错。
            case ':': out += 'C'; break;              // Colon     K<4:long>  → K_4Clong
            case ',': case '<': case '>':
                out += '_'; break;
            default:  out += c;   break;
        }
    }
    return out;
}

ClassDeclPtr TemplateInstantiator::instantiate(
    TemplateDeclPtr templateDecl,
    const std::vector<TemplateArg>& args,
    const TypeSubstitution* substOverride) {

    // ── 0. 形参表选取 + 实参个数校验（[temp.arg.explicit]）──
    // ★ 必须用 templateParams（带 kind 的结构化表），不能用 typeParams（裸名字表）：
    //   后者把 `template<int N>` 的 N 也记成类型形参名，分不出哪一位是 NTTP。
    const auto& params = templateDecl->templateParams;

    // ── 两条路径：主模板 vs 特化（[temp.class.spec] / [temp.expl.spec]）──
    // 主模板：args[i] ↔ params[i] 逐位对应，按 kind 分派 + 校验。
    // 特化  ：替换表由 Sema 的偏特化匹配（TemplateDeducer::matchPattern）给出，与特化
    //         自己的形参表【不】逐位对应（Box<T*,T> 1 个形参 ↔ Box<double*,double>
    //         2 个实参），故直接采用外部传入的替换表，跳过逐位与个数校验。
    // args 在两条路径下都只用于「实例名生成 + mangling」，语义是使用点的实参。
    TypeSubstitution subst;
    if (substOverride) {
        subst = *substOverride;
        std::cout << std::format(
            "  [subst:map] (specialization) substitution supplied by pattern matching: ");
        if (subst.empty()) {
            // 全特化：形参表为空，没有需要绑定的未知量，替换表本就该是空的 ——
            // 特化体里的类型全是具体类型，逐字克隆即可。
            std::cout << "(none — fully concrete body, verbatim clone)";
        }
        for (auto& [k, v] : subst) std::cout << std::format("{} := {} ", k, v.toString());
        std::cout << "\n";
    }
    else {
        if (args.size() != params.size()) {
            throw std::runtime_error(std::format(
                "[Instantiate Error] template '{}' expects {} argument(s), got {}",
                templateDecl->templateName(),
                params.size(), args.size()));
        }

        // ── 1. 构建替换映射：按 kind 分派，类型实参进 type 槽，值实参进 value 槽 ──
        //    例: template<class T>      + Box<int> → { "T" → TemplateArg{Type,int} }
        //        template<int N>        + Buf<4>   → { "N" → TemplateArg{Integral,4} }
        //        template<class T, int N> + Pair<int,8>
        //                                      → { "T"→{Type,int}, "N"→{Integral,8} }
        //    混排天然可处理——因为分派依据是每一位形参自己的 kind，而非实参长相。
        for (size_t i = 0; i < params.size(); i++) {
            const TemplateParam& p = *params[i];

            // 形态自检（SemanticAnalyzer::checkTemplateArguments 已把过关，
            // 此处是 TemplateInstantiator 被直接调用时的兜底）
            if (p.kind == TemplateParamKind::Type && args[i].isValue()) {
                throw std::runtime_error(std::format(
                    "[Instantiate Error] template argument {} for '{}' ('{}') must be "
                    "a type, but '{}' is a value",
                    i + 1, templateDecl->templateName(), p.name, args[i].toString()));
            }
            if (p.kind == TemplateParamKind::NonType && args[i].isType()) {
                throw std::runtime_error(std::format(
                    "[Instantiate Error] template argument {} for '{}' ('{}') must be "
                    "a value, but '{}' is a type",
                    i + 1, templateDecl->templateName(), p.name, args[i].toString()));
            }

            subst[p.name] = args[i];
            std::cout << std::format(
                "  [subst:map] '{}' ({}) := {}\n", p.name,
                p.kind == TemplateParamKind::Type ? "type" : "non-type",
                args[i].toString());
        }
    }

    // ── 2. 生成实例化后的类名 ──
    // 实例名直接进汇编符号（方法名 <类名>_<方法>），必须剔除空格与 '<' '>' ',' 等
    // 非法字符；NTTP 值实参走同一套清洗（Buf<4> ⇒ Buf_4），天然区分不同实例。
    // ★ 这里只是「人读的实例名」，真正的符号编码在 NameMangler。
    //   清洗规则见 sanitizeSymbolChars（与成员模板实例共用同一份，避免两处漂移）。
    // ★ 逐位渲染与缓存键同源（renderArgLossless）—— auto 位的形态在这里也必须出现，
    //   否则 `K<4>`(int) 与 `K<4L>`(long) 会洗出同一个汇编符号前缀 `K_4`，
    //   撞名守卫会当场拦下（比静默合并好，但正确做法是让两者各得其所）。
    std::string instanceName = templateDecl->classTemplate->name;
    for (size_t i = 0; i < args.size(); i++) {
        instanceName += "_" + sanitizeSymbolChars(
            renderArgLossless(args[i], paramAt(params, i)));
    }

    // ── 2a. ★ 撞名守卫 ──
    // 清洗非单射，极端情况下两个不同实参表会洗出同一实例名 ⇒ 两条实例产出同一批汇编
    // 符号，而 as 只会报 `symbol 'X' is already defined`，指不到根因 —— 这里拦一道
    // 并说清谁跟谁撞了。无损键与 Sema 的 m_classInstanceCache 同构，键相同即"本来就
    // 是同一条实例"，放行（单元测试会重放同一条）。
    {
        // ★ 键必须单射（见 NameMangler::losslessArgumentsKey）—— 此前这里就地拼
        //   `toString()`，与 Sema 的缓存键是同一份逻辑写了两遍；现在两处共用一个原语。
        const std::string lossless = NameMangler::losslessArgumentsKey(
            templateDecl->classTemplate->name, args, params);

        auto [it, inserted] = m_instanceNameOwner.emplace(instanceName, lossless);
        if (!inserted && it->second != lossless) {
            throw std::runtime_error(std::format(
                "[Instantiate Error] 实例名撞车：'{}' 与 '{}' 清洗后都得到汇编符号名 "
                "'{}'。两者是不同类型，必须产出不同符号 —— 请给实例名清洗规则"
                "（template_instantiation.cpp 的 Step 2a）补上区分字符。",
                it->second, lossless, instanceName));
        }
        if (inserted) {
            std::cout << std::format(
                "  [instantiate:name] '{}' → 汇编符号前缀 '{}'\n", lossless, instanceName);
        }
    }

    // ── 2b. 蓝图摘要打印 ──
    // ★ 形参表必须由 templateParams 渲染（才能把 NTTP 打成 `int N` 而非 `typename N`）。
    // 特化路径额外打印命中版本（主模板/偏特化/全特化），是"三者择优"的观测点。
    const std::string kindTag =
        templateDecl->isExplicitSpec() ? " [EXPLICIT SPECIALIZATION]"
      : templateDecl->isPartialSpec()  ? " [PARTIAL SPECIALIZATION]"
                                       : " [primary template]";
    std::cout << std::format("\n  ╔══ Template Instantiation ═════════════════════════╗\n");
    std::cout << std::format("  ║ Blueprint: {} <{}>{}\n",
        templateDecl->classTemplate->name,
        params.empty() ? "?" :
            [&]() { std::string s; for (size_t i = 0; i < params.size(); i++) {
                if (i > 0) s += ", ";
                s += (params[i]->kind == TemplateParamKind::Type)
                         ? "typename " + params[i]->name
                     : (params[i]->kind == TemplateParamKind::NonType)
                         ? (params[i]->nonType ? params[i]->nonType->toString() : "?")
                               + " " + params[i]->name
                         : "template <...> class " + params[i]->name;
            } return s; }(),
        kindTag);
    if (templateDecl->isSpecialization()) {
        std::cout << std::format("  ║ Pattern:   {}<{}>\n",
            templateDecl->classTemplate->name,
            [&] { std::string s;
                  for (size_t i = 0; i < templateDecl->specPattern.size(); i++) {
                      if (i > 0) s += ", ";
                      // 类型位印类型名、值位印值（TemplateArg::toString 已分派）
                      s += templateDecl->specPattern[i].toString();
                  } return s; }());
    }
    std::cout << std::format("  ║ Instance:  {}\n", instanceName);

    // 打印替换映射
    std::cout << "  ║ Substitution map: { ";
    for (auto& [param, arg] : subst) {
        std::cout << std::format("'{}' → '{}', ", param, arg.toString());
    }
    std::cout << "}\n";

    // 3. 深拷贝类声明
    auto newClass = std::make_shared<ClassDecl>();
    newClass->name = instanceName;
    newClass->baseClassNames = templateDecl->classTemplate->baseClassNames;
    newClass->location = templateDecl->location;

    // 4. 克隆字段：demo: T* data ⇒ int* data（substituteType 递归进复合类型）
    std::cout << "  ║ ── Field Substitution ──\n";
    for (auto& field : templateDecl->classTemplate->fields) {
        std::cout << std::format("  ║   field '{}' : {} → ",
            field.name, field.type ? field.type->toString() : "?");
        auto cloned = cloneField(field, subst);
        std::cout << std::format("{}\n", cloned.type ? cloned.type->toString() : "?");
        newClass->fields.push_back(cloned);
    }

    // 5. 克隆方法：返回类型/形参/体内局部变量声明中的 T 全部替换，表达式走 cloneExpr
    std::cout << "  ║ ── Method Substitution ──\n";
    const std::string& blueprintName = templateDecl->classTemplate->name;
    for (auto& method : templateDecl->classTemplate->methods) {
        std::cout << std::format("  ║   method '{}' : ", method->name);

        // 打印参数类型替换
        for (size_t i = 0; i < method->parameters.size(); i++) {
            auto& param = method->parameters[i];
            if (i > 0) std::cout << ", ";
            std::cout << std::format("{}:{}", param.name,
                param.type ? param.type->toString() : "?");
        }
        std::cout << std::format(" → {}\n",
            method->returnType ? method->returnType->toString() : "void");

        auto cloned = cloneMethod(method, subst, instanceName);
        // ★ 构造/析构函数名绑定在类名上：蓝图里的 Box/~Box 必须随实例改名，否则符号
        //   与 CodeGen 发射的调用符号（类名_类名）对不上 ⇒ undefined reference。
        //   对照 clang：实例化时按新类名重建 CXXConstructorDecl 的 DeclName。
        if (cloned->kind == NodeKind::Constructor && cloned->name == blueprintName) {
            cloned->name = instanceName;
        }
        if (cloned->kind == NodeKind::Destructor && cloned->name == "~" + blueprintName) {
            cloned->name = "~" + instanceName;
        }
        newClass->methods.push_back(cloned);
    }

    // 5.5 克隆类内类型别名（using X = T; / typedef T X;，理论见 docs/learn/24）
    // ★ 每个实例必须自带"已替换"的别名表，否则 Box<int>::type 会拿到蓝图里的裸 T。
    for (const auto& aliasName : templateDecl->classTemplate->typeAliasOrder) {
        auto src = templateDecl->classTemplate->typeAliases.find(aliasName);
        if (src == templateDecl->classTemplate->typeAliases.end()) continue;
        TypePtr sub = substituteType(src->second, subst);
        newClass->typeAliases[aliasName] = sub;
        newClass->typeAliasOrder.push_back(aliasName);
        std::cout << std::format("  ║   alias '{}' : {} → {}\n",
            aliasName,
            src->second ? src->second->toString() : "?",
            sub ? sub->toString() : "?");
    }

    // 成员模板随外层类克隆，替换外层形参，保留自身形参待调用时推导。
    // 例：Box<T>::get<U> 在 Box<int> 中先替换 T，U 仍是占位符。
    // 共享蓝图会让不同外层实例互相污染。
    if (!templateDecl->classTemplate->memberTemplates.empty()) {
        std::cout << "  ║ ── Member Template Substitution ──\n";
    }
    for (auto& mt : templateDecl->classTemplate->memberTemplates) {
        auto clonedMt = std::make_shared<TemplateDecl>();
        clonedMt->location        = mt->location;
        clonedMt->templateParams  = mt->templateParams;   // 内层形参 U 原样带过
        clonedMt->typeParams      = mt->typeParams;
        clonedMt->isMemberTemplate = true;
        clonedMt->funcTemplate    = cloneMethod(mt->funcTemplate, subst, instanceName);

        // 内层形参名列表（只为日志可读：说明"这一位没被替换，等调用点推导"）
        std::string innerNames;
        for (size_t i = 0; i < mt->templateParams.size(); i++) {
            if (i > 0) innerNames += ", ";
            innerNames += mt->templateParams[i]->name;
        }
        std::cout << std::format(
            "  ║   member template '{}' <{}> → 挂到实例类 '{}'（内层形参不替换）\n",
            mt->funcTemplate->name, innerNames, instanceName);
        newClass->memberTemplates.push_back(clonedMt);
    }

    // static 成员每个实例独立声明，类型与初始化式都应用外层替换。
    // 例：Box<int>::value 与 Box<double>::value 分别登记。
    for (auto& sm : templateDecl->classTemplate->staticDataMembers) {
        StaticDataMember cloned;
        cloned.name = sm.name;
        cloned.type = substituteType(sm.type, subst);
        cloned.isConstexpr = sm.isConstexpr;
        if (sm.initializer) {
            cloned.initializer = cloneExpr(sm.initializer, subst);
        }
        newClass->staticDataMembers.push_back(std::move(cloned));
    }

    // 嵌套类是模板成员，须递归克隆并替换外层形参（[class.nest]、[temp.inst]）。
    // 例：Box<T>::Inner{T v;} 在 Box<int> 中 v 为 int。
    // 克隆保留裸名，完整限定名交给 Sema 统一登记。
    if (!templateDecl->classTemplate->nestedClasses.empty()) {
        std::cout << "  ║ ── Nested Class Substitution ──\n";
    }
    for (auto& nc : templateDecl->classTemplate->nestedClasses) {
        auto clonedNc = cloneNestedClass(nc, subst);
        newClass->nestedClasses.push_back(clonedNc);
        std::cout << std::format("  ║   nested class '{}' → 克隆（字段/方法随形参替换）\n",
            nc->name);
    }

    // 6. 生成 mangled name
    std::string mangledName = NameMangler::mangleTemplateInstance(
        templateDecl->classTemplate->name, args);

    std::cout << std::format("  ║ Mangled: {} → {}\n", instanceName, mangledName);
    std::cout << std::format("  ╚═══════════════════════════════════════════════════╝\n");

    m_instantiatedClasses.push_back(newClass);
    return newClass;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数模板实例化 S5（理论见 docs/learn/05）
// ─────────────────────────────────────────────────────────────────────────────
// 推导（S2~S4）给出 typeArgs 后：建替换表 → 复用 cloneMethod 深拷贝蓝图函数
//   → 生成 mangled 符号 → 交回 Sema 注册并用具体类型分析函数体（两阶段查找第二阶段）。
// demo: twice(3) ⇒ T := int ⇒ void twice(T x){...} 的每个 T 换成 int
//                   ⇒ 符号 _Z5twiceIiE ⇒ 进 m_instantiatedFunctions
// ─────────────────────────────────────────────────────────────────────────────
FuncDeclPtr TemplateInstantiator::instantiateFunction(
    TemplateDeclPtr templateDecl,
    const std::vector<TypePtr>& typeArgs,
    const std::string& ownerClassName) {

    // 推导引擎 S2~S4 只产出裸 TypePtr（函数模板的 NTTP 尚未实现），此处统一包成
    // TemplateArg::ofType 放进同一张替换表 —— 表本身类型/值两形态通用。
    TypeSubstitution subst;
    for (size_t i = 0; i < templateDecl->typeParams.size()
         && i < typeArgs.size(); i++) {
        subst[templateDecl->typeParams[i]] = TemplateArg::ofType(typeArgs[i]);
    }

    auto& blueprint = templateDecl->funcTemplate;

    // mangler 收 tagged 实参表，而这条路径手上只有裸类型，故再包一遍。
    std::vector<TemplateArg> targs;
    targs.reserve(typeArgs.size());
    for (auto& a : typeArgs) targs.push_back(TemplateArg::ofType(a));

    // 函数模板符号须包含返回与形参类型，不能只编码模板实参。
    // 例：同为 f<int> 的 f(T) 与 f(T*) 必须区分（Itanium ABI §5.1.8）。
    std::string mangled;
    if (ownerClassName.empty()) {
        mangled = NameMangler::mangleFunctionTemplateInstance(
            blueprint->name, targs,
            blueprint->returnType, blueprint->parameters,
            templateDecl->typeParams);
    } else {
        mangled = sanitizeSymbolChars(ownerClassName + "_" + blueprint->name);
        for (auto& a : targs) mangled += "_" + sanitizeSymbolChars(a.toString());
    }

    std::cout << std::format("\n  ╔══ Function Template Instantiation (S5) ═══════╗\n");
    std::cout << std::format("  ║ Blueprint: {} <{}>\n",
        blueprint->name,
        [&]() { std::string s;
            for (size_t i = 0; i < templateDecl->typeParams.size(); i++) {
                if (i > 0) s += ", ";
                s += templateDecl->typeParams[i];
            }
            return s.empty() ? "?" : s; }());
    std::cout << "  ║ Substitution: { ";
    for (auto& [p, t] : subst) {
        std::cout << std::format("{} → {}, ", p, t.toString());
    }
    std::cout << "}\n";

    // 复用方法克隆（ownerClassName = "" 表示自由函数；成员模板传所属类名，
    // 使实例带隐式 this —— CodeGen 与 Sema 都靠 ownerClassName 判"有没有 this"）
    FuncDeclPtr instance = cloneMethod(blueprint, subst, ownerClassName);
    instance->mangledName = mangled;

    std::cout << std::format("  ║ Symbol: {} → {}\n", blueprint->name, mangled);
    std::cout << std::format("  ╚═══════════════════════════════════════════════╝\n");

    m_instantiatedFunctions.push_back(instance);
    return instance;
}

// 按类型树结构替换，未命中的非依赖类型保持原样。
// 参考 clang/Sema/TreeTransform.h: TransformType；标准 [temp.subst]。
// 例：T* 代入 int → int*；T&& 代入 int& → int&；typename T::type 延迟查成员。
TypePtr TemplateInstantiator::substituteType(
    TypePtr type, const TypeSubstitution& subst) {

    if (!type) return nullptr;

    // ── Case 1: 模板参数 → 直接替换 ──
    // demo: T 配 {T := int} ⇒ int；不在表中则原样保留（如外层模板的形参）
    if (type->isTemplateParam()) {
        auto it = subst.find(type->templateParamName);
        if (it != subst.end()) {
            // ★ 类型位置只接受【类型】实参：命中值实参说明 NTTP 名被当类型用了，
            //   必须报硬错误而非把值硬塞成类型
            //   （clang 在更早的实参校验期就拒：
            //   clang/lib/Sema/SemaTemplate.cpp:Sema::CheckTemplateArgument）。
            if (it->second.isValue()) {
                Sfinae::fail(std::format(
                    "[Subst Error] non-type template parameter '{}' is used as a "
                    "type (its value is '{}')",
                    type->templateParamName, it->second.toString()));
            }
            std::cout << std::format("    [subst] ★ TemplateParam '{}' → '{}' (direct replacement)\n",
                type->templateParamName, it->second.type->toString());
            return it->second.type;
        }
        std::cout << std::format("    [subst] TemplateParam '{}' not in substitution map, keep as-is\n",
            type->templateParamName);
        return type;
    }

    // decltype 保留的表达式在替换后求值，此时形参已有实际类型。
    // 例：decltype(x) 按声明类型，decltype((x)) 还取决于值类别。

    if (type->isDecltype()) {
        std::cout << "    [subst] decltype: 先替换操作数，再求值（两段式的第二段）\n";

        // ① 操作数表达式做替换
        ExprPtr concrete = cloneExpr(type->decltypeExpr, subst);

        // ② 求值
        if (!m_decltypeEval) {
            // 单元测试路径：没有挂求值器 → 原样保留（保持"延迟"可见）
            std::cout << "    [subst] decltype: no evaluator attached — stays deferred\n";
            return Type::makeDecltype(concrete, type->decltypeParen);
        }

        // 求值失败会抛 SubstitutionFailure —— 让它向上传播：
        // 那正是 SFINAE 的信号，由 matchPattern / 重载决议负责捕获。
        TypePtr result = m_decltypeEval->evaluateDecltype(concrete, type->decltypeParen);
        std::cout << std::format("    [subst] ★ decltype 求值完成 → {}\n",
            result ? result->toString() : "?");
        return result;
    }

    // ── Case 2.1: 数组 T[N] → 递归替换 elementType ──
    if (type->isArray() && type->elementType) {
        TypePtr newElem = substituteType(type->elementType, subst);
        if (newElem != type->elementType) {
            auto result = Type::makeArray(newElem, type->arraySize);
            return result;
        }
    }

    // ── Case 2: 指针 T* → 递归替换 pointee ──
    // demo: T* 配 {T := int} ⇒ int*；pointee 无变化则复用原节点（结构共享）
    if (type->isPointer() && type->pointeeType) {
        std::cout << std::format("    [subst] Pointer({}*) → recursing into pointee...\n",
            type->pointeeType->toString());
        TypePtr newPointee = substituteType(type->pointeeType, subst);
        if (newPointee != type->pointeeType) {
            auto result = Type::makePointer(newPointee);
            std::cout << std::format("    [subst] ★ Pointer substituted: {}* → {}*\n",
                type->pointeeType->toString(), newPointee->toString());
            return result;
        }
    }

    // ── Case 3: 左值引用 T& → 递归替换 + 引用折叠 ──
    // demo: T& 配 {T := int} ⇒ int&；配 {T := int&} ⇒ int& & 折叠为 int&
    if (type->isLValueReference() && type->referencedType) {
        std::cout << std::format("    [subst] LValueRef({}&) → recursing into referenced type...\n",
            type->referencedType->toString());
        TypePtr newInner = substituteType(type->referencedType, subst);

        // ★ 内层若已是引用，结果必须【重新构造】，不能直接把 newInner 返回：
        //   内层是右值引用时原样返回会得到 `int&&`，而 [dcl.ref]/6 要求 `int&`
        //   （"& 赢"只写在 Type::makeLValueReference 里，调用点不得自行判断）。
        if (newInner->isReference()) {
            TypePtr folded = Type::makeLValueReference(newInner);
            std::cout << std::format("    [subst] ★ Reference collapsing: {}& → {} (& wins)\n",
                newInner->toString(), folded->toString());
            return folded;
        }

        if (newInner != type->referencedType) {
            auto result = Type::makeLValueReference(newInner);
            std::cout << std::format("    [subst] ★ LValueRef substituted: {}& → {}&\n",
                type->referencedType->toString(), newInner->toString());
            return result;
        }
    }

    // ── Case 4: 右值引用 T&& → 递归替换 + 引用折叠 ──
    // ★ 万能引用（forwarding reference）的关键路径：
    //   T = int ⇒ int&& │ T = int& ⇒ int& && ⇒ int& │ T = int&& ⇒ int&& && ⇒ int&&
    // demo: T&& 配 {T := int&} ⇒ int& && ⇒ int&（左值引用赢，规则见 Case 3）
    if (type->isRValueReference() && type->referencedType) {
        std::cout << std::format("    [subst] RValueRef({}&&) → recursing into referenced type...\n",
            type->referencedType->toString());
        TypePtr newInner = substituteType(type->referencedType, subst);

        // 引用折叠
        if (newInner->isLValueReference()) {
            // int& && → int&（左值引用永远赢）
            std::cout << std::format("    [subst] ★ Reference collapsing: {}&& → {} (lvalue ref wins!)\n",
                newInner->toString(), newInner->toString());
            return newInner;
        }
        if (newInner->isRValueReference()) {
            // int&& && → int&&（右值引用 + 右值引用 = 右值引用）
            std::cout << std::format("    [subst] ★ Reference collapsing: {}&& → {} (rvalue ref stays)\n",
                newInner->toString(), newInner->toString());
            return newInner;
        }

        // 普通类型：T&& → actualType&&
        if (newInner != type->referencedType) {
            auto result = Type::makeRValueReference(newInner);
            std::cout << std::format("    [subst] ★ RValueRef substituted: {}&& → {}&&\n",
                type->referencedType->toString(), newInner->toString());
            return result;
        }
    }

    // ── Case 5: const T → 递归替换 inner ──
    // demo: const T 配 {T := int} ⇒ const int
    if (type->isConst() && type->innerType) {
        std::cout << std::format("    [subst] Const(const {}) → recursing into inner type...\n",
            type->innerType->toString());
        TypePtr newInner = substituteType(type->innerType, subst);
        if (newInner != type->innerType) {
            auto result = Type::makeConst(newInner);
            std::cout << std::format("    [subst] ★ Const substituted: const {} → const {}\n",
                type->innerType->toString(), newInner->toString());
            return result;
        }
    }

    // 依赖成员类型先替换限定者，再交给 Sema 查表。
    // 例：typename T::type 在 T=S 后查 S::type；缺失成员由 SFINAE 边界裁决。
    if (type->isClass() && type->isNestedName() && type->nestedQualifier) {
        std::cout << std::format("    [subst] 依赖限定名 {}::{} → 先替换限定者...\n",
            type->nestedQualifier->toString(), type->name);
        TypePtr newQual = substituteType(type->nestedQualifier, subst);

        if (newQual && newQual->isTemplateParam()) {
            std::cout << std::format(
                "    [subst] 限定者 '{}' 仍是模板形参（外层模板的）⇒ 保持依赖\n",
                newQual->templateParamName);
            return type;
        }

        if (m_memberResolver) {
            // 查不到 / 限定者不是类 ⇒ resolveMemberType 抛 SubstitutionFailure
            TypePtr member = m_memberResolver->resolveMemberType(newQual, type->name);
            std::cout << std::format("    [subst] ★ {}::{} ⇒ {}\n",
                newQual ? newQual->toString() : "?", type->name,
                member ? member->toString() : "?");
            return member;
        }

        // 单元测试路径（没挂 resolver）：替换限定者后原样保留，保持"延迟"可见
        if (newQual != type->nestedQualifier) {
            auto result = Type::makeClass(type->name);
            result->nestedQualifier = newQual;
            std::cout << std::format("    [subst] 无成员查表器 ⇒ 保持延迟: {}\n",
                result->toString());
            return result;
        }
        return type;
    }

    // 类模板 id 的依赖可能藏在实参中，不能只按类名查替换表。
    // 例：MyPtr<T> 在 T=int 后变为 MyPtr<int>。
    TypePtr idNode = type;
    if (type->isClass() && !type->templateArgs.empty()) {
        std::vector<TemplateArg> newArgs;
        newArgs.reserve(type->templateArgs.size());
        bool changed = false;
        for (const auto& arg : type->templateArgs) {
            if (arg.isType() && arg.type) {
                // ── 实参位的"裸名字"其实是 NTTP 名 ⇒ 还原成值实参 ──
                // Parser 的实参分流只看 Token 形态（整数字面量 → 值，其余 → 类型），
                // 于是 `Buf<N>` 的 N（非类型形参名）被建成 Class("N") 类型节点。
                // ★ 替换表是唯一权威：N 在表里绑的是【值】⇒ 这里的裸 N 只能是值实参。
                // demo: template<int N> using BufA = Buf<N>; BufA<4> ⇒ Buf<4> ✓
                // 对照 clang：Parser 维护 TemplateParameterScope，无此还原步骤。
                auto sit = (arg.type->isClass() && arg.type->templateArgs.empty() &&
                            !arg.type->isNestedName())
                               ? subst.find(arg.type->name)
                               : subst.end();
                if (sit != subst.end() && sit->second.isValue()) {
                    std::cout << std::format(
                        "    [subst] ★ 实参 '{}' 在替换表里绑定的是值 ⇒ 还原为 NTTP 值实参 {}\n",
                        arg.type->name, sit->second.toString());
                    newArgs.push_back(sit->second);
                    changed = true;
                    continue;
                }

                TypePtr na = substituteType(arg.type, subst);
                if (na != arg.type) changed = true;
                newArgs.push_back(TemplateArg::ofType(na));
            } else {
                newArgs.push_back(arg);   // NTTP 值实参：值不随类型替换而变
            }
        }
        if (changed) {
            auto rebuilt = Type::makeClass(type->name);
            rebuilt->templateArgs    = newArgs;
            rebuilt->nestedQualifier = type->nestedQualifier;
            std::cout << std::format("    [subst] ★ 模板 id 实参替换: {} → {}\n",
                type->toString(), rebuilt->toString());
            idNode = rebuilt;
        }
    }

    // 模板模板形参替换模板名，实参仍须逐位替换。
    // 例：C<T> 在 C=Box、T=int 后成为 Box<int>。
    if (idNode->isClass() && !idNode->templateArgs.empty()) {
        auto it = subst.find(idNode->name);
        if (it != subst.end() && it->second.isTemplate()) {
            auto rebuilt = Type::makeClass(it->second.templateName);
            rebuilt->templateArgs    = idNode->templateArgs;
            rebuilt->nestedQualifier = idNode->nestedQualifier;
            std::cout << std::format(
                "    [subst] ★ 模板模板形参 '{}' → 模板 '{}'：{} → {}\n",
                idNode->name, it->second.templateName,
                idNode->toString(), rebuilt->toString());
            idNode = rebuilt;
        }
    }

    // 别名模板不产生新类型，代入实参后交给 Sema 展开。
    // 例：Ptr<T>=T*，Ptr<int> → int*。
    if (idNode->isClass() && !idNode->templateArgs.empty() && m_aliasResolver) {
        std::vector<TemplateArg> substArgs;
        substArgs.reserve(idNode->templateArgs.size());
        for (const auto& arg : idNode->templateArgs)
            substArgs.push_back(arg.isType() && arg.type
                                    ? TemplateArg::ofType(substituteType(arg.type, subst))
                                    : arg);
        TypePtr expanded = m_aliasResolver->expandAliasTemplate(idNode->name, substArgs);
        if (expanded) {
            std::cout << std::format(
                "    [subst] ★ 别名模板 {}<...> 解糖 ⇒ {}\n",
                idNode->name, expanded->toString());
            return expanded;
        }
    }

    // 重建过模板 id（名字不是别名、实参已替换）⇒ 返回重建结果
    if (idNode != type) return idNode;

    // ── Case 6: 类类型节点里裸着的形参名（简化处理）──
    // demo: Class("T") 配 {T := int} ⇒ int（parseType 把裸 T 建成 Class 节点，故按名兜底）
    if (type->isClass()) {
        auto it = subst.find(type->name);
        if (it != subst.end() && it->second.isType()) {
            std::cout << std::format("    [subst] ★ Class '{}' matches template param → '{}'\n",
                type->name, it->second.type->toString());
            return it->second.type;
        }
        // 命中但值是 Integral：同样说明 NTTP 名被当类型用了（见 Case 1 的说明）。
        if (it != subst.end()) {
            // 同 Case 1：立即上下文内的替换失败 → 可恢复，发软失败信号
            Sfinae::fail(std::format(
                "[Subst Error] non-type template parameter '{}' is used as a type "
                "(its value is '{}')",
                type->name, it->second.toString()));
        }
    }

    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// 克隆字段
// ─────────────────────────────────────────────────────────────────────────────
// 类型过 substituteType，其余元信息（offset/size/access）原样复制。
// demo: MyPtr<int> 的 "data : T*" ⇒ "data : int*"
FieldInfo TemplateInstantiator::cloneField(
    const FieldInfo& field, const TypeSubstitution& subst) {

    FieldInfo newField;
    newField.name = field.name;
    newField.type = substituteType(field.type, subst);
    newField.offset = field.offset;
    newField.size = field.size;
    newField.access = field.access;
    return newField;
}

// ─────────────────────────────────────────────────────────────────────────────
// 克隆方法 —— 声明重建，对应 clang TreeTransform
// ─────────────────────────────────────────────────────────────────────────────
// 返回类型、形参类型、函数体三处的 T 全部替换；ownerClassName 指向实例类名
// （自由函数传 ""，见 instantiateFunction）。克隆不携带旧的语义分析结果 —— 实例方法
// 交回 Sema 用具体类型重新检查（两阶段查找的第二阶段）。
FuncDeclPtr TemplateInstantiator::cloneMethod(
    FuncDeclPtr method, const TypeSubstitution& subst,
    const std::string& newClassName) {

    // ★ 必须按源节点种类重建节点类型：构造函数 → ConstructorDecl（保住 initList/
    //   isDefaultCtor），析构 → DestructorDecl（保住 isDefaultDtor）。一律建
    //   FunctionDecl 会让合成构造/析构失效，CodeGen 的 Class_Class 调用链接不到符号。
    //   对照 clang：TreeTransform 按 DeclKind 分派 TransformConstructorDecl/DestructorDecl。
    FuncDeclPtr newMethod;
    // 按种类分派（[class.copy.ctor]/[class.dtor]）：构造带初始化列表，析构带
    // isDefaultDtor 标志，普通方法走空壳。
    if (method->kind == NodeKind::Constructor) {
        auto ctor = std::static_pointer_cast<ConstructorDecl>(method);
        auto clonedCtor = std::make_shared<ConstructorDecl>();
        clonedCtor->isDefaultCtor = ctor->isDefaultCtor;
        // 初始化列表：成员名原样保留，实参表达式递归克隆
        for (auto& init : ctor->initList) {
            CtorInitializer clonedInit;
            clonedInit.memberName = init.memberName;
            clonedInit.location = init.location;
            for (auto& arg : init.arguments) {
                clonedInit.arguments.push_back(cloneExpr(arg, subst));
            }
            clonedCtor->initList.push_back(clonedInit);
        }
        newMethod = clonedCtor;
    } else if (method->kind == NodeKind::Destructor) {
        auto dtor = std::static_pointer_cast<DestructorDecl>(method);
        auto clonedDtor = std::make_shared<DestructorDecl>();
        clonedDtor->isDefaultDtor = dtor->isDefaultDtor;
        newMethod = clonedDtor;
    } else {
        newMethod = std::make_shared<FunctionDecl>();
    }
    newMethod->name = method->name;
    newMethod->isVirtual = method->isVirtual;
    newMethod->isStatic = method->isStatic;
    newMethod->isOverride = method->isOverride;
    newMethod->ownerClassName = newClassName;
    newMethod->location = method->location;

    // 替换返回类型
    std::cout << std::format("    [clone:method] '{}' return type: {} → ",
        method->name, method->returnType ? method->returnType->toString() : "void");
    newMethod->returnType = substituteType(method->returnType, subst);
    std::cout << std::format("{}\n", newMethod->returnType ? newMethod->returnType->toString() : "void");

    // 克隆参数
    for (auto& param : method->parameters) {
        Parameter newParam;
        newParam.name = param.name;
        std::cout << std::format("    [clone:method]   param '{}' : {} → ",
            param.name, param.type ? param.type->toString() : "?");
        newParam.type = substituteType(param.type, subst);
        std::cout << std::format("{}\n", newParam.type ? newParam.type->toString() : "?");
        newMethod->parameters.push_back(newParam);
    }

    // 克隆函数体
    if (method->body) {
        auto newBody = std::make_shared<BlockStmt>();
        newBody->location = method->body->location;
        for (auto& stmt : method->body->statements) {
            newBody->statements.push_back(cloneStmt(stmt, subst));
        }
        newMethod->body = newBody;
    }

    return newMethod;
}

// 递归克隆嵌套类，字段、方法、别名与成员模板中的外层形参一并替换。
// 例：Box<int>::Inner 与 Box<double>::Inner 各持独立节点。
// 保持裸名供 Sema 登记完整路径，避免重复拼接限定名。
ClassDeclPtr TemplateInstantiator::cloneNestedClass(
    const ClassDeclPtr& src, const TypeSubstitution& subst) {

    auto cloned = std::make_shared<ClassDecl>();
    cloned->name = src->name;                    // 裸名；前缀化由 processClassDecl 收口
    cloned->baseClassNames = src->baseClassNames;
    cloned->location = src->location;
    cloned->isForwardDecl = src->isForwardDecl;

    for (auto& f : src->fields) {
        auto clonedField = cloneField(f, subst);
        std::cout << std::format("    [clone:nested]   field '{}' : {} → {}\n",
            f.name, f.type ? f.type->toString() : "?",
            clonedField.type ? clonedField.type->toString() : "?");
        cloned->fields.push_back(std::move(clonedField));
    }
    // ownerClassName 交空：processClassDecl 会把它写成【登记名】（"Box_int::Inner"），
    //   那是唯一定名点 —— 这里若先填一个"猜的"名字，两处就会漂。
    for (auto& m : src->methods) {
        cloned->methods.push_back(cloneMethod(m, subst, ""));
    }
    for (auto& sm : src->staticDataMembers) {
        StaticDataMember c;
        c.name = sm.name;
        c.type = substituteType(sm.type, subst);
        c.isConstexpr = sm.isConstexpr;
        if (sm.initializer) c.initializer = cloneExpr(sm.initializer, subst);
        cloned->staticDataMembers.push_back(std::move(c));
    }
    for (const auto& aliasName : src->typeAliasOrder) {
        auto it = src->typeAliases.find(aliasName);
        if (it == src->typeAliases.end()) continue;
        cloned->typeAliases[aliasName] = substituteType(it->second, subst);
        cloned->typeAliasOrder.push_back(aliasName);
    }
    for (auto& mt : src->memberTemplates) {
        auto clonedMt = std::make_shared<TemplateDecl>();
        clonedMt->location = mt->location;
        clonedMt->templateParams = mt->templateParams;   // 内层形参原样带过
        clonedMt->typeParams = mt->typeParams;
        clonedMt->isMemberTemplate = true;
        clonedMt->funcTemplate = cloneMethod(mt->funcTemplate, subst, "");
        cloned->memberTemplates.push_back(clonedMt);
    }
    for (auto& nc : src->nestedClasses) {
        cloned->nestedClasses.push_back(cloneNestedClass(nc, subst));
    }
    return cloned;
}

// 深拷贝表达式结构，实例的类型注解由语义分析重新计算。
// 参考 clang/Sema/TreeTransform.h: TransformExpr。
ExprPtr TemplateInstantiator::cloneExpr(
    ExprPtr expr, const TypeSubstitution& subst) {

    if (!expr) return nullptr;

    // ★ 分派判据（详见 docs/learn/29）：clone* 是【取值型】递归，每个分支都要返回新建
    //   节点（ExprPtr/StmtPtr），而 AstVisitor::visit 返回 void ⇒ 走 NodeKind 标签分派。
    //   static_cast 安全：节点 kind 由构造函数设定，恒等于自身类型。
    switch (expr->kind) {
        case NodeKind::IntLiteral: {
            auto& e = static_cast<IntLiteralExpr&>(*expr);
            // 形态（literalType）一并克隆 —— 它是类型信息，丢了会让 `4L` 在实例里
            // 退回 int（模板体里 `long x = 4L;` 的语义随之改变）。
            auto cloned = std::make_shared<IntLiteralExpr>(e.value, e.literalType);
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::CharLiteral: {
            auto& e = static_cast<CharLiteralExpr&>(*expr);
            auto cloned = std::make_shared<CharLiteralExpr>(e.value);
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::BoolLiteral: {
            auto& e = static_cast<BoolLiteralExpr&>(*expr);
            auto cloned = std::make_shared<BoolLiteralExpr>(e.value);
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::StringLiteral: {
            auto& e = static_cast<StringLiteralExpr&>(*expr);
            auto cloned = std::make_shared<StringLiteralExpr>(e.value);
            cloned->location = e.location;
            return cloned;
        }

        // 表达式中的 NTTP 名替换为带类型的字面量。
        // 例：template<int N> 的 return N 在 N=4 后得到整数 4；bool/char 保留相应种类。
        // 类型位置由 substituteType 处理。
        case NodeKind::Var: {
            auto& e = static_cast<VarExpr&>(*expr);
            auto it = subst.find(e.name);
            if (it != subst.end() && it->second.isValue()) {
                std::cout << std::format(
                    "    [subst] ★ NTTP value substitution: VarExpr '{}' → IntLiteral {}\n",
                    e.name, it->second.value);
                auto lit = std::make_shared<IntLiteralExpr>(it->second.value);
                lit->location = e.location;
                return lit;
            }
            // 未命中，或命中类型形参（那是类型名不是变量）—— 克隆名字本身。
            // ★ 但 explicitTemplateArgs 必须一并替换：declval<T>() 的 T 挂在
            //   VarExpr{declval, [T]} 上，只克隆名字会让 decltype 求值阶段看到裸
            //   declval()，T 凭空丢失 ⇒ 探测链断裂。
            auto cloned = std::make_shared<VarExpr>(e.name);
            cloned->location = e.location;
            for (const auto& ta : e.explicitTemplateArgs) {
                if (ta.isType() && ta.type) {
                    cloned->explicitTemplateArgs.push_back(
                        TemplateArg::ofType(substituteType(ta.type, subst)));
                } else {
                    cloned->explicitTemplateArgs.push_back(ta);   // 值实参原样带过
                }
            }
            return cloned;
        }

        case NodeKind::Binary: {
            auto& e = static_cast<BinaryExpr&>(*expr);
            auto cloned = std::make_shared<BinaryExpr>(
                e.op, cloneExpr(e.left, subst), cloneExpr(e.right, subst));
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::Unary: {
            auto& e = static_cast<UnaryExpr&>(*expr);
            auto cloned = std::make_shared<UnaryExpr>(
                e.op, cloneExpr(e.operand, subst));
            cloned->location = e.location;
            return cloned;
        }

        // 条件（三元）表达式：三支全递归克隆。
        // demo: "return a > b ? x : y;" ⇒ 结构不变、内部 T 已替换
        case NodeKind::Conditional: {
            auto& e = static_cast<ConditionalExpr&>(*expr);
            auto cloned = std::make_shared<ConditionalExpr>(
                cloneExpr(e.condition, subst),
                cloneExpr(e.thenExpr, subst),
                cloneExpr(e.elseExpr, subst));
            cloned->location = e.location;
            return cloned;
        }
        case NodeKind::Comma: {
            auto& e = static_cast<CommaExpr&>(*expr);
            auto cloned = std::make_shared<CommaExpr>(
                cloneExpr(e.left, subst),
                cloneExpr(e.right, subst));
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::Call: {
            auto& e = static_cast<CallExpr&>(*expr);
            std::vector<ExprPtr> clonedArgs;
            for (auto& arg : e.arguments) {
                clonedArgs.push_back(cloneExpr(arg, subst));
            }
            auto cloned = std::make_shared<CallExpr>(
                cloneExpr(e.callee, subst), std::move(clonedArgs));
            cloned->location = e.location;
            cloned->isCtorCall = e.isCtorCall;
            cloned->ctorClassType = e.ctorClassType;
            return cloned;
        }

        case NodeKind::Member: {
            auto& e = static_cast<MemberExpr&>(*expr);
            auto cloned = std::make_shared<MemberExpr>(
                cloneExpr(e.object, subst), e.memberName, e.isArrow);
            cloned->location = e.location;
            return cloned;
        }

        // 下标访问（容器封装的糖：模板体内 v[i] 也能实例化到具体类）
        case NodeKind::Index: {
            auto& e = static_cast<IndexExpr&>(*expr);
            auto cloned = std::make_shared<IndexExpr>(
                cloneExpr(e.object, subst), cloneExpr(e.index, subst));
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::This: {
            auto& e = static_cast<ThisExpr&>(*expr);
            auto cloned = std::make_shared<ThisExpr>();
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::New: {
            auto& e = static_cast<NewExpr&>(*expr);
            auto cloned = std::make_shared<NewExpr>(e.className);
            cloned->location = e.location;
            for (auto& arg : e.constructorArgs) {
                cloned->constructorArgs.push_back(cloneExpr(arg, subst));
            }
            return cloned;
        }

        // dynamic_cast：类名是字面名（非模板参数），原样克隆即可
        case NodeKind::DynamicCast: {
            auto& e = static_cast<DynamicCastExpr&>(*expr);
            auto cloned = std::make_shared<DynamicCastExpr>(
                e.targetClassName, cloneExpr(e.operand, subst));
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::NullptrLiteral: {
            auto& e = static_cast<NullptrLiteralExpr&>(*expr);
            auto cloned = std::make_shared<NullptrLiteralExpr>();
            cloned->location = e.location;
            return cloned;
        }

        // ── delete 表达式（ast.h 的 DeleteExpr 节点）──
        // 表达式位置的 `delete p`/`delete[] p`（如 `return delete p, 0;` 或作为子表达式）。
        // pointerExpr 递归克隆，isArray 原样带上。
        // ★ 必须深拷贝：蓝图 ExprPtr 被多实例共享时，任一实例改写都会污染其它实例。
        // demo: "delete data;" 配 {T := int} ⇒ "delete data;"（data 的 T*→int* 由 Sema 推出）
        case NodeKind::Delete: {
            auto& e = static_cast<DeleteExpr&>(*expr);
            auto cloned = std::make_shared<DeleteExpr>(
                cloneExpr(e.pointerExpr, subst), e.isArray);
            cloned->location = e.location;
            return cloned;
        }

        // 不会到达。留 default 是为了新增节点却忘了补 case 时由 -Wswitch 报出。
        default:
            return nullptr;
    }
}

// 递归深拷贝语句，保证各实例不互相修改，也不改写蓝图。
// 参考 clang/Sema/TreeTransform.h: TransformStmt。
// 例：Box<int> 与 Box<double> 的局部类型和函数体节点分别持有。
StmtPtr TemplateInstantiator::cloneStmt(
    StmtPtr stmt, const TypeSubstitution& subst) {

    if (!stmt) return nullptr;

    // ── 变量声明（本函数唯一直接碰类型的分支）──
    // declaredType 过 substituteType，initializer 过 cloneExpr（初始化式里可能嵌 new T(...)）。
    // demo: "T item = x;" 配 {T := int} ⇒ "int item = x;"
    // demo: "T* p = new T();" 配 {T := int} ⇒ "int* p = new int();"
    //       （declaredType 是 Pointer(T)，substituteType 会递归进 pointeeType）

    // 同 cloneExpr：取值型递归 ⇒ NodeKind 标签分派。
    switch (stmt->kind) {
        case NodeKind::VarDecl: {
            auto& s = static_cast<VarDeclStmt&>(*stmt);
            auto cloned = std::make_shared<VarDeclStmt>(
                s.name,
                substituteType(s.declaredType, subst),
                cloneExpr(s.initializer, subst));
            cloned->location = s.location;
            // 直接初始化实参表 / 选定符号 / constexpr / 平凡拷贝标记全部要跟身：
            // 模板体里的 `T x(args);` 若丢了 ctorArgs，实例化后会被当成无初始化声明，
            // 默默去调不存在的默认构造（链接期才炸）。
            for (auto& a : s.ctorArgs) cloned->ctorArgs.push_back(cloneExpr(a, subst));
            cloned->ctorSymbol = s.ctorSymbol;
            cloned->isConstexpr = s.isConstexpr;
            cloned->isTrivialCopy = s.isTrivialCopy;
            return cloned;
        }

        // ── 表达式语句 ──
        // 以分号结尾的裸表达式（f(x); / new T();）。本节点不含类型，替换全靠 cloneExpr。
        case NodeKind::ExprStmt: {
            auto& s = static_cast<ExprStmt&>(*stmt);
            auto cloned = std::make_shared<ExprStmt>(cloneExpr(s.expr, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── 赋值语句（C++ 里赋值是表达式 [expr.ass]，本项目简化为语句，ast.h 的 AssignStmt）──
        // target/value 两侧都过 cloneExpr；变量名不变，故通常无替换发生。
        // demo: "item = x;" ⇒ "item = x;"（结构深拷贝，内容不变）
        case NodeKind::Assign: {
            auto& s = static_cast<AssignStmt&>(*stmt);
            auto cloned = std::make_shared<AssignStmt>(
                cloneExpr(s.target, subst),
                cloneExpr(s.value, subst));
            cloned->location = s.location;
            cloned->isClassAssign = s.isClassAssign;
            cloned->assignClassType = s.assignClassType;
            return cloned;
        }

        // ── return 语句 ──
        // 返回值表达式过 cloneExpr；返回类型本身在 cloneMethod 里替换，不在这里。
        // demo: "return item;" ⇒ "return item;"
        case NodeKind::Return: {
            auto& s = static_cast<ReturnStmt&>(*stmt);
            auto cloned = std::make_shared<ReturnStmt>(
                cloneExpr(s.value, subst));
            cloned->location = s.location;
            return cloned;
        }

        case NodeKind::If: {
            auto& s = static_cast<IfStmt&>(*stmt);
            auto cloned = std::make_shared<IfStmt>(
                cloneExpr(s.condition, subst),
                cloneStmt(s.thenBranch, subst),
                s.elseBranch ? cloneStmt(s.elseBranch, subst) : nullptr);
            cloned->location = s.location;
            return cloned;
        }

        // ── while 语句 ──
        // condition 走 cloneExpr，body 走 cloneStmt 自递归。
        // demo: "while (i < n) { T t = i; i = i + 1; }" ⇒ "while (i < n) { int t = i; i = i + 1; }"
        case NodeKind::While: {
            auto& s = static_cast<WhileStmt&>(*stmt);
            auto cloned = std::make_shared<WhileStmt>(
                cloneExpr(s.condition, subst),
                cloneStmt(s.body, subst));
            cloned->location = s.location;
            return cloned;
        }

        // 循环的初始化、条件、增量与循环体都可能含依赖节点，须全部克隆。
        case NodeKind::For: {
            auto& s = static_cast<ForStmt&>(*stmt);
            auto cloned = std::make_shared<ForStmt>(
                cloneStmt(s.init, subst),
                cloneExpr(s.condition, subst),
                cloneStmt(s.increment, subst),
                cloneStmt(s.body, subst));
            cloned->location = s.location;
            return cloned;
        }
        case NodeKind::Break: {
            auto& s = static_cast<BreakStmt&>(*stmt);
            auto cloned = std::make_shared<BreakStmt>();
            cloned->location = s.location;
            return cloned;
        }
        case NodeKind::Continue: {
            auto& s = static_cast<ContinueStmt&>(*stmt);
            auto cloned = std::make_shared<ContinueStmt>();
            cloned->location = s.location;
            return cloned;
        }
        case NodeKind::Switch: {
            auto& s = static_cast<SwitchStmt&>(*stmt);
            auto cloned = std::make_shared<SwitchStmt>(
                cloneExpr(s.condition, subst),
                cloneStmt(s.body, subst));
            cloned->location = s.location;
            return cloned;
        }
        case NodeKind::Case: {
            auto& s = static_cast<CaseStmt&>(*stmt);
            auto cloned = std::make_shared<CaseStmt>(
                cloneExpr(s.value, subst),
                cloneStmt(s.body, subst));
            cloned->location = s.location;
            cloned->constValue = s.constValue;
            cloned->resolved   = s.resolved;
            return cloned;
        }
        case NodeKind::Default: {
            auto& s = static_cast<DefaultStmt&>(*stmt);
            auto cloned = std::make_shared<DefaultStmt>(cloneStmt(s.body, subst));
            cloned->location = s.location;
            return cloned;
        }
        case NodeKind::DoWhile: {
            auto& s = static_cast<DoWhileStmt&>(*stmt);
            auto cloned = std::make_shared<DoWhileStmt>(
                cloneStmt(s.body, subst),
                cloneExpr(s.condition, subst));
            cloned->location = s.location;
            return cloned;
        }

        case NodeKind::Block: {
            auto& s = static_cast<BlockStmt&>(*stmt);
            auto cloned = std::make_shared<BlockStmt>();
            cloned->location = s.location;
            for (auto& inner : s.statements) {
                cloned->statements.push_back(cloneStmt(inner, subst));
            }
            return cloned;
        }

        // delete 语句保留操作数及 delete[] 标记，语义分析在实例中重新完成。
        case NodeKind::Empty: {
            auto cloned = std::make_shared<EmptyStmt>();
            cloned->location = stmt->location;
            // ★ B37：裸类型名声明的名字槽要跟着克隆（漏了 ⇒ 实例化后的裸名声明
            //   变成无名空语句，可见性裁决失效 ⇒ 非法程序静默通过）。
            cloned->typeName = static_cast<EmptyStmt&>(*stmt).typeName;
            return cloned;
        }

        case NodeKind::DeleteStmt: {
            auto& s = static_cast<DeleteStmt&>(*stmt);
            auto cloned = std::make_shared<DeleteStmt>(
                cloneExpr(s.pointerExpr, subst), s.isArray);
            cloned->location = s.location;
            return cloned;
        }

        // 不会到达。留 default 是为了新增节点却忘了补 case 时由 -Wswitch 报出。
        default:
            return nullptr;
    }
}

} // namespace minicc
