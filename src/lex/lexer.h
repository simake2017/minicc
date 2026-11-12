#pragma once
// 单向词法扫描器：预处理文本 → 带位置的 Token 流。
// 参考 clang/Lex/Lexer.cpp: Lexer::LexTokenInternal；标准 [lex.token]。
// 例：int x=42; → KwInt、Identifier、Assign、IntLiteral、Semicolon、Eof。
// 采用最长匹配：<= 是一个 Token，< = 是两个。

#include "token.h"
#include <string>
#include <string_view>
#include <vector>

namespace minicc {

// 扫描器持有源码及游标，支持逐个读取和一次性读取。
class Lexer {
public:
    // 按值接管源码，Token 的文本独立持有。
    explicit Lexer(std::string source);

    // 返回含末尾 Eof 的完整 Token 流。
    std::vector<Token> tokenizeAll();

    // 流式读取；耗尽后重复调用仍返回 Eof。
    Token nextToken();

private:
    // 扫描状态；行列号用于诊断。
    std::string m_source;
    size_t      m_pos    = 0;
    uint32_t    m_line   = 1;
    uint32_t    m_col    = 1;

    // 字符前瞻与消费。

    // 查看当前字符但不消费。demo: "x+1" ⇒ peek()=='x'，再 peek() 仍是 'x'
    char peek() const;
    // 查看下一个字符但不消费（判断 "==", "->", "//" 等双字符结构用）。
    // demo: "->" ⇒ peek()=='-'，peekNext()=='>'
    char peekNext() const;
    // 消费并返回当前字符，光标 +1；遇 '\n' 行号 +1、列号归 1，否则列号 +1。
    // demo: "a\nb" ⇒ advance()=='a'；再 advance()=='\n'（行 1→2、列归 1）
    char advance();
    // 源码是否已读完（光标越过最后一个字符）。demo: 空源码 ⇒ 构造后立即为 true
    bool isAtEnd() const;

    // ── 跳过空白与注释 ──
    // [lex.comment]：注释不是 token，词法上等价于空白，直接"吃掉"即可。
    // 每次 nextToken() 开头调用，保证产出的 token 之间没有杂质。
    void skipWhitespaceAndComments();

    // ── 扫描各类 Token（DFA 的四大分支）──
    // 统一约定：调用时光标指向该 token 的首字符；返回时光标恰好停在它之后。

    // 数字字面量（[lex.icon]）：按进制判前缀（0x/0b/前导 0），再吞整数后缀（u/U/l/L/z/Z）。
    // ★ 只切串、不解读 —— "值 + 形态" 由 Parser::parseIntLiteral 按 [lex.icon]/2 的表定。
    // demo: "42;" ⇒ Token{IntLiteral,"42"}，光标停在 ';'；"4ul" ⇒ Token{IntLiteral,"4ul"}
    Token scanNumber();
    // 标识符/关键字（[lex.name]/[lex.key]）：消费字母/数字/下划线后查 kKeywordMap
    // 决定是关键字还是标识符。
    // demo: "if(x" ⇒ Token{KwIf,"if"}，光标停在 '('；"foo " ⇒ Token{Identifier,"foo"}
    Token scanIdentifierOrKeyword();
    // 字符串字面量（[lex.string]）：消费到未转义的 '"' 为止，途中翻译转义序列。
    // demo: 源码 6 字符 "hi\n" ⇒ Token{StringLiteral, "hi"+换行符}，结尾引号被消费
    Token scanString();
    // 字符字面量（[lex.ccon]）：消费到未转义的 '\'' 为止，转义规则与 scanString 同表。
    // demo: 'a' ⇒ Token{CharLiteral,"a"}；'\n' ⇒ Token{CharLiteral, 换行符}；'' ⇒ 报错
    Token scanChar();
    // 运算符/分隔符（[lex.operators]）：按最长匹配决定取双字符还是单字符。
    // demo: "<=" ⇒ Token{LessEqual}；"<x" ⇒ Token{Less}，光标停在 'x'
    Token scanOperator();

    // ── 辅助 ──
    // 快照当前 (行, 列)：各扫描函数都在开头先拍快照，
    // 保证 token 的位置指向它的"起始处"而不是结束处。
    SourceLocation currentLocation() const;
    // 统一的 Token 装配入口，避免各扫描函数重复聚合初始化。
    // demo: makeToken(KwInt, "int", {1,1}) ⇒ Token{KwInt,"int",{1,1}}
    Token makeToken(TokenType type, std::string text, SourceLocation loc) const;
};

} // namespace minicc
