// =============================================================================
// 测试：成员模板的形参表与自由模板同构（[temp.mem] × [temp.param]）
// =============================================================================
// 考察理论点：
//   · 成员函数模板的【形参表】与自由函数模板的形参表是**同一个文法、同一条语义规则**
//     （[temp.param]）—— 模板体住在命名空间里还是类体内，不改变形参表的规则。
//     本实现此前把成员侧的形参表**另写了一份简化版**，两处随批次演进而漂移：
//     成员侧缺【无名形参 / 模板模板形参 / 默认实参 / 默认实参约束 / 帧计数同步】，
//     其中"帧计数同步"（TemplateParamFrame::count）缺了最要命 ——
//     成员模板**自己**的形参在方法体里认不出（建成 Class 节点而非 TemplateParam 节点）
//     ⇒ 依赖限定名 `typename T::type` 直接报 "unknown type name 'T::type'"。
//   · 收口后两处走**同一份**实现（parser.cpp:parseTemplateParameterList）：
//     成员模板的形参能力与形态与自由模板完全一致。
//   · 边界（本批未做，且**自由路径同款** ⇒ 不是成员侧独有）：
//     模板模板形参在**函数模板**上的推导、默认模板实参在推导期的填充
//     —— 两者自由路径同样不支持（clang 支持），属独立的既有缺口。
//
// 预期行为：
//   · a.add(5)      ⇒ T := int，返回 base + 5
//   · a.get(b)      ⇒ T := Box，返回 Box::type（依赖限定名，★ 旧实现直接报错）
//   · 三个只声明不调用的成员模板（无名形参 / 模板模板形参 / 默认实参）
//     ⇒ 解析期【收得下】（clang 同样接受这些声明）
//   · 程序返回 22（与 clang 逐字一致）。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:template]   parsing parameter list <class T     ← ★ 与自由模板同款
//     [parse:template]   ★ type parameter registered: 'T'
//     [parse:type] base = T (template param, scope hit)      ← ★ 成员模板【自己】的形参
//     [parse:member] ★ 'add' 是成员模板（1 个模板形参，[temp.mem]）—— 调用点按实参推导实例化
//   Phase 3 [Sema]:
//     ↳ member template 'Acc::add' registered (1 template parameter(s))
//     [call] Acc::add → Acc_add_int (member template instantiated) → int
//   Phase 4：
//     ║ Blueprint: add <T>    Substitution: { T → int, }
//
// 可复现实验：
//   ./minicc tests/tmpl/test_tmpl_78_member_template_params.cpp -o /tmp/t78 && /tmp/t78; echo $?
//   clang++-18 -std=c++20 tests/tmpl/test_tmpl_78_member_template_params.cpp -o /tmp/t78c && /tmp/t78c; echo $?
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_tmpl_78_member_template_params) { minicc_gtest_body(); }
#endif

struct Box {
    int value;
    using type = int;      // ★ 供依赖限定名 `typename T::type` 兑现
};

struct Acc {
    int base;
    Acc(int b) : base(b) {}

    // ① 成员模板【自己】的类型形参 T —— 在方法体里必须认得出是形参
    template <class T>
    T add(T x) { return base + x; }

    // ② 依赖限定名（[temp.res]/5）：T 是依赖的限定者，不能拼成 "T::type" 字符串
    template <class T>
    typename T::type get(T x) { return x.value; }

    // ③ 无名类型形参（[temp.param]/3）：`template <class>` —— clang 接受
    //    （只声明不调用：无名形参无法从实参推导，与 clang 同款）
    template <class>
    int unnamed(int x) { return x * 2; }

    // ④ 模板模板形参（[temp.param]/4）—— clang 接受
    //    （只声明不调用：函数模板上的模板模板实参推导尚未实现，自由路径同款）
    template <template <class> class C, class T>
    T wrap(C<T> x) { return x.value; }

    // ⑤ 默认模板实参（[temp.param]/12）—— clang 接受
    //    （只声明不调用：推导期还没做"未推导形参用默认值顶上"，自由路径同款）
    template <class T, class U = int>
    T withDefault(T x) { return x; }
};

int main() {
    Acc a(10);
    Box b;
    b.value = 7;

    int r1 = a.add(5);        // T := int ⇒ 10 + 5 = 15
    int r2 = a.get(b);        // T := Box ⇒ Box::type = int ⇒ b.value = 7

    return r1 + r2;           // 22
}
