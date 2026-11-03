#pragma once
// =============================================================================
// 名字作用域契约（parse ⇄ sema 共享）
// =============================================================================
// ① 定位：解析期"名字属于哪一层作用域 / 它是哪一类东西"的两枚编码 ——
//    Parser 侧负责登记，Sema 侧负责按同一套编码回答"它在此处是什么"。
// ② 源码锚点：clang/include/clang/Sema/Scope.h:Scope::ScopeType（作用域种类）
//    + clang/lib/Sema/SemaDecl.cpp:912:Sema::ClassifyName（名字分类轴：
//    type / namespace / template / value 四类）。
// ③ 动机：这两枚编码是【跨域契约】而非实现细节 —— Parser 说"这是个类型名"、
//    Sema 答"它是类型"，两边必须用同一套位；各写一份必然漂（判据单点铁律）。
//    放 include/ 是因为它既不属于 parse 域也不属于 sema 域，是两者之间的契约。
// ④ demo: class Box                  ⇒ declare("Box", ScopeBit::Type)
//          template<class T> struct Box ⇒ declare("Box", Type | Template)
//          namespace N                 ⇒ declare("N",  ScopeBit::Namespace)
//        ★ 一枚名字可以同时是多种：类模板名既是类型名也是模板名 —— 位并集表达。
// =============================================================================

namespace minicc {

// ── 作用域种类（clang Scope.h:Scope::ScopeType 的子集）─────────────────────
// Global/Namespace/Class 三层引入【路径成分】（名字影响限定名解析）；
// Function/Block/TemplateParam 三层不引入（路径沿用父层）。
enum class ScopeKind : unsigned {
    Global = 0,
    Namespace,
    Class,
    Function,
    Block,
    TemplateParam,
};

// ── 名字种类位（clang Sema::ClassifyName 的分类轴）─────────────────────────
namespace ScopeBit {
inline constexpr unsigned Type      = 1u << 0;  // 类/结构体/枚举/typedef/using 别名
inline constexpr unsigned Namespace = 1u << 1;  // namespace 名
inline constexpr unsigned Template  = 1u << 2;  // 模板名（类/别名/函数模板、推导指引、
                                               //   成员模板、模板模板形参）
inline constexpr unsigned Value     = 1u << 3;  // 值名（变量/参数/函数/枚举项）
}  // namespace ScopeBit

}  // namespace minicc
