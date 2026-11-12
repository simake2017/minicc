#pragma once
// 抽象语法树是 Parser、Sema、模板实例化与 CodeGen 共享的结构。
// Sema 回填类型、符号和布局；实例化克隆并替换依赖节点，CodeGen 消费终态。
// `int a = 1 + 2;` 对应变量声明及 Add(IntLiteral(1), IntLiteral(2))。
// 参考：clang/include/clang/AST/Decl.h、Stmt.h、Expr.h；本实现统一使用 ASTNode 基类。

#include "ast_visitor.h"   // 节点 accept 的实现要访问 AstVisitor，故在此引入。
                           // 依赖是单向的：ast_visitor.h 只前置声明节点类型，
                           // 不回头 include 本文件（理由见那边的文件头注释）。
#include "type.h"
#include "token.h"
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>

namespace minicc {

// ─── 前向声明与智能指针别名 ──────────────────────────────────────────────────
struct ASTNode;
struct Expression;
struct Statement;
struct Declaration;

using ASTNodePtr     = std::shared_ptr<ASTNode>;
using ExprPtr        = std::shared_ptr<Expression>;
using StmtPtr        = std::shared_ptr<Statement>;
using DeclPtr        = std::shared_ptr<Declaration>;

// 所有节点共用 ASTNode；NodeKind 支持显式分类，accept 支持访问者分派。
enum class NodeKind : uint8_t {
    // 表达式
    IntLiteral, CharLiteral, BoolLiteral, StringLiteral, NullptrLiteral,
    Var, Binary, Unary, Call, Member, New, This, Delete, DynamicCast, Index,
    Conditional, Comma,
    // 语句
    ExprStmt, VarDecl, Return, If, While, Block, Assign, DeleteStmt, Empty,
    For, Break, Continue, Switch, Case, Default, DoWhile,
    // 声明
    Function, Class, Template, GlobalVar, Enum, Namespace, TypeAlias,
    Constructor, Destructor, DeductionGuide,
};

struct ASTNode {
    NodeKind kind;
    SourceLocation location;
    virtual ~ASTNode() = default;

    // 访问者挂钩：接受一个 AstVisitor，由具体节点在重写里回调对应的 visit 重载。
    // 纯虚 ⇒ 新增节点类型若忘了实现 accept，编译期即报错（不会静默漏掉）。
    // 各节点类里的 accept 一律写成类内 inline 定义（见下方各 struct），
    // 这样不会成为"键函数"，vtable 才能在每个用到的 TU 里弱符号发射。
    virtual void accept(AstVisitor& v) = 0;

protected:
    explicit ASTNode(NodeKind k) : kind(k) {}
};

// 表达式保存 resolvedType，可求值并作为子表达式；语句与声明不使用此结果槽。
struct Expression : ASTNode {
    TypePtr resolvedType; // 语义分析阶段填充

