#pragma once
// 递归下降 Parser：Token 流 → AST，名字分类由 Sema 活作用域表提供。
// 参考 clang/Parse/Parser.h、ParseExpr.cpp、ParseDecl.cpp。
// 例：int x=1; 生成声明；x+1; 生成表达式；template<class T> struct Box{} 保存蓝图。
// 表达式按优先级分层，试探解析可回滚游标与日志。

#include "token.h"
#include "ast.h"
#include "name_scope.h"
#include <vector>
#include <memory>
#include <stdexcept>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace minicc {

// 前置声明 Sema，保持接口依赖单向；对应 clang Parser 的 Actions 句柄。
class SemanticAnalyzer;

// 语法错误使用专用异常，试探解析仅捕获它以回滚。
// 其他异常继续传播，避免把内部故障误当作语法歧义。
class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

// 单向解析 Token 流；试探解析允许临时保存与恢复游标。
class Parser {
public:
    // 接管 Token 流并持有默认 Sema；attachSema 可接入外部语义实例。
    explicit Parser(std::vector<Token> tokens);
    // ★ 析构函数必须【在 .cpp 里定义】：成员是 std::unique_ptr<SemanticAnalyzer>
    //   而 SemanticAnalyzer 在本头文件里只有前向声明（不引 sema 域的头）。
    ~Parser();

    // 接入名字分类的 Sema 实例，Parser 登记与查询共用其活作用域表。
    void setSema(SemanticAnalyzer* sema) { m_sema = sema; }

    // 解析整个编译单元（入口函数，由 main 驱动调用）
    TranslationUnit parseTranslationUnit();

private:
    std::vector<Token> m_tokens;   // 完整 Token 序列（含末尾哨兵 Eof）
    size_t             m_pos = 0;  // 游标：下一个待消费的 Token 下标

    // 模板形参帧随模板体保持有效，内层同名形参遮蔽外层。
    // 例：Box<T> 的成员模板 get<U> 可同时查询 T 与 U。
    struct TemplateParamFrame {
        const TemplateDecl* owner  = nullptr;  // 归属：这是哪个 template<>
        size_t              count  = 0;        // 已注册形参个数（随解析推进增长）
        TemplateParamFrame* parent = nullptr;  // 外层帧（clang: Scope::getParent()）
        Parser*             parser = nullptr;  // 出栈时回写 m_currentFrame

        TemplateParamFrame(Parser* p, const TemplateDecl* d)
            : owner(d), parent(p->m_currentFrame), parser(p) {
            p->m_currentFrame = this;                        // ← 入栈
        }
        ~TemplateParamFrame() {
            if (parser) parser->m_currentFrame = parent;     // ← 出栈
        }
        TemplateParamFrame(const TemplateParamFrame&) = delete;
        TemplateParamFrame& operator=(const TemplateParamFrame&) = delete;
    };

    // 当前帧（无模板上下文时为 nullptr）—— 对应 clang 的"当前 Scope"
    TemplateParamFrame* m_currentFrame = nullptr;

    // 从内向外查询形参，返回种类信息而非仅判断存在。
    // 例：template<class T,int N> 中 T 是类型形参，N 是值形参。
    const TemplateParam* lookupTemplateParam(const std::string& name) const;

    // ── Token 流操作（LL(1) 前瞻的底层设施）──
    // 前瞻（lookahead）：不移动游标，查看当前 Token，据此决定走哪条产生式分支。
    const Token& current() const;            // 查看当前 Token（不消费）
    const Token& peek() const;               // current() 的别名（语义提示：只看不吃）
    const Token& advance();                  // 消费当前 Token 并推进游标，返回被消费的 Token
    bool         check(TokenType t) const;   // 前瞻判断：当前 Token 是否为 t？
    bool         match(TokenType t);         // check + 条件 advance（"尝试消费"）
    const Token& expect(TokenType t, const std::string& msg);  // 强制消费，不符则报错
    bool         isAtEnd() const;            // 是否到达流末尾（Eof 哨兵）

