#pragma once
// 函数模板实参推导与偏特化模式匹配。
// 参考 clang/Sema/SemaTemplateDeduction.cpp: DeduceTemplateArguments。
// 以结构化合一构造形参到实参的替换表；调用推导与偏特化保留各自调整规则。
// 例：P=T*、A=int* 得 T=int；P=T&&、左值 int 实参得 T=int&。

#include "ast.h"
#include "type.h"
#include "template_instantiation.h"   // DecltypeEvaluator / SubstitutionFailure
#include <string>
#include <unordered_map>
#include <vector>

namespace minicc {

// 单次 P/A 配对的轨迹（教学日志用）
// demo：identity(42) 的推导产生一步 trace ——
//   pattern="T", argument="int", result="T := int", ok=true
// 对应运行日志行（P/A 两栏各宽 12 左对齐）：[deduction]   P=T          A=int        ⇒ T := int
struct DeductionStep {
    std::string pattern;   // P：参数类型模式
    std::string argument;  // A：实参类型
    std::string result;    // "T := int" / "一致 ✓" / 失败原因
    bool        ok = true;
};

// 一次完整推导的产出（重载决议 S6 中每试一个候选模板就构造一份）
// demo：identity(42) → success=true, deducedArgs=[int]
//       make(1, 2.0) → success=false，failureReason 记录 T 的冲突绑定
struct DeductionResult {
    bool success = false;
    std::vector<TypePtr> deducedArgs;  // 按模板参数表顺序（显式给定 + 推导补全）
    std::string failureReason;
    std::vector<DeductionStep> trace;
};

// 替换表：模板形参名 → 已推出的类型（合一算法的「解」）
using Subst = std::unordered_map<std::string, TypePtr>;

class TemplateDeducer {
public:
    // 一次函数模板调用的完整推导：S3 显式前缀 → S2 逐对合一 → S4 收尾检查。
    // 理论：形参类型是含"变量"(T) 的模式 P，与实参 A 逐对合一积累替换表，
    //   同一变量多次绑定必须一致；产出交 S5 实例化（[temp.deduct.call]）。
    // demo: identity(42) → T := int ✓，deducedArgs=[int] │ twice(3) → 两次绑定一致 ✓
    //       make(1,2.0) → 先 T := int 再 T := double ⇒ 冲突 ✗，整体失败
    // 参数：tmpl 蓝图（funcTemplate 非空）│ argTypes 各实参类型 │ argIsLValue
    //   各实参是否左值（T& 绑定检查、万能引用折叠要用）│ explicitArgs 显式模板
    //   实参（S3，占模板参数表前缀，可为空）
    DeductionResult deduce(const TemplateDeclPtr& tmpl,
                           const std::vector<TypePtr>& argTypes,
                           const std::vector<bool>& argIsLValue,
                           const std::vector<TypePtr>& explicitArgs = {});

    // 偏特化匹配（[temp.class.spec.match]）：用使用点实参 args 逐位推导偏特化模式
    //   pattern 中的模板参数 —— 与函数模板推导是同一个合一算法（复用 deducePair），
    //   如 Box<T*, T> 对 Box<double*, double> ⇒ {T := double}。
    // demo: pattern=[T*, T]、args=[double*, double] → 成功，subst={T→double}
    //       pattern=[T*, T]、args=[int, int]      → 失败（指针结构失配）
    // 返回：全部位匹配成功 → true；任一位失败 → false 并填 failReason。
    // 注：不做 [temp.class.order] 偏序裁决（多偏特化同时匹配时取先成功者）。
    bool matchPattern(const std::vector<TemplateArg>& pattern,
                      const std::vector<TemplateArg>& args,
                      const std::vector<std::string>& paramNames,
                      std::unordered_map<std::string, TypePtr>& subst,
                      std::string& failReason);

    // decltype / void_t 的求值回调（见 DecltypeEvaluator）。
    // nullptr（默认，单元测试路径）→ 模式中的 void_t 不求值。
    void setDecltypeEvaluator(DecltypeEvaluator* ev) { m_decltypeEval = ev; }
    // 成员类型查表回调：reducePattern 里临时建的 instantiator 要用它
    // （void_t<typename T::type> 这类模式位要靠它在替换阶段查表）。
    void setMemberTypeResolver(MemberTypeResolver* r) { m_memberResolver = r; }
    // 别名模板展开回调：同样给 reducePattern 里那个临时 instantiator 用
    // （偏特化模式位写成别名模板 id 时，靠它在替换阶段解糖）。
    void setAliasTemplateResolver(AliasTemplateResolver* r) { m_aliasResolver = r; }

    // 逐对结构合一；调用推导允许值传递调整和转发引用，偏特化按结构匹配。
    // 例：T* 对 int* 得 T=int；T&& 对左值 int 得 T=int&；T 对 int 与 double 冲突。
    // argIsLValue 用于引用绑定，structuralMatch 保留模板实参中的引用层。
    bool deducePair(const TypePtr& P, TypePtr A, bool argIsLValue,
                    const std::vector<std::string>& paramNames,
                    Subst& subst, DeductionResult& out,
                    bool structuralMatch = false);

private:
    // 模式位里的别名模板 id 解糖：`Vec<T>` → `MyPtr<T>`（[temp.alias]/1）。
    // 名字不是别名模板（或没挂解析器）时原样返回。见实现处的详细说明。
    TypePtr desugarAlias(const TypePtr& P,
                         const std::unordered_map<std::string, TypePtr>& subst);

    DecltypeEvaluator* m_decltypeEval = nullptr;
    MemberTypeResolver* m_memberResolver = nullptr;
    AliasTemplateResolver* m_aliasResolver = nullptr;

    // 依赖模式先代入已有绑定再求值，失败按 SFINAE 处理。
    // 例：void_t<typename T::type> 在已知 T 后才能决定是否匹配。
    TypePtr reducePattern(const TypePtr& P,
                          const std::unordered_map<std::string, TypePtr>& subst);

    // 绑定模板参数（一致性检查 = 替换合成）：把 paramName := type 写入替换表；
    //   若该名字已有绑定，则要求新旧类型相等，否则整次推导失败。
    // demo: max(x:int, y:int) → 第一次 T := int 入表；第二次 int == int，
    //       记"T := int（一致 ✓）"；若第二次是 double 则冲突 ✗。
    bool bind(const std::string& paramName, const TypePtr& type,
              Subst& subst, DeductionResult& out,
              const std::string& patternStr, const std::string& argStr);

    // 类型是否是"模板参数"（TemplateParam 或被 parseType 解析成 Class 的参数名）
    // 合一算法里"识别变量位置"的谓词：只有模板参数才是可绑定的未知量，
    // 其余类型都是待比较的常量结构。
    bool isTemplateParamName(const TypePtr& t,
                             const std::vector<std::string>& paramNames) const;
    // 取出类型节点对应的模板参数名（如 "T"），作为替换表 Subst 的键
    std::string paramKey(const TypePtr& t) const;
};

} // namespace minicc
