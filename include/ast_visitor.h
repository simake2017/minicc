// AST 访问者：accept 选择节点对应的 visit 重载，递归由访问者自行控制。
// 参考 clang/AST/RecursiveASTVisitor.h；此处使用虚函数分派。
// 只前置声明节点，保持 ast.h → ast_visitor.h 的单向依赖。
// 例：BinaryExpr::accept 调用 visit(BinaryExpr&)；BlockStmt 同理。
#pragma once

namespace minicc {

// 节点定义见 ast.h；此清单与各节点的 accept 配套。
struct IntLiteralExpr;
struct CharLiteralExpr;
struct BoolLiteralExpr;
struct StringLiteralExpr;
struct NullptrLiteralExpr;
struct VarExpr;
struct BinaryExpr;
struct ConditionalExpr;
struct CommaExpr;
struct UnaryExpr;
struct CallExpr;
struct MemberExpr;
struct IndexExpr;
struct NewExpr;
struct DynamicCastExpr;
struct ThisExpr;
struct DeleteExpr;
// 语句
struct ExprStmt;
struct VarDeclStmt;
struct AssignStmt;
struct ReturnStmt;
struct IfStmt;
struct WhileStmt;
struct ForStmt;
struct SwitchStmt;
struct CaseStmt;
struct DefaultStmt;
struct DoWhileStmt;
struct BreakStmt;
struct ContinueStmt;
struct BlockStmt;
struct DeleteStmt;
struct EmptyStmt;
// 声明
struct FunctionDecl;
struct ConstructorDecl;
struct DestructorDecl;
struct GlobalVarDecl;
struct EnumDecl;
struct NamespaceDecl;
struct TypeAliasDecl;
struct ClassDecl;
struct DeductionGuideDecl;
struct TemplateDecl;

class AstVisitor {
public:
    virtual ~AstVisitor() = default;

    // 表达式
    virtual void visit(IntLiteralExpr&) {}
    virtual void visit(CharLiteralExpr&) {}
    virtual void visit(BoolLiteralExpr&) {}
    virtual void visit(StringLiteralExpr&) {}
    virtual void visit(NullptrLiteralExpr&) {}
    virtual void visit(VarExpr&) {}
    virtual void visit(BinaryExpr&) {}
    virtual void visit(ConditionalExpr&) {}
    virtual void visit(CommaExpr&) {}
    virtual void visit(UnaryExpr&) {}
    virtual void visit(CallExpr&) {}
    virtual void visit(MemberExpr&) {}
    virtual void visit(IndexExpr&) {}
    virtual void visit(NewExpr&) {}
    virtual void visit(DynamicCastExpr&) {}
    virtual void visit(ThisExpr&) {}
    virtual void visit(DeleteExpr&) {}

    // 语句
    virtual void visit(ExprStmt&) {}
    virtual void visit(VarDeclStmt&) {}
    virtual void visit(AssignStmt&) {}
    virtual void visit(ReturnStmt&) {}
    virtual void visit(IfStmt&) {}
    virtual void visit(WhileStmt&) {}
    virtual void visit(ForStmt&) {}
    virtual void visit(SwitchStmt&) {}
    virtual void visit(CaseStmt&) {}
    virtual void visit(DefaultStmt&) {}
    virtual void visit(DoWhileStmt&) {}
    virtual void visit(BreakStmt&) {}
    virtual void visit(ContinueStmt&) {}
    virtual void visit(BlockStmt&) {}
    virtual void visit(DeleteStmt&) {}
    virtual void visit(EmptyStmt&) {}

    // 声明
    virtual void visit(FunctionDecl&) {}
    virtual void visit(ConstructorDecl&) {}
    virtual void visit(DestructorDecl&) {}
    virtual void visit(GlobalVarDecl&) {}
    virtual void visit(EnumDecl&) {}
    virtual void visit(NamespaceDecl&) {}
    virtual void visit(TypeAliasDecl&) {}
    virtual void visit(ClassDecl&) {}
    virtual void visit(DeductionGuideDecl&) {}
    virtual void visit(TemplateDecl&) {}
};

} // namespace minicc