    // ── 错误处理（panic 模式：抛异常中止当前编译单元，不做 Token 级错误恢复）──
    [[noreturn]] void error(const std::string& msg) const;
    [[noreturn]] void errorAt(const Token& tok, const std::string& msg) const;

    // ── 类型解析 ──
    // type := ['const'] base-type ('*' | '&' | '&&')*
    // base-type := 'int'|'double'|'bool'|'void'|'auto' | IDENT(类名/模板参数名)
    //            | type-specifier-seq（整型家族的多关键字写法，见下）
    TypePtr parseType();

    // 内置整型家族的 type-specifier-seq（[dcl.type.simple]）——
    //   `unsigned long long int` / `signed char` / `short` 这类由 1~3 个关键字拼成的写法。
    // 调用约定：仅当【当前 Token 是 short/long/signed/unsigned/char 之一】时才调用；
    //   函数负责把整段关键字吃干净并返回规范化的类型节点，不会回溯。
    // 单个关键字的写法（int / double / bool / void / auto）不走这里，仍由 parseType
    //   的分支快筛 —— 这样既有代码的日志逐字节不变。
    TypePtr parseBuiltinTypeSpecifierSeq();

    // 模板实参保留类型、整型值、常量表达式或模板名，常量求值由 Sema 完成。
    // 参考 clang ParseTemplate.cpp: Parser::ParseTemplateArgumentList。
    // 例：Box<int>、Buf<4>、Buf<2+2>、Wrap<Box,int>；本入口消费两侧尖括号。
    std::vector<TemplateArg> parseTemplateArgumentList();
    // 该 token 是否会继续一个（以字面量开头的）表达式 —— 用于区分
    // 「裸字面量实参 Buf<4>」与「常量表达式实参 Buf<4+1>」。
    static bool isExprContinuation(TokenType t);

    // 模板值实参中裸 > 结束实参，括号内恢复普通比较表达式。
    // 例：Buf<2+2>；大于比较使用 Buf<(2>1)>。
    bool m_stopAtGreater = false;

    // ── constexpr 前缀的搬运 ────────────────────────────────
    // parseType 吃到 constexpr 时置真（并把它当 const 处理），紧随其后的
    // parseVarDeclStmt / parseGlobalVarDecl 读走并写进节点的 isConstexpr。
    // 只应出现在变量声明处；出现在形参表等其它位置会被静默当 const（已知边界）。
    bool m_pendingConstexpr = false;

    // 试探解析保存游标、上下文标记及日志缓冲，失败后不泄漏状态或输出。
    // 参考 clang TentativeParsingAction。
    class TokenCheckpoint {
        Parser& p;
        size_t pos;
        bool   stopAtGreater;
        bool   committed = false;
    public:
        explicit TokenCheckpoint(Parser& p) : p(p), pos(p.m_pos),
            stopAtGreater(p.m_stopAtGreater) { p.m_logBuffers.emplace_back(); }
        ~TokenCheckpoint() {
            std::string buf = std::move(p.m_logBuffers.back());
            p.m_logBuffers.pop_back();
            if (committed) {
                // 确认：日志落盘；嵌套试探（外层还在缓冲）⇒ 并入外层缓冲，
                // 由最外层的 checkpoint 统一决定冲刷或丢弃。
                if (p.m_logBuffers.empty()) std::cout << buf;
                else p.m_logBuffers.back() += buf;
            }
            // 未确认：缓冲随 token 进度一起废弃 —— 试探日志绝不泄漏成幽灵行。
            if (!committed) {
                p.m_pos             = pos;
                p.m_stopAtGreater   = stopAtGreater;
            }
        }
        void commit() { committed = true; }   // 试探成立：保留进度与日志
        TokenCheckpoint(const TokenCheckpoint&)            = delete;
        TokenCheckpoint& operator=(const TokenCheckpoint&) = delete;
    };

    // 试探期日志缓冲栈：空 ⇒ 打点直接出流；非空 ⇒ 追加到栈顶缓冲，由
    //   TokenCheckpoint 决定冲刷（commit）或丢弃（回卷）。模板-id 消歧等
    //   试探路径用；非试探路径行为与直写 std::cout 逐字节一致。
    std::vector<std::string> m_logBuffers;

