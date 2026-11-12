#pragma once
// 模板实例化：独立克隆蓝图并按替换表改写类型与表达式。
// 参考 clang/Sema/SemaTemplateInstantiate.cpp、SemaTemplateInstantiateDecl.cpp、TreeTransform.h。
// 例：Box<T> 代入 int 得到独立的 Box_int；T&& 代入 int& 折叠为 int&。
// 缓存复用相同实例，符号编码与可读实例键分别保留用途。

#include "ast.h"
#include "type.h"
#include "sfinae.h"      // SubstitutionFailure（替换失败信号）+ SFINAE 协议
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <stdexcept>

namespace minicc {

// SubstitutionFailure 定义于 sfinae.h，推导与实例化共用失败协议。

// decltype 求值回调；由 Sema 提供名字查找与表达式类型规则。
class DecltypeEvaluator {
public:
    virtual ~DecltypeEvaluator() = default;

    // 求 decltype(expr) 的类型。
    //   expr  —— 已完成替换的操作数表达式
    //   paren —— 原文是否为 decltype((e)) 形态（[dcl.type.decltype] 两套规则）
    // 返回：求得的类型。
    // 抛：SubstitutionFailure —— 表达式在该上下文中不合法（SFINAE 软失败）。
    virtual TypePtr evaluateDecltype(const ExprPtr& expr, bool paren) = 0;
};

// 依赖成员类型查询回调，由 Sema 在限定者替换后执行。
// 例：typename T::type 在 T=int 时失败，在 T=S 且 S::type=int 时得到 int。
class MemberTypeResolver {
public:
    virtual ~MemberTypeResolver() = default;

    // 在 qualifier 所指的类里查成员类型别名 member。
    //   qualifier —— 已完成替换的限定者（如 int / Plain / Box_int）
    // 返回：查到的成员类型（已解糖）。
    // 抛：SubstitutionFailure —— 查不到，或限定者根本不是类（软失败）。
    virtual TypePtr resolveMemberType(const TypePtr& qualifier,
                                      const std::string& member) = 0;
};

// 别名模板展开回调，由 Sema 保有蓝图及作用域信息。
// 例：template<class T> using Ptr=T*; 的 Ptr<int> 展开为 int*。
class AliasTemplateResolver {
public:
    virtual ~AliasTemplateResolver() = default;

    // name 是否是已登记的别名模板。
    // 【为什么要单独一个查询】推导器每匹配一个模式位都要问一次"这名字要不要
    //   先解糖"；若靠 expandAliasTemplate 返回 nullptr 来判，就得以"试展开"
    //   的方式提问 —— 而试展开会走完整的替换流程，实参个数不符时还会发
    //   Sfinae::fail。判断句不该有副作用，故单列一个纯查询。
    virtual bool isAliasTemplate(const std::string& name) const = 0;

    // 把 `name<args>` 展开成别名所指向类型的替换结果。
    // 返回 nullptr ⇒ name 不是已登记的别名模板（调用方按普通类模板继续处理）。
    // 抛：SubstitutionFailure —— 实参个数不符、或替换途中失败（软失败）。
    virtual TypePtr expandAliasTemplate(const std::string& name,
                                        const std::vector<TemplateArg>& args) = 0;
};

// 符号修饰：用 Itanium 风格编码保留模板实参及函数签名。
class NameMangler {
public:
    // 类模板实例符号名：MyPtr<int> → _Z5MyPtrIiE。
    // 教学版省略完整 ABI 的名称替换表等压缩规则。
    static std::string mangleTemplateInstance(
        const std::string& templateName,
        const std::vector<TemplateArg>& args);

    // 无损实例键，不能直接用清洗后的汇编名称当缓存键。
    // 例：int 与 int*、const int* 与 int* const 必须各自区分。
    // 值实参同时编码类型和值，避免 K<4> 与 K<4L> 撞键。
    static std::string losslessArgumentsKey(
        const std::string& templateName,
        const std::vector<TemplateArg>& args,
        const std::vector<TemplateParamPtr>& params);

    // 函数模板实例还须编码返回及形参类型，以区分同模板实参的重载。
    // 例：f<int>(int) 与 f<int>(int*) 必须得到不同符号（Itanium ABI §5.1.8）。
    static std::string mangleFunctionTemplateInstance(
        const std::string& funcName,
        const std::vector<TemplateArg>& args,
        const TypePtr& returnType,
        const std::vector<Parameter>& params,
        const std::vector<std::string>& typeParams);

    // 对函数生成符号名
    // 例: foo(int, double) → _Z3foo id
    static std::string mangleFunction(
        const std::string& funcName,
        const std::string& className,
        const std::vector<Parameter>& params);

    // 生成 RTTI type_info 的符号名
    // 例: MyClass → _ZTI7MyClass
    static std::string mangleRTTI(const std::string& className);

    // 生成 vtable 的符号名
    // 例: MyClass → _ZTV7MyClass
    static std::string mangleVTable(const std::string& className);

private:
    // 将类型编码为 mangling 字符串
    // ★ typeParams 非空时，TypeKind::TemplateParam 编成 Itanium 的 <template-param>
    //   （T_ / T0_ / T1_…）；为空表时保持原行为（原样输出形参名）—— 故类模板
    //   路径（恒传 1 参）的符号逐字节不变。
    static std::string encodeType(TypePtr type,
        const std::vector<std::string>& typeParams = {});
};

// ─────────────────────────────────────────────────────────────────────────────
// TemplateInstantiator：模板实例化引擎
// ─────────────────────────────────────────────────────────────────────────────
class TemplateInstantiator {
public:
    // 模板参数名 → 实际实参的映射（公开：推导引擎构造后传入）
    // ★ value 是 TemplateArg（tagged）而非裸 TypePtr：模板形参可以是类型
    //   （typename T）也可以是非类型的值（int N，NTTP），两种形态必须能共存于
    //   同一张表。若 value 仍是 TypePtr，{N → 4} 这条映射根本无处安放——
    //   这正是 NTTP 在同一张表里"活不下来"的根因。
    // 对照 clang：Sema 的 MultiLevelTemplateArgumentList
    //（DeclTemplate.h），每层是 TemplateArgument 列表；单层对应本表。
    using TypeSubstitution = std::unordered_map<std::string, TemplateArg>;