    explicit Expression(NodeKind k) : ASTNode(k) {}
};

// [expr.prim.literal]：字面量节点保存词法值，类型由 Sema 决定。
// 参考：clang 的 IntegerLiteral、CXXBoolLiteralExpr、StringLiteral。

struct IntLiteralExpr : Expression {
    int64_t value;
    // ── 字面量的【形态】（[lex.icon]）──
    // 4 是 int、4L 是 long、4u 是 unsigned int —— 后缀决定类型，值一样形态不同。
    //  为空 = 未标注 ⇒ 按 int 处理。既有构造点（如 NTTP 替换期造的 IntLiteralExpr）
    // 全部走单参构造，行为与日志一字不变。
    TypePtr literalType;
    explicit IntLiteralExpr(int64_t v) : Expression(NodeKind::IntLiteral), value(v) {}
    IntLiteralExpr(int64_t v, TypePtr t)
        : Expression(NodeKind::IntLiteral), value(v), literalType(std::move(t)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 字符字面量 'a'（[lex.ccon]）。 单独一个节点而不是复用 IntLiteralExpr：
// `'a'` 的【类型是 char】、占 1 字节，而 `97` 的类型是 int、占 4 字节 ——
// 若共用节点，`char c = 'a';` 与 `int c = 97;` 在下游就分不开了
// （codegen 按宽度分派、重载/推导按类型匹配，两处都会错）。
// 对照 clang：CharacterLiteral 与 IntegerLiteral 是两个独立 Stmt 类，同此理。
// 值在构造时就定好（词法阶段已翻译转义）：'a' ⇒ 97。
struct CharLiteralExpr : Expression {
    int64_t value;          // 字符的码点值（本项目按 ASCII/单字节）
    explicit CharLiteralExpr(int64_t v) : Expression(NodeKind::CharLiteral), value(v) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct BoolLiteralExpr : Expression {
    bool value;
    explicit BoolLiteralExpr(bool v) : Expression(NodeKind::BoolLiteral), value(v) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct StringLiteralExpr : Expression {
    std::string value;
    explicit StringLiteralExpr(std::string v)
        : Expression(NodeKind::StringLiteral), value(std::move(v)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct NullptrLiteralExpr : Expression {
    NullptrLiteralExpr() : Expression(NodeKind::NullptrLiteral) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 变量引用 ─────────────────────────────────────────────────────────────────
// 对应 [expr.prim.id]（未限定名查找）；clang: DeclRefExpr。
// demo：return a;   → ReturnStmt[ VarExpr{name="a"} ]
// 语义阶段把 VarExpr 解析到符号表中的具体声明，并填 resolvedType。
struct VarExpr : Expression {
    std::string name;
    // 显式模板实参（S3）：foo<int>(x) 中的 <int>。Parser 在标识符后识别 template-id 时
    // 填充；语义阶段据此做"显式 + 推导"混合。挂在 VarExpr 上是因为 template-id 出现在
    // 调用的 '(' 之前；用 TemplateArg（tagged）与 Type::templateArgs 保持一致，使
    // foo<4>(x) 也能被解析出来（否则语法阶段就崩成 "Expected type name"），再由语义
    // 阶段给出"函数模板暂不支持非类型实参"的明确报错。
    std::vector<TemplateArg> explicitTemplateArgs;
    // 限定字段位置由 Sema 回填；如派生类隐藏 v 时，`S::v` 仍指向基类字段。
    // offset < 0 表示未回填，此时使用布局名字查询。
    int      resolvedFieldOffset = -1;
    uint32_t resolvedFieldSize   = 0;
    explicit VarExpr(std::string n) : Expression(NodeKind::Var), name(std::move(n)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 二元表达式 ───────────────────────────────────────────────────────────────
// 对应 [expr.add]/[expr.mul]/[expr.rel]/[expr.eq]/[expr.log.and] 等；
// clang: BinaryOperator。BinaryOp 把运算符抽象掉，树形与具体运算符无关。
enum class BinaryOp : uint8_t {
    Add, Sub, Mul, Div, Mod,
    Eq, Neq, Lt, Gt, Le, Ge,
    And, Or,
};

// demo：int c = a + b;
//   VarDeclStmt{ name=c, initializer = BinaryExpr{ op=Add,
//        left = VarExpr{a}, right = VarExpr{b} } }
//   ASCII:     BinaryExpr(+)
//              ├─ left  → VarExpr(a)
//              └─ right → VarExpr(b)
struct BinaryExpr : Expression {
    BinaryOp op;
    ExprPtr  left;
    ExprPtr  right;
    BinaryExpr(BinaryOp o, ExprPtr l, ExprPtr r)
        : Expression(NodeKind::Binary), op(o),
          left(std::move(l)), right(std::move(r)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// [expr.cond]：`cond ? a : b` 保存条件与两分支，只求值被选中的分支。
// 参考：clang::ConditionalOperator。
struct ConditionalExpr : Expression {
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
    ConditionalExpr(ExprPtr cond, ExprPtr t, ExprPtr e)
        : Expression(NodeKind::Conditional), condition(std::move(cond)),
          thenExpr(std::move(t)), elseExpr(std::move(e)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 一元表达式 ───────────────────────────────────────────────────────────────
// 对应 [expr.unary.op]；clang: UnaryOperator。本项目仅实现 Neg(-) 与 Not(!)。
enum class UnaryOp : uint8_t {
    Neg,    // -x
    Not,    // !x
    Addr,   // &x —— 取地址 [expr.unary.op]/3
    Deref,  // *x —— 解引用 [expr.unary.op]/1
//  一元 '*' 与二元的 '*' 同形，靠【位置】区分：有左操作数才是乘法。
// 与 '&'（取地址 vs 位与/引用）完全同构的判据，见 parseUnaryExpr。
};

// demo：return -x;  → ReturnStmt[ UnaryExpr{ op=Neg, operand=VarExpr{x} } ]
struct UnaryExpr : Expression {
    UnaryOp op;
    ExprPtr operand;
    UnaryExpr(UnaryOp o, ExprPtr e)
        : Expression(NodeKind::Unary), op(o), operand(std::move(e)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// [expr.call]：调用保存 callee 与显式实参，Sema 回填目标符号及成员调用信息。
// `f(x)` 使用 VarExpr；`obj.f(x)` 使用 MemberExpr。参考：clang::CallExpr。
struct CallExpr : Expression {
    ExprPtr              callee;     // 被调用的函数（可以是 VarExpr 或 MemberExpr）
    std::vector<ExprPtr> arguments;  // 实参列表
    CallExpr(ExprPtr c, std::vector<ExprPtr> args)
        : Expression(NodeKind::Call), callee(std::move(c)),
          arguments(std::move(args)) {}

    // `Box<int>(7)` 解析为 CallExpr；Sema 识别类构造并回填实例类型。
    // CodeGen 据此物化临时对象，避免将类模板名字当普通函数符号。
    bool   isCtorCall = false;
    TypePtr ctorClassType;

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 成员访问表达式 ───────────────────────────────────────────────────────────
// 对应 [expr.ref]（类成员访问）；clang: MemberExpr。
// demo：obj.field   → MemberExpr{ object=VarExpr{obj}, memberName="field", isArrow=false }
//       ptr->field  → MemberExpr{ object=VarExpr{ptr}, memberName="field", isArrow=true }
// isArrow=true 时语义分析先对指针解引用（取其 pointee 的类布局再查 memberName）。
struct MemberExpr : Expression {
    ExprPtr     object;      // obj.field 中的 obj
    std::string memberName;  // 字段名或方法名
    bool        isArrow;     // true 表示 ->, false 表示 .
    bool        isMethodCall = false; // Sema 标记成员调用及类型限定访问；`Cls<Args>::member` 不依赖对象实例。
    bool        isTypeAccess = false;

    // 成员模板实例化符号由 Sema 选定并回填，CodeGen 不再依据实参自行定名。
    std::string resolvedCalleeSymbol;

    // [expr.ref]/[expr.prim.id.qual]：`obj.Q::m` 使用 Sema 回填的字段位置。
    // 按裸名重查可能误命中派生类隐藏成员，CodeGen 应优先采用非负偏移。
    int      resolvedFieldOffset = -1;
    uint32_t resolvedFieldSize   = 0;

    // [class.static.data]：静态成员不占对象偏移，使用 Sema 回填的全局符号访问。
    std::string resolvedStaticSymbol;

    // 类内初始化的 static const 成员可折叠为字面量；无存储时不能按字段地址读取。
    bool    staticConstFolded = false;
    int64_t staticConstValue  = 0;

    MemberExpr(ExprPtr obj, std::string member, bool arrow)
        : Expression(NodeKind::Member), object(std::move(obj)),
          memberName(std::move(member)), isArrow(arrow) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// [expr.sub]：下标节点同时表示原生数组访问与容器 at/set 调用。
// Sema 区分两者，CodeGen 分别按元素偏移或成员调用生成代码。
struct IndexExpr : Expression {
    ExprPtr object;  // 被下标的容器对象（类类型）
    ExprPtr index;   // 下标表达式（按约定方法签名校验）
// Sema 回填的约定方法【符号】（at = 读，set = 写）。
//  CodeGen 不能硬拼 `类名_at`：成员方法名由 Sema 追加了"参数个数"后缀
// （IntVec_at_1 / IntVec_set_2，见 semantic_analyzer.cpp 的 mangledName 规则），
// 硬拼会拼出一个谁也没定义过的符号 ⇒ 链接期 undefined reference。
// 空串 = 类里没有对应约定方法 ⇒ CodeGen 退回硬拼（把报错留给链接期）。
    std::string atSymbol;
    std::string setSymbol;
    IndexExpr(ExprPtr obj, ExprPtr idx)
        : Expression(NodeKind::Index),
          object(std::move(obj)), index(std::move(idx)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── new 表达式 ───────────────────────────────────────────────────────────────
// 对应 [expr.new]；clang: CXXNewExpr。
// demo：new Point()      → NewExpr{ className="Point", constructorArgs=[] }
//       new Point(a, b)  → NewExpr{ className="Point", constructorArgs=[a, b] }
// 结果类型是 Point*（指针）；CodeGen 生成 operator new 调用 + 构造函数调用。
struct NewExpr : Expression {
    std::string            className;
    std::vector<ExprPtr>   constructorArgs;
    // P3 —— new Box<int>() / new Buf<4>() 的模板实参（Parser 填写
    // 语义阶段实例化后把 className 改写为实例名，CodeGen 只看改写后的名字）。
    // 用 TemplateArg（tagged）：同时容纳类型实参与 NTTP 值实参。
    std::vector<TemplateArg> templateArgs;
    // new 的构造符号由 Sema 选定并回填，使分配后的调用与构造定义共享同一名字。
    std::string            ctorSymbol;
    explicit NewExpr(std::string cls)
        : Expression(NodeKind::New), className(std::move(cls)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// [expr.dynamic.cast]：保存源表达式与目标类型，Sema 检查后由 CodeGen 生成运行时转换。
// 参考：clang::CXXDynamicCastExpr。
struct DynamicCastExpr : Expression {
    std::string targetClassName;  // dynamic_cast<这里>中的类名
    ExprPtr     operand;          // 被转换的表达式
    DynamicCastExpr(std::string cls, ExprPtr op)
        : Expression(NodeKind::DynamicCast),
          targetClassName(std::move(cls)), operand(std::move(op)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── this 表达式 ──────────────────────────────────────────────────────────────
// 对应 [expr.prim.this]；clang: CXXThisExpr。
// demo：return this;   → ReturnStmt[ ThisExpr ]
// 仅出现在成员函数体内；类型是"指向所属类的指针"（[class.this]）。
struct ThisExpr : Expression {
    ThisExpr() : Expression(NodeKind::This) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── delete 表达式 ────────────────────────────────────────────────────────────
// 对应 [expr.delete]；clang: CXXDeleteExpr。
// demo：delete ptr;  → DeleteExpr{ pointerExpr=VarExpr{ptr}, isArray=false }
// 先调用析构函数（如果是类类型指针），再调用 free 释放堆内存。
struct DeleteExpr : Expression {
    ExprPtr pointerExpr;
    bool    isArray = false; // 是否为 delete[]
    explicit DeleteExpr(ExprPtr p, bool isArr = false)
        : Expression(NodeKind::Delete), pointerExpr(std::move(p)), isArray(isArr) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Statement：语句基类
// ─────────────────────────────────────────────────────────────────────────────
// 无类型、只描述执行动作（判据见上方 Expression 的三大族）。对应 clang::Stmt。
// demo: return 0; ⇒ ReturnStmt；if (f) {...} ⇒ IfStmt；x = 5; ⇒ AssignStmt
// （ C++ 里赋值本身是表达式，本项目简化成语句。）
struct Statement : ASTNode {
    explicit Statement(NodeKind k) : ASTNode(k) {}
};

// [stmt]：空语句也承载无声明符声明候选，如 `S;`。
// 保留 typeName 供 Sema 检查可见性；未知类型不能在解析期悄悄变成合法空语句。
// 参考：clang::NullStmt。
struct EmptyStmt : Statement {
    explicit EmptyStmt() : Statement(NodeKind::Empty) {}
    explicit EmptyStmt(std::string n) : Statement(NodeKind::Empty),
                                       typeName(std::move(n)) {}

    std::string typeName;   // ③ 裸类型名声明的名字；空串 = 普通空语句

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 表达式语句 ───────────────────────────────────────────────────────────────
// 对应 [stmt.expr]。clang 中表达式本身即可充当语句，无独立 ExprStmt 节点；
// 教学实现单独建模，便于区分"表达式"与"以分号结尾的语句"。
// demo：foo();   → ExprStmt[ CallExpr{callee=VarExpr{foo}, arguments=[]} ]
struct ExprStmt : Statement {
    ExprPtr expr;
    explicit ExprStmt(ExprPtr e) : Statement(NodeKind::ExprStmt), expr(std::move(e)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 变量声明语句（含 auto） ──────────────────────────────────────────────────
// 对应 [dcl.stmt]/[dcl.init]；clang: VarDecl + DeclStmt。
// demo：int x = 5;      → VarDeclStmt{ name=x, declaredType=int, initializer=IntLiteral{5} }
//       auto y = foo(); → VarDeclStmt{ name=y, declaredType=<auto 占位>, initializer=CallExpr }
// auto 的 declaredType 是 Type::makeAuto()，语义阶段用 initializer 的类型回填
// （[dcl.spec.auto]）。
struct VarDeclStmt : Statement {
    std::string name;
    TypePtr     declaredType;    // 声明的类型（可能是 auto 占位符）
    ExprPtr     initializer;     // 初始化可为空；直接初始化 `T x(args);` 保存构造实参，由 Sema 选定构造符号。
    std::vector<ExprPtr> ctorArgs;

    // 语义阶段选定并【回填】的构造函数符号（成员 mangledName）。
    //  不能让 CodeGen 自己拼：mangling 是有状态的（同名多参追加参数个数后缀），"该调
    // 哪个重载"只有刚做完推导/决议的 Sema 知道；硬拼 `Name_Name` 会拼出不存在的符号。
    // 空 ⇒ 零参构造，CodeGen 回退 `Name_Name`。
    std::string ctorSymbol;

    // ── constexpr 局部变量（[dcl.constexpr]）──
    // 语义同 GlobalVarDecl::isConstexpr：初始化式必须可常量求值。
    bool isConstexpr = false;

    // ── 平凡拷贝初始化：`Box<int> b = a;`（同类左值，无用户拷贝构造函数）──
    // 类实例按内存逐字节复制（CodeGen 发 memcpy），不调用（不存在的）拷贝构造函数。
    // 由 Sema 在 init 类型与声明类型同类时回填。
    bool isTrivialCopy = false;

    // ── 按值返回类的调用初始化：`S x = f();`（sret + 拷贝省略）──
    // f 按值返回类对象 ⇒ 调用点需要一个【返回缓冲】（sret 约定：隐藏指针作第 0 参数）。
    //  缓冲【就是 x 的槽】⇒ 调用直接把返回值写进 x，无需再拷贝一次 ——
    // 这就是 [class.copy.elision]/8 的拷贝省略（clang 默认也这么做）。
    //  判据由 Sema 落在这里（单一来源）：CodeGen 只照办，
    // 不再自己判断"这个调用返回不返回类"（两处各判一次必漂，learn/49）。
    bool initIsSretCall = false;

    VarDeclStmt(std::string n, TypePtr t, ExprPtr init)
        : Statement(NodeKind::VarDecl), name(std::move(n)),
          declaredType(std::move(t)), initializer(std::move(init)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 赋值语句 ─────────────────────────────────────────────────────────────────
// 对应 [expr.ass]；clang 用 BinaryOperator(BO_Assign) 表示，教学实现单独建 AssignStmt。
// demo：x = 5;      → AssignStmt{ target=VarExpr{x}, value=IntLiteral{5} }
//       obj.f = 1;  → AssignStmt{ target=MemberExpr{obj.f}, value=IntLiteral{1} }
struct AssignStmt : Statement {
    ExprPtr target;   // 赋值目标（变量或成员访问）
    ExprPtr value;    // 右值
    AssignStmt(ExprPtr t, ExprPtr v)
        : Statement(NodeKind::Assign), target(std::move(t)), value(std::move(v)) {}

    // 类实例赋值按对象布局逐字节复制，由 Sema 的 isClassAssign 标记决定。
    // 右值可为同类变量、成员子对象或构造临时对象；临时对象在复制后析构。
    bool    isClassAssign = false;
    TypePtr assignClassType;

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── return 语句 ──────────────────────────────────────────────────────────────
// 对应 [stmt.return]；clang: ReturnStmt。
// demo：return a;   → ReturnStmt{ value=VarExpr{a} }
//       return;     → ReturnStmt{ value=nullptr }  （void 返回）
struct ReturnStmt : Statement {
    ExprPtr value; // 可为 nullptr（void 返回）
    explicit ReturnStmt(ExprPtr v = nullptr)
        : Statement(NodeKind::Return), value(std::move(v)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// [stmt.if]：条件、then 与可选 else。参考：clang::IfStmt。
struct IfStmt : Statement {
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch; // 可为 nullptr
    IfStmt(ExprPtr cond, StmtPtr then_, StmtPtr else_ = nullptr)
        : Statement(NodeKind::If), condition(std::move(cond)),
          thenBranch(std::move(then_)), elseBranch(std::move(else_)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── while 语句 ───────────────────────────────────────────────────────────────
// 对应 [stmt.iter]/[stmt.while]；clang: WhileStmt。
// demo：while (i) { i = i - 1; }
//   → WhileStmt{ condition=VarExpr{i},
//                body=BlockStmt[ AssignStmt{ target=VarExpr{i},
//                        value=BinaryExpr{Sub, VarExpr{i}, IntLiteral{1}} } ] }
struct WhileStmt : Statement {
    ExprPtr condition;
    StmtPtr body;
    WhileStmt(ExprPtr cond, StmtPtr b)
        : Statement(NodeKind::While), condition(std::move(cond)),
          body(std::move(b)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// [stmt.for]：保存初始化、可选条件、增量及循环体；省略条件表示持续循环。
// 参考：clang::ForStmt。
struct ForStmt : Statement {
    StmtPtr init;            // 进入循环前恰好执行一次
    ExprPtr condition;       // 每次迭代前求值；nullptr ⇒ 恒真
    StmtPtr increment;       // 每次迭代体结束后执行；nullptr ⇒ 无
    StmtPtr body;
    ForStmt(StmtPtr i, ExprPtr cond, StmtPtr incr, StmtPtr b)
        : Statement(NodeKind::For), init(std::move(i)),
          condition(std::move(cond)), increment(std::move(incr)),
          body(std::move(b)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// [stmt.break]/[stmt.cont]：break 退出循环或 switch，continue 进入下一次循环。
// 合法性由 Sema 检查，跳转目标由 CodeGen 的嵌套控制流栈提供。
struct CommaExpr : Expression {
    ExprPtr left;   // 只求副作用
    ExprPtr right;  // 整个表达式的值与类型
    CommaExpr(ExprPtr l, ExprPtr r)
        : Expression(NodeKind::Comma), left(std::move(l)), right(std::move(r)) {}
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── switch / case / default───────────────────────────────────
// 对应 [stmt.switch]：[stmt.stmt]/1 的 selection-statement 第三种。
// 值匹配的多路分支；case 标号可以出现在 switch 体的【任意语句位置】
// （C++ 允许穿插声明等），本实现的 codegen 用"比较链"降级：条件值存栈顶，
// 逐条 cmp-je 到各 case 标号，无匹配落 default 或出口 —— 对照 clang 的
// 两种下沉（小密值跳表 / 大稀疏比较链，SelectionDAG 决定），教学版恒用比较链。
struct SwitchStmt : Statement {
    ExprPtr condition;
    StmtPtr body;   // 通常是 BlockStmt；case/default 是它的直接子语句
    SwitchStmt(ExprPtr c, StmtPtr b)
        : Statement(NodeKind::Switch), condition(std::move(c)), body(std::move(b)) {}
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// case 标号：[stmt.switch]/1 的 case 常量表达式。value 在 Sema 折叠后
// 必须是整数字面量（clang："case value is not a constant expression"），
// 整数值回填进 constValue 供 codegen 的比较链取用。
struct CaseStmt : Statement {
    ExprPtr  value;
    StmtPtr  body;
    int64_t  constValue = 0;   // Sema 回填
    bool     resolved   = false;
    std::string asmLabel;      // CodeGen 第一遍收集 case 时分配的标号
    CaseStmt(ExprPtr v, StmtPtr b)
        : Statement(NodeKind::Case), value(std::move(v)), body(std::move(b)) {}
    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct DefaultStmt : Statement {
    StmtPtr body;
    std::string asmLabel;      // 同 CaseStmt
    DefaultStmt(StmtPtr b)
        : Statement(NodeKind::Default), body(std::move(b)) {}
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── do-while 循环 ───────────────────────────────────────────────────────────
// 对应 [stmt.do]：体【先执行一次】，再每轮结束测条件；continue 落在条件式
// （与 while 相同，与 for 的增量步不同）。空条件不合法（do-while 必须有条件）。
struct DoWhileStmt : Statement {
    StmtPtr body;
    ExprPtr condition;
    DoWhileStmt(StmtPtr b, ExprPtr c)
        : Statement(NodeKind::DoWhile), body(std::move(b)), condition(std::move(c)) {}
    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct BreakStmt : Statement {
    BreakStmt() : Statement(NodeKind::Break) {}
    void accept(AstVisitor& v) override { v.visit(*this); }
};
struct ContinueStmt : Statement {
    ContinueStmt() : Statement(NodeKind::Continue) {}
    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 代码块语句 ───────────────────────────────────────────────────────────────
// 对应 [stmt.block]/[stmt.dcl]；clang: CompoundStmt。函数体就是一个 BlockStmt。
// demo：{ int x = 1; return x; }
//   → BlockStmt{ statements=[ VarDeclStmt{x}, ReturnStmt{VarExpr{x}} ] }
struct BlockStmt : Statement {
    std::vector<StmtPtr> statements;
    explicit BlockStmt(std::vector<StmtPtr> stmts = {})
        : Statement(NodeKind::Block), statements(std::move(stmts)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── delete 语句 ──────────────────────────────────────────────────────────────
// 对应 [stmt.expr] / [expr.delete]；在语句位置出现的 `delete p;` 或 `delete[] p;`
struct DeleteStmt : Statement {
    ExprPtr pointerExpr;
    bool    isArray = false;
    explicit DeleteStmt(ExprPtr p, bool isArr = false)
        : Statement(NodeKind::DeleteStmt), pointerExpr(std::move(p)), isArray(isArr) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// 声明引入名字与结构签名，自身不产生表达式值。参考：clang::Decl。
struct Declaration : ASTNode {
    explicit Declaration(NodeKind k) : ASTNode(k) {}
};

// ─── 参数定义 ─────────────────────────────────────────────────────────────────
// 对应 [dcl.fct] 的函数参数；clang: ParmVarDecl。
// demo：int f(int a) 的参数列表 → [ Parameter{name="a", type=int} ]
struct Parameter {
    std::string name;
    TypePtr     type;
};

// ─── 函数声明 ─────────────────────────────────────────────────────────────────
// 对应 [dcl.fct]/[dcl.fct.def]；clang: FunctionDecl（成员函数为 CXXMethodDecl）。
// demo: int f(int a) { return a; } ⇒ FunctionDecl{ name=f, returnType=int,
//         parameters=[Parameter{a:int}], body=BlockStmt[ ReturnStmt[ VarExpr{a} ] ] }
// isVirtual/isOverride 用于类的虚函数（[class.virtual]）；mangledName 由阶段 4 填充；
// ownerClassName 非空表示成员函数。
struct FunctionDecl : Declaration {
    std::string          name;
    TypePtr              returnType;
    std::vector<Parameter> parameters;
    std::shared_ptr<BlockStmt> body;      // nullptr = 纯声明（无实现）
    bool                 isVirtual  = false;
    bool                 isOverride = false;
    // 后置 const（[dcl.fct]/7）：`int get() const` —— cv-qualifier-seq 是【函数类型】
    // 的一部分，改变隐式 this 的类型（T* → const T*），这是"const 对象不能调
    // 非 const 成员"的底层机制。本项目记录该标志用于日志与后续检查，
    //  未实现：`f()` 与 `f() const` 的重载区分（clang 视其为两个不同重载）。
    bool                 isConstMethod = false;
    // 类内 static 成员函数（[class.static]/2）：**没有隐式 this 参数**，
    // 调用不传对象（C::f() 与 obj.f() 等价，都不装 rdi）。
    //  static 数据成员使用独立存储，不属于实例字段。
    bool                 isStatic = false;
    std::string          mangledName;     // 符号修饰后的名字（阶段4填充）
    std::string          ownerClassName;  // 所属类名（如果是成员函数）

    FunctionDecl() : Declaration(NodeKind::Function) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using FuncDeclPtr = std::shared_ptr<FunctionDecl>;

// 虚表槽引用方法声明，符号取自该声明的 mangledName。
// 唯一定名点为 SemanticAnalyzer::registerFunction，布局阶段不重复计算。
inline std::string vtableEntrySymbol(const VTableEntry& entry) {
    return entry.method ? entry.method->mangledName : std::string();
}

// [dcl.ref]：别名绑定要求被绑定表达式具有可取得的地址。
// 变量、成员、下标和解引用可表示可寻址对象；字面量和算术结果不走此通道。
// 引用形参与局部绑定共用此判断入口，避免各自维护表达式种类清单。
inline bool isAddressableExpr(const ExprPtr& expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case NodeKind::Var:
            return true;                      // 局部 / 全局 / 引用变量：都有地址
        case NodeKind::Member: {
            auto mem = std::static_pointer_cast<MemberExpr>(expr);
            return isAddressableExpr(mem->object);
        }
        case NodeKind::Unary: {
            auto un = std::static_pointer_cast<UnaryExpr>(expr);
            return un->op == UnaryOp::Deref;  // *p：指针值就是地址
        }
        case NodeKind::Index: {
            auto idx = std::static_pointer_cast<IndexExpr>(expr);
            TypePtr objType = idx->object->resolvedType;
            if (objType && objType->isReference()) objType = objType->referencedType;
            // 裸数组 / 指针的下标是真正的对象访问 ⇒ 可寻址；
            // 容器下标走 at()（按值返回）⇒ 不可寻址。
            return objType && (objType->isArray() || objType->isPointer());
        }
        default:
            return false;                     // 字面量 / 调用结果 / 算术结果
    }
}

// ─── 构造函数初始化器 ─────────────────────────────────────────────────────────
// 对应 [class.base.init]；clang: CXXCtorInitializer。
// demo：Point(int x, int y) : m_x(x), m_y(y) {} 中的 m_x(x)
struct CtorInitializer {
    std::string          memberName; // 字段名或基类名
    std::vector<ExprPtr> arguments;  // 传入的实参
    SourceLocation       location;
};

// ─── 构造函数声明 ─────────────────────────────────────────────────────────────
// 对应 [class.ctor]；clang: CXXConstructorDecl。
// 构造函数没有返回值类型，可携带初始化列表 initList。
struct ConstructorDecl : FunctionDecl {
    std::vector<CtorInitializer> initList;
    bool                         isDefaultCtor = false; // 是否为编译器合成的默认构造

    ConstructorDecl() {
        kind = NodeKind::Constructor;
    }

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using CtorDeclPtr = std::shared_ptr<ConstructorDecl>;

// ─── 析构函数声明 ─────────────────────────────────────────────────────────────
// 对应 [class.dtor]；clang: CXXDestructorDecl。
// 析构函数名字为 ~ClassName，无参无返回值，可声明为 virtual。
struct DestructorDecl : FunctionDecl {
    bool isDefaultDtor = false; // 是否为编译器合成的默认析构

    DestructorDecl() {
        kind = NodeKind::Destructor;
    }

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using DtorDeclPtr = std::shared_ptr<DestructorDecl>;

// ─── 全局变量声明 ─────────────────────────────────────────────────────────────
// 对应 [dcl.dcl] / [dcl.init]；clang: VarDecl（全局变量 ⇒ isStaticDataMember() == false、
//   hasGlobalStorage() == true）。
// demo：int g_counter = 0;
struct GlobalVarDecl : Declaration {
    std::string name;
    TypePtr     declaredType;
    ExprPtr     initializer; // 初始化式可为空；constexpr 声明则要求初始化式可常量求值，并登记为传播和 NTTP 的常量源。
    bool isConstexpr = false;

    GlobalVarDecl() : Declaration(NodeKind::GlobalVar) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using GlobalVarDeclPtr = std::shared_ptr<GlobalVarDecl>;

// ─── 枚举声明 ─────────────────────────────────────────────────────────────────
// 对应 [dcl.enum]；clang: EnumDecl / EnumConstantDecl。
// demo：enum Color { Red = 1, Green, Blue };
//       enum class Status : int { Ok = 0, Error = -1 };
struct EnumItem {
    std::string    name;
    int64_t        value = 0;
    bool           hasCustomValue = false;
    ExprPtr        valueExpr = nullptr;
    SourceLocation location;
};

struct EnumDecl : Declaration {
    std::string           name;
    bool                  isScoped = false; // enum class / enum struct
    TypePtr               underlyingType;   // 底层类型（默认 int）
    std::vector<EnumItem> items;

    EnumDecl() : Declaration(NodeKind::Enum) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using EnumDeclPtr = std::shared_ptr<EnumDecl>;

// ─── 命名空间声明 ─────────────────────────────────────────────────────────────
// 对应 [namespace.def]；clang: NamespaceDecl。
// demo：namespace Math { int add(int a, int b) { return a + b; } }
struct NamespaceDecl : Declaration {
    std::string          name;
    std::vector<DeclPtr> declarations;

    NamespaceDecl() : Declaration(NodeKind::Namespace) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using NamespaceDeclPtr = std::shared_ptr<NamespaceDecl>;

// ─── 类型别名声明 ─────────────────────────────────────────────────────────────
// 对应 [dcl.typedef] / [dcl.type.simple]；clang: TypeAliasDecl / TypedefDecl。
// demo：using IntPtr = int*;  或  typedef int* IntPtr;
struct TypeAliasDecl : Declaration {
    std::string aliasName;
    TypePtr     underlyingType;

    TypeAliasDecl() : Declaration(NodeKind::TypeAlias) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using TypeAliasDeclPtr = std::shared_ptr<TypeAliasDecl>;

// [class]/[class.derived]：保存类成员与直接基类，Sema 据此构建对象布局和虚表。
// 参考：clang::CXXRecordDecl。
struct StaticConstMember {
    int64_t value = 0;
    TypePtr type;          // 常量自身的类型（bool / int …）
};

// ─── static 数据成员【声明】（[class.static.data]；）───────────────
// 与 staticConsts（纯编译期常量表）互补：这是完整的静态数据成员 ——
// 占独立存储（类外定义 `int A::count = 7;` 发射全局符号），不占对象内存、
// 不进 classLayout（sizeof(A) 不含它）。
// 类内初始化式（initializer 非空）只允许 const 整型族/constexpr
// （[class.static.data]/3）：这种成员没有存储，读一律编译期折叠成字面量。
struct StaticDataMember {
    std::string name;
    TypePtr     type;
    ExprPtr     initializer;      // 类内初始化式（可空 ⇒ 等类外定义给存储）
    bool        isConstexpr = false;
};

// 前置声明：ClassDecl 内部要持有嵌套类的 shared_ptr（见下方 nestedClasses）。
struct ClassDecl;
using ClassDeclPtr = std::shared_ptr<ClassDecl>;

struct ClassDecl : Declaration {
    std::string              name;
    std::vector<std::string> baseClassNames; // 基类名列表（空 = 无继承；
                                             // 单继承 = 1 个元素；多继承 = 声明顺序）
    std::vector<FieldInfo>   fields;         // 字段列表
    std::vector<FuncDeclPtr> methods;        // 方法列表
    TypePtr                  classType;      // 对应的 Type 对象
    AccessModifier           currentAccess = AccessModifier::Private;

    // [dcl.type.elab]：`struct A;` 只登记名字，后续定义再提供成员与布局。
    bool isForwardDecl = false;

    // 静态常量表供编译期成员访问折叠；不计入对象字段布局。
    std::unordered_map<std::string, StaticConstMember> staticConsts;

    // ── static 数据成员【声明表】（[class.static.data]；）──────────
    // Parser 类体分支解析 `static int count;` / `static const int limit = 7;` 的产物。
    // 与 staticConsts 的关系：类内给了初值的 const 整型族成员【同时】登记进
    // staticConsts（值在声明点已确定，`Trait<T>::value` 折叠路由此对普通类可用）；
    // staticDataMembers 是权威声明表 —— 类外定义的校验（有无声明/类型/重定义）
    // 只认它。符号表另注册 "类名::成员" 全名（inferVar/inferMember 的查找源）。
    std::vector<StaticDataMember> staticDataMembers;

    // [dcl.typedef]：成员别名保存名字与目标类型，如 `using type = T;`、`typedef T type;`。
    std::unordered_map<std::string, TypePtr> typeAliases;
    std::vector<std::string> typeAliasOrder;   // 保持声明顺序，便于日志可观测

    // ── 成员模板（[temp.mem]）──────────────────────────────────────────────
    //   struct S { template <class T> T id(T x) { return x; } };
    // 每个元素是一个函数模板蓝图（funcTemplate 非空），与自由函数模板**同构** ——
    // 调用点用同一套合一算法推导，只是实例化出来的函数带 ownerClassName。
    std::vector<std::shared_ptr<TemplateDecl>> memberTemplates;

    // [class.nest]：嵌套类保存在成员树中，由 Sema 统一限定名字。
    // `A::B` 与其他类中的 B 保持独立身份；模板实例化递归克隆该树。
    std::vector<ClassDeclPtr> nestedClasses;

    // ── 外层类的【限定名】（"A" / "N::A"）；空 = 顶层类 ────────────────────
    //  它是【类作用域链】的一环：类作用域的名字查找沿它逐级外溯
    // （B 里的成员函数能看见 A 的成员 ⇒ 与 clang 的 DeclContext 父链同形）。
    std::string ownerClassName;

    ClassDecl() : Declaration(NodeKind::Class) {}

    // 便捷访问：第一个基类（无继承时返回空串）
    std::string firstBase() const {
        return baseClassNames.empty() ? "" : baseClassNames.front();
    }

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// [temp]：模板声明保存待替换的类、函数、别名或推导指引蓝图。
// `template<class T> T id(T);` 的函数体在实例化后进入正常语义分析。
enum class TemplateParamKind {
    Type,     // typename T, class T
    NonType,  // int N, bool Flag 等非类型模板参数 (NTTP)
    Template, // template <class> class C —— 模板模板参数 [temp.param]/4
};

struct TemplateParam;                       // 前置声明，理由见下方「指针形式」说明
using TemplateParamPtr = std::shared_ptr<TemplateParam>;

struct TemplateParam {
    TemplateParamKind kind = TemplateParamKind::Type;
    std::string       name;
    TypePtr           nonType = nullptr; // 值形参保存其声明类型；模板模板形参另存内层签名，供逐位种类/类型匹配。
    std::vector<TemplateParamPtr> innerParams;

    // ── 默认模板实参（[temp.param]/12）──
    // demo: template<typename T, typename U = void> 里 U 的 `= void` ⇒ hasDefault = true
    // 用 hasDefault 而非 defaultArg.type == nullptr 判"有无默认"：显式写 `= void` 时
    //   type 非空但语义上仍是"有默认"，两者必须分开。
    // 【标准约束】默认实参只能给在主模板（及别名模板）的形参表；偏特化不得有默认实参；
    //   且一旦某位形参有默认值，其后的每一位都必须有。
    TemplateArg       defaultArg;
    bool              hasDefault = false;

    // ── 无名形参（[temp.param]/3）──
    // demo: template <typename T, typename = void> 第 2 位形参只用默认值参与 void_t 推导。
    // 内部给一个合成名（"$unnamed0"）当替换表的键 —— 表总得有键，而 '$' 不是合法标识符
    //   字符，保证用户代码写不出同名引用，语义上等价于"这个名字不可见"。
    bool              isUnnamed = false;

    SourceLocation    location;
};

// 形参用 shared_ptr 持有，使 vector 扩容不移动节点，作用域帧中的形参指针保持有效。
// 参考：clang::TemplateParameterList 同样保存稳定的形参节点指针。
// 模板种类独立记录：主模板 `Box<T, U>`、偏特化 `Box<T*, T>`、全特化 `Box<int*, int>`。
// [temp.class.spec.match]：全特化精确匹配优先，其次匹配偏特化，否则使用主模板。
enum class TemplateSpecKind {
    Primary,      // 主模板
    PartialSpec,  // 偏特化：形参表非空，且 specPattern 中含模板参数（如 T*）
    ExplicitSpec, // 全特化：形参表为空 template<>，specPattern 全为具体类型
};

// [temp.deduct.guide]：推导指引保存参数表与目标模板 id，如 `Box(int) -> Box<int>;`。
// 参考：clang::CXXDeductionGuideDecl。
struct DeductionGuideDecl : Declaration {
    std::vector<TemplateParamPtr> templateParams;  // 指引自身的模板形参（可为空）
    std::string                guideName;       // 被指引的类模板名（如 "MyPtr"）
    std::vector<Parameter>     parameters;      // 指引的形参表（推导模式 P）
    std::vector<TypePtr>       targetArgs;      // `->` 右侧的实参（含模板形参）

    DeductionGuideDecl() : Declaration(NodeKind::DeductionGuide) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using DeductionGuideDeclPtr = std::shared_ptr<DeductionGuideDecl>;

struct TemplateDecl : Declaration {
    std::vector<std::string>   typeParams;     // 模板参数名列表（如 ["T", "N"]，向后兼容）
    std::vector<TemplateParamPtr> templateParams; // 结构化模板形参列表（含类型/非类型区分）
    ClassDeclPtr               classTemplate;  // 类模板蓝图（与 funcTemplate 互斥）
    FuncDeclPtr                funcTemplate;   // 函数模板蓝图（S1+）
    TypeAliasDeclPtr           aliasTemplate;  // 别名模板蓝图：template<T> using X = ...;
    DeductionGuideDeclPtr      guide;          // 模板蓝图可承载推导指引；特化模式与主模板形参表分别保存，供后续匹配。
    TemplateSpecKind           specKind = TemplateSpecKind::Primary;
    std::vector<TemplateArg>   specPattern;

    TemplateDecl() : Declaration(NodeKind::Template) {}

    bool isPrimary()      const { return specKind == TemplateSpecKind::Primary; }
    bool isPartialSpec()  const { return specKind == TemplateSpecKind::PartialSpec; }
    bool isExplicitSpec() const { return specKind == TemplateSpecKind::ExplicitSpec; }
    bool isSpecialization() const { return specKind != TemplateSpecKind::Primary; }

    bool isClassTemplate()    const { return classTemplate != nullptr; }
    bool isFunctionTemplate() const { return funcTemplate != nullptr; }
    // ── 别名模板 [temp.alias] ──
    // template<class T> using Vec = MyPtr<T>;
    //  别名【不是新类型】，只是既有类型的另一个名字：Vec<int> 和 MyPtr<int> 是同一个
    // 类型，不产生包装类、不产生新符号 —— 故它没有"实例化"，只有"替换"（换掉形参）。
    // clang 对照：TypeAliasTemplateDecl + AliasTemplateSpecializationType（为诊断保留
    // 一层 sugar，本实现直接解糖到最终类型）。
    bool isAliasTemplate()    const { return aliasTemplate != nullptr; }
    // ── 推导指引（[temp.deduct.guide]）──
    // 【它不是"模板"】借用 TemplateDecl 只因语法上共用 `template<...>` 前缀外壳；指引没有
    //   实体、没有符号、不被实例化 —— 它只是 CTAD 可问到的一条"映射规则"（故也有
    //   templateParams）。
    bool isDeductionGuide()   const { return guide != nullptr; }
    // ── 成员模板（[temp.mem]）──
    // 与自由函数模板同构（funcTemplate 非空），区别只在"出生地"：它定义在类体内、
    // 实例化出的函数带 ownerClassName（有隐式 this）。调用点由 Sema 在对象的类里
    // 查到它，再走与自由函数模板完全相同的推导 + 实例化路径。
    bool isMemberTemplate = false;
    // 模板的主名（类名 / 函数名 / 别名名 / 被指引的类模板名）
    const std::string& templateName() const {
        if (isClassTemplate()) return classTemplate->name;
        if (isAliasTemplate()) return aliasTemplate->aliasName;
        if (isDeductionGuide()) return guide->guideName;
        return funcTemplate->name;
    }

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using TemplateDeclPtr = std::shared_ptr<TemplateDecl>;

// ─────────────────────────────────────────────────────────────────────────────
// TranslationUnit：编译单元的根节点
// ─────────────────────────────────────────────────────────────────────────────
// 对应 clang::TranslationUnitDecl。Parser 的最终产物，CodeGen 从这里开始遍历。
// demo: 含 1 函数 + 1 类的源文件 ⇒ TranslationUnit{ declarations=[FunctionDecl, ClassDecl] }
struct TranslationUnit {
    std::vector<DeclPtr> declarations;  // 所有顶层声明
};

} // namespace minicc