    // ── Sema 活句柄（）──
    // ★ 只持指针，不持作用域数据 —— 作用域表住在 Sema 那边（红线 1）。
    //   自建 Parser 时由 m_ownedSema 兜底（clang 的 Actions 恒在位）。
    SemanticAnalyzer* m_sema = nullptr;
    // 自建 Sema（仅"Parser 单跑"时用；主线由 main 注入，见 src/driver/main.cpp）。
    std::unique_ptr<SemanticAnalyzer> m_ownedSema;

    // 名字登记助手统一类、模板与值的分类规则。
    void declareName(const std::string& name, unsigned bits);
    void openScope(ScopeKind kind, const std::string& name, unsigned bits);
    void closeScope();

    // 查询当前位置的名字种类，位定义见 include/name_scope.h；未找到返回 0。
    unsigned nameBits(const std::string& name) const;

    // C++20 [temp.names]：未声明名后跟 < 时，可按 UndeclaredTemplate 解析。
    // 已知值名仍按值处理，不能仅凭后续出现括号就判断为模板。
    bool isTemplateNameHere(const std::string& name) const;

    // 语句层链闸专用：实参表已解析完 ⇒ '<' 必然存在 ⇒ C++20 假定直接适用。
    bool isTemplateNameInTemplateId(const std::string& name) const;

    // 解析日志的统一出口；试探期间缓冲，提交后才输出。
    template<typename... Args>
    void logParse(std::string_view fmt, Args&&... args) {
        std::string line = std::vformat(fmt, std::make_format_args(args...));
        if (m_logBuffers.empty()) std::cout << line;
        else m_logBuffers.back() += line;
    }

    // ── 声明解析 ──
    // declaration := template-decl | class-decl | enum-decl | namespace-decl
    //              | type-alias-decl | global-var-decl | ['virtual'] function-decl
    DeclPtr            parseDeclaration();
    TemplateDeclPtr    parseTemplateDecl();
    // 模板形参表解析（[temp.param]）—— 自由模板与成员模板【共用同一份】。
    // 形参表是同一个文法、同一条语义规则 ⇒ 只有一处实现（两处各写一遍必然漂移）。
    // 调用方负责"建帧 + 开模板形参作用域"（模板体解析期间形参要在作用域内），
    // 本函数只管形参表本体 + expect('>') + 默认实参约束检查。
    void               parseTemplateParameterList(TemplateDecl& decl,
                                                   TemplateParamFrame& frame);
    // outSpecPattern 非空时，若类名后紧跟模板 id（Box<T*, T>），
    // 把尖括号里的模式写回该向量 —— 供 parseTemplateDecl 判定偏特化/全特化。
    ClassDeclPtr       parseClassDecl(std::vector<TemplateArg>* outSpecPattern = nullptr);
    EnumDeclPtr        parseEnumDecl();
    NamespaceDeclPtr   parseNamespaceDecl();
    TypeAliasDeclPtr   parseTypeAliasDecl();
    // ── 推导指引（[temp.deduct.guide]，理论见 docs/learn/28）──
    // 文法：Name '(' params ')' '->' Name '<' args '>' ';'
    //   template<class T> MyPtr(T) -> MyPtr<T>;   （模板指引，形参由外层 template<> 给）
    //   Box(int)          -> Box<int>;            （非模板指引，写死映射）
    // ⚠ 为什么要前瞻：`MyPtr(T) -> MyPtr<T>;` 与函数声明 `MyPtr f(T);` 前两个
    //   Token 完全一样（标识符 + '('），只能靠"配对右括号之后是不是 '->'"区分。
    // 对照 clang：clang/lib/Parse/ParseTemplate.cpp:Parser::ParseTemplateDeclarationOrSpecialization
    //   （靠 AllowDeductionGuide 参数区分指引与普通声明）。
    bool               looksLikeDeductionGuide() const;
    DeductionGuideDeclPtr parseDeductionGuide();
    GlobalVarDeclPtr   parseGlobalVarDecl();
    FuncDeclPtr        parseFunctionDecl(bool isVirtual = false,
                                         const std::string& ownerClass = "");
    FuncDeclPtr        parseMethodDecl(const std::string& ownerClass,
                                       AccessModifier access);
    CtorDeclPtr        parseConstructorDecl(const std::string& ownerClass);
    DtorDeclPtr        parseDestructorDecl(const std::string& ownerClass, bool isVirtual = false);
    std::vector<CtorInitializer> parseCtorInitializerList();

