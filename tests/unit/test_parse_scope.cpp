// =============================================================================
// 单测：解析期活作用域表（ParseScopeTable）—— clang Scope 链的对应物
// =============================================================================
// 考察不变量（不钉具体容器/日志，命名规则随便改都不会误报）：
//   · 链：进出作用域，名字进它所在的那一层；
//   · 遮蔽：内层同名遮蔽外层（首命中即返回）；
//   · 可见性：命名空间里的类型在命名空间外【不可见】（裸名查不到）；
//   · 限定名：限定者逐段定位，末段只在限定者那层查（不外溢到全局）；
//   · 持久：离开命名空间/类之后，`N::T` 仍要查得到（clang 的 DeclContext 持久）；
//   · 透明：模板形参作用域对【名字登记】透明（模板名属外层，形参名属形参域）；
//   · 局部：函数/块里的局部类不持久（`f::Local` 不是合法限定名）。
// ★ 与 NameRegistry 的分水岭（判据级）：NameRegistry 对裸名"跨作用域 OR"，
//   本表"沿链首命中即返回" —— 同一段源码，前者说"可见"、后者说"不可见"。
// =============================================================================

#include "parse_scope.h"

#include <gtest/gtest.h>

using namespace minicc;

namespace {

// ── 链与遮蔽 ────────────────────────────────────────────────────────────────

TEST(ParseScopeChain, DeclaredNameIsFoundInItsOwnScope) {
    ParseScopeTable t;
    t.declare("S", ScopeBit::Type);
    EXPECT_EQ(t.classify("S"), ScopeBit::Type);
    EXPECT_TRUE(t.isDeclared("S"));
    EXPECT_EQ(t.classify("Unknown"), 0u);
    EXPECT_FALSE(t.isDeclared("Unknown"));
}

TEST(ParseScopeChain, InnerScopeShadowsOuter) {
    ParseScopeTable t;
    t.declare("x", ScopeBit::Value);            // 全局 x
    t.openScope(ScopeKind::Function, "f", ScopeBit::Value);
    t.declare("x", ScopeBit::Type);             // 函数里的 x（同名不同类）
    // ★ 内层遮蔽外层：查到的应是内层那一位（类型位），不是两层并集。
    EXPECT_EQ(t.classify("x"), ScopeBit::Type);
    t.leave();
    EXPECT_EQ(t.classify("x"), ScopeBit::Value); // 出了函数作用域，外层那份回来
}

TEST(ParseScopeChain, NamespaceMemberIsNotVisibleOutside) {
    ParseScopeTable t;
    t.openScope(ScopeKind::Namespace, "N", ScopeBit::Namespace);
    t.declare("S", ScopeBit::Type);
    EXPECT_EQ(t.classify("S"), ScopeBit::Type);  // 在 N 里面：可见
    t.leave();
    // ★ 出了 N 就不该查到 —— 这正是 NameRegistry 判错的地方（跨作用域 OR）。
    EXPECT_EQ(t.classify("S"), 0u);
}

TEST(ParseScopeChain, DepthAndPathGrowWithNesting) {
    ParseScopeTable t;
    EXPECT_EQ(t.depth(), 1u);
    EXPECT_EQ(t.currentPath(), "");
    t.openScope(ScopeKind::Namespace, "N", ScopeBit::Namespace);
    EXPECT_EQ(t.currentPath(), "N::");
    t.openScope(ScopeKind::Class, "C", ScopeBit::Type);
    EXPECT_EQ(t.currentPath(), "N::C::");
    t.openScope(ScopeKind::Block, "", 0u);
    EXPECT_EQ(t.currentPath(), "N::C::");   // 块不引入路径成分
    t.leave();
    t.leave();
    t.leave();
    EXPECT_EQ(t.currentPath(), "");
}

// ── 限定名 ──────────────────────────────────────────────────────────────────

TEST(ParseScopeQualified, LooksUpInsideTheQualifierOnly) {
    ParseScopeTable t;
    t.declare("B", ScopeBit::Type);                  // 全局 B
    t.openScope(ScopeKind::Namespace, "N", ScopeBit::Namespace);
    t.declare("S", ScopeBit::Type);                  // N::S
    t.leave();
    // N 里没有 B ⇒ 查不到，【不外溢】到全局 B（B38「由宽改严」）。
    EXPECT_EQ(t.classifyQualified("N::B"), 0u);
    EXPECT_EQ(t.classifyQualified("N::S"), ScopeBit::Type);
}

TEST(ParseScopeQualified, SurvivesLeavingTheNamespace) {
    ParseScopeTable t;
    t.openScope(ScopeKind::Namespace, "N", ScopeBit::Namespace);
    t.declare("S", ScopeBit::Type);
    t.leave();
    // ★ 离开命名空间之后 `N::S` 仍要查得到 —— clang 的 DeclContext 是持久的，
    //   Scope 节点才是临时的（clang/include/clang/Sema/Scope.h:Scope）。
    EXPECT_EQ(t.classifyQualified("N::S"), ScopeBit::Type);
    EXPECT_EQ(t.classify("S"), 0u);   // 但裸名依然不可见（可见性 ≠ 限定可达）
}

TEST(ParseScopeQualified, MultiSegmentQualifier) {
    ParseScopeTable t;
    t.openScope(ScopeKind::Namespace, "N", ScopeBit::Namespace);
    t.openScope(ScopeKind::Class, "C", ScopeBit::Type);
    t.declare("m", ScopeBit::Value);
    t.leave();
    t.leave();
    EXPECT_EQ(t.classifyQualified("N::C::m"), ScopeBit::Value);
    EXPECT_EQ(t.classifyQualified("N::C::nope"), 0u);
}

// ── 类作用域与模板形参作用域 ────────────────────────────────────────────────

TEST(ParseScopeClass, MembersLiveInTheClassScope) {
    ParseScopeTable t;
    t.openScope(ScopeKind::Class, "C", ScopeBit::Type);
    t.declare("f", ScopeBit::Value);
    EXPECT_EQ(t.classify("f"), ScopeBit::Value);
    t.leave();
    // 出了类作用域，成员不再"无限定可见"，但限定名仍可达。
    EXPECT_EQ(t.classify("f"), 0u);
    EXPECT_EQ(t.classifyQualified("C::f"), ScopeBit::Value);
}

TEST(ParseScopeTemplate, ParamScopeIsTransparentForTheTemplateName) {
    ParseScopeTable t;
    t.openScope(ScopeKind::TemplateParam, "", 0u);
    t.declare("T", ScopeBit::Type);              // 形参名属形参域
    t.openScope(ScopeKind::Class, "Box", ScopeBit::Type | ScopeBit::Template);
    t.declare("v", ScopeBit::Value);
    t.leave();   // 类作用域
    t.leave();   // 模板形参作用域
    // ★ 模板的名字属于【外层】（命名空间），不属于形参域 —— 这就是"透明"。
    EXPECT_EQ(t.classify("Box"), ScopeBit::Type | ScopeBit::Template);
    // 形参名随形参作用域一起消失。
    EXPECT_EQ(t.classify("T"), 0u);
    EXPECT_EQ(t.classifyQualified("Box::v"), ScopeBit::Value);
}

TEST(ParseScopeTemplate, ParamNameVisibleInsideTheTemplateBody) {
    ParseScopeTable t;
    t.openScope(ScopeKind::TemplateParam, "", 0u);
    t.declare("T", ScopeBit::Type);
    t.openScope(ScopeKind::Class, "Box", ScopeBit::Type | ScopeBit::Template);
    // 类体里能看见形参 T。
    EXPECT_EQ(t.classify("T"), ScopeBit::Type);
    t.leave();
    t.leave();
}

// ── 局部类不持久 ────────────────────────────────────────────────────────────

TEST(ParseScopeLocal, LocalClassIsNotPersisted) {
    ParseScopeTable t;
    t.openScope(ScopeKind::Function, "f", ScopeBit::Value);
    t.openScope(ScopeKind::Class, "Local", ScopeBit::Type);
    t.declare("x", ScopeBit::Value);
    t.leave();   // 类作用域
    t.leave();   // 函数作用域
    // ★ 局部类没有 `f::Local` 这种限定名（C++ 里不存在），故不持久化。
    EXPECT_EQ(t.classifyQualified("f::Local::x"), 0u);
    // 但全局作用域里没有 f，也没有 Local。
    EXPECT_EQ(t.classify("Local"), 0u);
}

// ── 位可以并存 ──────────────────────────────────────────────────────────────

TEST(ParseScopeBits, SameNameCanBeBothTypeAndTemplate) {
    ParseScopeTable t;
    t.declare("Box", ScopeBit::Type);
    t.declare("Box", ScopeBit::Template);
    EXPECT_EQ(t.classify("Box"), ScopeBit::Type | ScopeBit::Template);
}

// ─────────────────────────────────────────────────────────────────────────────
// 名字登记表的退场（Stage 3 批 2）：旧表（NameRegistry）是【前向扫 token 流】
// 猜出来的跨作用域并集，本表是 Sema 的活作用域链 —— 下面四条钉住两者的分水岭。
// ─────────────────────────────────────────────────────────────────────────────

TEST(ParseScopeRetirement, TemplateInNamespaceIsNotVisibleAsBareName) {
    // ★ 旧表说"可见"（裸名对任意作用域 OR 取并集）⇒ `B<int>` 在 N 外也被
    //   当成模板；真 C++ 里 N 里的模板在 N 外不可见（[basic.lookup.unqual]）。
    ParseScopeTable t;
    t.openScope(ScopeKind::Namespace, "N", ScopeBit::Namespace);
    t.declare("B", ScopeBit::Type | ScopeBit::Template);
    t.leave();
    EXPECT_EQ(t.classify("B"), 0u);              // 裸名不可见
    EXPECT_EQ(t.classifyQualified("N::B"), ScopeBit::Type | ScopeBit::Template);
}

TEST(ParseScopeRetirement, ClassScopeAliasIsNotVisibleOutsideTheClass) {
    // ★ 旧表把类体内的名字登记在【外层】作用域 ⇒ 全局裸用 `B` 被误判成
    //   "这里有类型 B"（basics_27 的现场）；活表里它只活在类作用域内。
    ParseScopeTable t;
    t.openScope(ScopeKind::Class, "A", ScopeBit::Type);
    t.declare("B", ScopeBit::Type);              // A 内的 using B = int;
    t.leave();
    EXPECT_EQ(t.classify("B"), 0u);              // 类外不可见
    EXPECT_EQ(t.classifyQualified("A::B"), ScopeBit::Type);
}

TEST(ParseScopeRetirement, ValueBitDistinguishesShadowingFromTypes) {
    // ★ 旧表没有 value 位 ⇒ 块作用域的变量遮蔽同名全局类型时，Parser 分不出
    //   `S;` 是声明还是表达式（basics_23 现场②）；活表有 value 位 ⇒ 分得出。
    ParseScopeTable t;
    t.declare("S", ScopeBit::Type);              // 全局 struct S
    EXPECT_EQ(t.classify("S") & ScopeBit::Value, 0u);   // 类型：不是值
    t.openScope(ScopeKind::Block, "", 0u);
    t.declare("S", ScopeBit::Value);             // 块作用域的 int S;
    EXPECT_EQ(t.classify("S") & ScopeBit::Value, ScopeBit::Value);
    EXPECT_EQ(t.classify("S") & ScopeBit::Type, 0u);    // ★ 遮蔽：类型位被压住
    t.leave();
    EXPECT_EQ(t.classify("S") & ScopeBit::Value, 0u);   // 出了块作用域，遮蔽消失
}

TEST(ParseScopeRetirement, NamespaceScopeStackIsRestoredOnPop) {
    // 旧表的花括号深度配对只能判"在不在某个作用域"，活表直接给出路径与深度。
    ParseScopeTable t;
    t.openScope(ScopeKind::Namespace, "N1", ScopeBit::Namespace);
    t.openScope(ScopeKind::Namespace, "N2", ScopeBit::Namespace);
    t.declare("S2", ScopeBit::Type);
    t.leave();
    t.declare("S1", ScopeBit::Type);             // N1 里的（不在 N2 里）
    t.leave();
    EXPECT_EQ(t.classifyQualified("N1::N2::S2"), ScopeBit::Type);
    EXPECT_EQ(t.classifyQualified("N1::S1"), ScopeBit::Type);
    EXPECT_EQ(t.classifyQualified("N1::N2::S1"), 0u);   // N2 里没有 S1
    EXPECT_EQ(t.classify("S1"), 0u);                     // 裸名都在 N1 之外不可见
}

}  // namespace
