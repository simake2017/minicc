#pragma once
// 名字与作用域的共享分类，供 Parser 登记、Sema 查询。
// 参考 clang/Sema/Scope.h: ScopeType；SemaDecl.cpp: ClassifyName。
// 用位集合表达同名实体的多种身份，避免两阶段各自定义分类。
// 例：struct Box 对应 Type；类模板 Box 对应 Type | Template。

namespace minicc {

// Namespace/Class 引入限定路径；Function/Block/TemplateParam 沿用父路径。
enum class ScopeKind : unsigned {
    Global = 0,
    Namespace,
    Class,
    Function,
    Block,
    TemplateParam,
};

// 名字种类位；一个名字可以同时占多个位置。
namespace ScopeBit {
inline constexpr unsigned Type      = 1u << 0;  // 类/结构体/枚举/typedef/using 别名
inline constexpr unsigned Namespace = 1u << 1;  // namespace 名
inline constexpr unsigned Template  = 1u << 2;  // 模板名（类/别名/函数模板、推导指引、
                                               //   成员模板、模板模板形参）
inline constexpr unsigned Value     = 1u << 3;  // 值名（变量/参数/函数/枚举项）
}  // namespace ScopeBit

}  // namespace minicc