    // ── 语句解析 ──
    // stmt := compound-stmt | if-stmt | while-stmt | return-stmt
    //       | var-decl-stmt | expr-or-assign-stmt
    // 分派依据：前瞻 Token（'{'/'if'/'while'/'return'/类型关键字/标识符）
    StmtPtr parseStatement();
    // compound-stmt := '{' stmt* '}'
    StmtPtr parseBlockStmt();
    // var-decl-stmt := IDENT ['=' expr] ';'   （类型已由调用方 parseType 解析好）
    StmtPtr parseVarDeclStmt(TypePtr type);
    // if-stmt := 'if' '(' expr ')' stmt ['else' stmt]
    StmtPtr parseIfStmt();
    // while-stmt := 'while' '(' expr ')' stmt
    StmtPtr parseWhileStmt();
    // 数组声明符后缀解析：在变量名后收可选的 `[ 常量表达式 ]`
    TypePtr parseArrayDeclaratorSuffix(TypePtr baseType);

    StmtPtr parseForStmt();
    StmtPtr parseSwitchStmt();   // 主线 C 二批（[stmt.switch]）
    StmtPtr parseCaseStmt();
    StmtPtr parseDefaultStmt();
    StmtPtr parseDoWhileStmt();  // [stmt.do]
    StmtPtr parseBreakStmt();
    StmtPtr parseContinueStmt();
    // return-stmt := 'return' [expr] ';'
    StmtPtr parseReturnStmt();
    // delete-stmt := 'delete' ['[' ']'] expr ';'
    StmtPtr parseDeleteStmt();
    // expr-or-assign := expr ['=' expr] ';'
    StmtPtr parseExprOrAssignStmt();

    // ── 表达式解析（优先级从低到高：优先级越低在文法中越靠外/越晚绑定）──
    // 优先级上升链（每层都是 left-assoc 左结合）：
    //   expr := or-expr
    ExprPtr parseExpression();
    // or-expr  := and-expr ('||' and-expr)*
    ExprPtr parseOrExpr();
    // and-expr := equality ('&&' equality)*
    ExprPtr parseAndExpr();
    // equality := comparison (('==' | '!=') comparison)*
    ExprPtr parseEqualityExpr();
    // comparison := additive (('<'|'>'|'<='|'>=') additive)*
    ExprPtr parseComparisonExpr();
    // additive := multiplicative (('+' | '-') multiplicative)*
    ExprPtr parseAdditiveExpr();
    // multiplicative := unary (('*' | '/' | '%') unary)*
    ExprPtr parseMultiplicativeExpr();
    // unary := ('-' | '!') unary | postfix
    ExprPtr parseUnaryExpr();
    // postfix := primary ( call-args | ('.'|'->') IDENT )*
    ExprPtr parsePostfixExpr();
    // primary := INT|BOOL|STRING|nullptr|this | 'new' IDENT ['(' args ')']
    //          | IDENT ['<' type-list '>' ]（template-id，S3 显式模板实参）
    //          | '(' expr ')'
    ExprPtr parsePrimaryExpr();

    // ── 辅助 ──
    std::vector<ExprPtr> parseArgumentList();   // call-args := '(' [expr (',' expr)*] ')'
    std::vector<Parameter> parseParameterList(); // params := [type IDENT (',' type IDENT)*]
    BinaryOp tokenToBinaryOp(TokenType t) const;
};

} // namespace minicc