    // 克隆类模板成员并应用替换表，结果按实例键缓存。
    // 例：Box<T>{T value;} 代入 int 得 value:int，代入 double 得 value:double。
    // 特化可传入已推导的绑定，避免把主模板实参误当特化形参。
    ClassDeclPtr instantiate(
        TemplateDeclPtr templateDecl,
        const std::vector<TemplateArg>& args,
        const TypeSubstitution* substOverride = nullptr);

    // 由推导结果实例化函数，克隆签名与函数体并缓存。
    // 例：T id(T x){return x;} 代入 int 得 int id(int)。

    // ownerClassName 非空 ⇒ 按【成员模板】实例化（[temp.mem]）：实例带隐式 this，
    // 符号用 `类名_方法名_实参后缀` 而非 Itanium 模板实例名。
    FuncDeclPtr instantiateFunction(
        TemplateDeclPtr templateDecl,
        const std::vector<TypePtr>& typeArgs,
        const std::string& ownerClassName = "");

    // 获取所有已实例化的类
    const std::vector<ClassDeclPtr>& getInstantiatedClasses() const {
        return m_instantiatedClasses;
    }

    // 获取所有已实例化的函数（S5）
    const std::vector<FuncDeclPtr>& getInstantiatedFunctions() const {
        return m_instantiatedFunctions;
    }

    // 类型替换（公开：推导引擎替换返回类型时复用同一套规则）。
    // 理论：[temp.subst] 的核心操作 —— 遍历类型树，模板参数叶节点换成实参类型，
    //   复合节点（指针/引用/const）递归重建；重建引用节点时执行引用折叠 [dcl.ref]。
    // demo: substituteType(T, {T := int}) → int │ (T&, {T := int}) → int&
    //       (T&&, {T := int&}) → int& && 折叠为 int&（万能引用落地）
    TypePtr substituteType(TypePtr type, const TypeSubstitution& subst);

    // decltype 求值回调（见 DecltypeEvaluator 注释）。
    // nullptr（默认，单元测试路径）→ 替换时不求值，Decltype 节点原样保留。
    // Sema 构造时会把自己挂上（setDecltypeEvaluator）。
    void setDecltypeEvaluator(DecltypeEvaluator* ev) { m_decltypeEval = ev; }
    DecltypeEvaluator* decltypeEvaluator() const { return m_decltypeEval; }

    // 成员类型查表回调（见 MemberTypeResolver 注释）。
    // nullptr（默认，单元测试路径）→ 依赖类型名原样保留，不查表。
    void setMemberTypeResolver(MemberTypeResolver* r) { m_memberResolver = r; }
    MemberTypeResolver* memberTypeResolver() const { return m_memberResolver; }

    // 别名模板展开回调（见 AliasTemplateResolver 注释）。
    // nullptr（默认，单元测试路径）→ 别名 id 原样保留，不解糖。
    void setAliasTemplateResolver(AliasTemplateResolver* r) { m_aliasResolver = r; }
    AliasTemplateResolver* aliasTemplateResolver() const { return m_aliasResolver; }

private:
    DecltypeEvaluator*        m_decltypeEval = nullptr;
    MemberTypeResolver*       m_memberResolver = nullptr;
    AliasTemplateResolver*    m_aliasResolver = nullptr;

    // 实例名占用表：清洗后的汇编符号前缀 → 无损键（"Box<int*>" 这种）。
    // 【为什么要有】实例名是人读的折中产物，清洗规则不是单射 —— 见
    //   template_instantiation.cpp Step 2a 的撞名守卫。这张表让"两条不同实例
    //   抢同一个符号前缀"从【汇编期 duplicate symbol】提前成【语义期可读报错】。
    std::unordered_map<std::string, std::string> m_instanceNameOwner;

    std::vector<ClassDeclPtr> m_instantiatedClasses;
    std::vector<FuncDeclPtr>  m_instantiatedFunctions;

    // ── AST 深拷贝与类型替换 ──
    // 对应 clang TreeTransform 的递归重建思想：逐节点克隆，类型位置套用 substituteType；
    // 克隆产物不携带旧的语义分析结果，交回语义分析器重新检查（两阶段查找的第二阶段）
    ExprPtr cloneExpr(ExprPtr expr, const TypeSubstitution& subst);
    StmtPtr cloneStmt(StmtPtr stmt, const TypeSubstitution& subst);
    FuncDeclPtr cloneMethod(FuncDeclPtr method, const TypeSubstitution& subst,
                            const std::string& newClassName);
    FieldInfo cloneField(const FieldInfo& field, const TypeSubstitution& subst);
    // 递归克隆嵌套类并替换外层形参；限定名仍交给 Sema 统一登记。
    ClassDeclPtr cloneNestedClass(const ClassDeclPtr& src,
                                  const TypeSubstitution& subst);
};

// 实例名清洗（见类内注释；定义在 src/template_instantiation.cpp）
std::string sanitizeSymbolChars(const std::string& raw);

} // namespace minicc
