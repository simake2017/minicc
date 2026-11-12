// 文本预处理实现；输出交给 Lexer 继续切词。
// 参考 clang/Lex/PPDirectives.cpp、PPMacroExpansion.cpp。
// 例：#define N 10 使 int a[N]; → int a[10];；#if 0 隐藏非活跃分支。

#include "preprocessor.h"
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace minicc {

// 统一的预处理错误出口：拼上 文件:行号 后抛 runtime_error，
// 由 main.cpp 驱动末尾的 try/catch 兜底打印并以非零码退出。
[[noreturn]] static void ppError(const std::string& msg, const std::string& file, int line) {
    throw std::runtime_error(std::format("[Preprocessor Error] {}:{}: {}", file, line, msg));
}

// ── 词法小工具 ───────────────────────────────────────────────────────────
// 预处理器不预先分词，只在文本上按需读"词"：这三个函数界定了什么算一个词
// （等价于 C++ 标识符字符集）。demo: readWord("MAX(a,b)", 0) ⇒ out="MAX"。
bool Preprocessor::isWordStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; } // 是不是字母
bool Preprocessor::isWordChar(char c)  { return std::isalnum((unsigned char)c) || c == '_'; } // 数字

size_t Preprocessor::readWord(const std::string& s, size_t pos, std::string& out) {
    out.clear();
    while (pos < s.size() && isWordChar(s[pos])) out += s[pos++];
    return pos;
}

std::string Preprocessor::trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// 剥注释时识别字符串与字符字面量，块注释状态跨行保留。
// 例：x/*注释*/y → x y；"//" 与 '/' 保持原样。
std::string Preprocessor::stripComment(const std::string& line, bool& inBlockComment) {
    std::string out;
    bool inStr = false, inChar = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (inBlockComment) {
            // 注释内部：只认结束标记，其余一律换空格（保持列宽，也防记号粘连）
            if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                inBlockComment = false;
                out += "  ";  // */ 两个字符 ⇒ 两个空格
                i++;
                continue;
            }
            out += ' ';
            continue;
        }
        if (inStr) {
            out += c;
            if (c == '\\' && i + 1 < line.size()) { out += line[++i]; continue; }
            if (c == '"') inStr = false;
        } else if (inChar) {
            out += c;
            if (c == '\\' && i + 1 < line.size()) { out += line[++i]; continue; }
            if (c == '\'') inChar = false;
        } else if (c == '"')  { inStr = true;  out += c; }
        else if (c == '\'')   { inChar = true; out += c; }
        else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            break;  // 行注释：丢弃整段
        }
        else if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            size_t end = line.find("*/", i + 2);
            out += ' ';  // 块注释 → 空格（防止记号粘连）
            if (end == std::string::npos) {
                inBlockComment = true;  // ★ 本行没闭合 ⇒ 记下，后续行继续剥
                i = line.size();
            } else {
                i = end + 1;  // 同行的 */ 直接跳过
            }
        }
        else out += c;
    }
    return out;
}

// 读文件全文（#include 并合与主文件入口共用；打开失败抛异常）
std::string Preprocessor::readFileContents(const std::string& path) const {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error(std::format("Cannot open file: {}", path));
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// ── 入口 ─────────────────────────────────────────────────────────────────
// 保存 -I 搜索目录等选项；宏表、pragma once 集合、include 栈初始为空。
Preprocessor::Preprocessor(PreprocessorOptions opts) : m_opts(std::move(opts)) {
    // __MINICC__ 恒为 1，可直接登记为对象宏，不需要按展开位置计算。
    MacroDef minicc{"__MINICC__", {}, "1", false};
    m_macros[minicc.name] = minicc;
}

// 主文件也进入 include 栈，保证直接或间接包含自身能被检测。
std::string Preprocessor::processFile(const std::string& path) {
    std::string canon = fs::weakly_canonical(path).string();
    m_includeStack.push_back(canon);
    std::cout << std::format("  [pp] preprocessing main file: {}\n", path);
    std::string out = processText(readFileContents(path), path);
    m_includeStack.pop_back();
    return out;
}

// 处理单个文件的指令、条件编译和宏展开。
std::string Preprocessor::processText(const std::string& src, const std::string& fileName) {
    // ── 翻译阶段 2：行拼接（'\' + 换行 → 删除）──
    // 全文一次性扫描完成，先于一切指令识别——标准规定阶段 2 在阶段 3 之前，
    // 因此 '\' + 换行 甚至能插在 "#de" 与 "fine" 中间（拼接发生在任何解析之前）。
    std::string spliced;
    spliced.reserve(src.size());
    for (size_t i = 0; i < src.size(); i++) {
        // 这里 \ 后面必须立刻就是换行，不能有空格
        if (src[i] == '\\' && i + 1 < src.size() && src[i + 1] == '\n') { i++; continue; } // 这里i++,上面又i++,相当于i+2
        spliced += src[i];
    }

    // 条件栈同时记录外层是否活跃、已有分支是否命中及是否遇到 else。
    // 非活跃区域仍须维护嵌套层次，防止内部 #endif 关闭外层条件。
    struct Cond {
        bool active;       // 当前分支是否活跃
        bool takenBranch;  // 是否已有分支被采纳（#elif/#else 互斥用）
        bool parentActive; // 外层活跃性快照
        bool sawElse;
    };
    std::vector<Cond> condStack;
    // 当前嵌套层级是否活跃：栈空 → 全程活跃；否则取最内层的 active。
    auto enclosingActive = [&]() {
        return condStack.empty() ? true : condStack.back().active;
    };

    std::istringstream iss(spliced);
    std::string line;
    std::string out;
    int lineNo = 0;
    // 跨行块注释状态（每个文件独立 —— #include 递归时各层不串味）
    bool inBlockComment = false;

    while (std::getline(iss, line)) {
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string clean = stripComment(line, inBlockComment);
        std::string t = trim(clean);

        // 调试：每行源文本单独打一个 [src-line] 块标记 —— 测试里的
        // dumpWithExplanation 把它当作"分块锚点"插 === 分隔。
        // ⚠ 必须用 <SRC>…</SRC> 包裹原文，否则原文里的方括号会干扰切串。
        std::cout << std::format("  [pp] {}:{} [src-line] <SRC>{}</SRC>\n",
            fileName, lineNo, line);

        // 预处理指令必须位于去空白后的行首。
        if (!t.empty() && t[0] == '#') {
            std::string after = trim(t.substr(1));
            std::string dir;
            size_t p = readWord(after, 0, dir);
            std::string rest = trim(after.substr(p));

            // 条件指令即使在非活跃分支也要处理（需追踪嵌套层级）—— 否则死分支里的
            // #endif 会错误地弹掉外层栈帧。
            // demo: #ifdef DEBUG（已定义）⇒ cond=真 → 压入 {active:T, takenBranch:T,
            //       parentActive:T}，此后直到配对 #endif 普通行照常输出。
            // ⚠ parent 不活跃时 cond 保持 false 且【不求值】—— 死分支里的 #if
            //   表达式可能引用未定义宏，贸然求值会误报。
            if (dir == "ifdef" || dir == "ifndef" || dir == "if") {
                bool parent = enclosingActive();
                bool cond = false;
                if (parent) {  // 外层不活跃时不求值（表达式可能依赖未定义宏）
                    if (dir == "ifdef") {
                        std::string name; readWord(rest, 0, name);
                        cond = m_macros.count(name) > 0;
                    } else if (dir == "ifndef") {
                        std::string name; readWord(rest, 0, name);
                        cond = m_macros.count(name) == 0;
                    } else {
                        cond = evalConstantExpr(rest, fileName, lineNo) != 0;
                    }
                }
                bool active = parent && cond;
                condStack.push_back({active, active, parent, false});
                std::cout << std::format("  [pp] {}:{} #{} {} → branch {}\n",
                    fileName, lineNo, dir, rest, active ? "taken" : "skipped");
                out += '\n'; continue;
            }
            // #elif：外层活跃 && 前面无分支取真 && 本条件为真，三者同时满足才激活。
            // && 的短路求值保证：前面分支已取真时表达式根本不被求值（[cpp.cond]）。
            // demo: #if 0 / A 段 / #elif 1 / B 段 / #endif ⇒ A 段跳过，B 段输出。
            if (dir == "elif") {
                if (condStack.empty()) ppError("#elif without #if", fileName, lineNo);
                auto& st = condStack.back();
                if (st.sawElse) ppError("#elif after #else", fileName, lineNo);
                bool cond = st.parentActive && !st.takenBranch &&
                            evalConstantExpr(rest, fileName, lineNo) != 0;
                st.active = cond;
                if (cond) st.takenBranch = true;
                out += '\n'; continue;
            }
            // #else：前面所有分支都未取真时才激活；sawElse 拦截重复的 #else，
            // 也配合上面的检查拦截 #elif 出现在 #else 之后（均违反 [cpp.cond]）。
            if (dir == "else") {
                if (condStack.empty()) ppError("#else without #if", fileName, lineNo);
                auto& st = condStack.back();
                if (st.sawElse) ppError("duplicate #else", fileName, lineNo);
                st.sawElse = true;
                st.active = st.parentActive && !st.takenBranch;
                st.takenBranch = true;
                out += '\n'; continue;
            }
            // #endif：弹出本层状态，回到外层分支的活跃性。
            if (dir == "endif") {
                if (condStack.empty()) ppError("#endif without #if", fileName, lineNo);
                condStack.pop_back();
                out += '\n'; continue;
            }

            // 其余指令仅在活跃分支有效：死分支里的 #define/#include 等一律跳过 ——
            // 这就是 "#if 0 ... #endif 可以整段注释掉代码"的原理。
            if (!enclosingActive()) { out += '\n'; continue; } // 死分支里面的 语句一律无效

            // 指令分派表：各指令的处理函数见各自注释
            if (dir == "define")       handleDefine(rest, fileName, lineNo);
            else if (dir == "undef")   handleUndef(rest, lineNo);
            else if (dir == "include") out += handleInclude(rest, fileName, lineNo);
            else if (dir == "pragma")  handlePragma(rest, fileName, lineNo);
            else if (dir == "error")   ppError("#error " + rest, fileName, lineNo);
            else {
                std::cout << std::format(
                    "  [pp] {}:{} warning: unknown directive '#{}' ignored\n",
                    fileName, lineNo, dir);
            }
            out += '\n';  // 指令行 → 空行，保住本文件行号
            continue;
        }

        // ── 普通代码行 ──
        // 死分支 → 只留空行；活跃分支 → 宏展开后写入输出。
        // expand 的 hide 集初始为空（每行都是全新的展开上下文）。
        if (!enclosingActive()) { out += '\n'; continue; }
        out += expand(clean, {}, fileName, lineNo);
        out += '\n';
    }

    // 文件收尾时条件栈必须清空，否则说明 #if/#ifdef 缺配对的 #endif（[cpp.cond]）。
    if (!condStack.empty())
        ppError(std::format("unterminated conditional directive ({} open)",
            condStack.size()), fileName, lineNo);

    return out;
}

// 解析对象宏或函数宏定义。
// 例：#define N 10；#define MAX(a,b) ((a)>(b)?(a):(b))。
void Preprocessor::handleDefine(const std::string& rest, const std::string& fileName, int line) {
    size_t i = 0;
    std::string name;
    i = readWord(rest, 0, name);
    if (name.empty()) ppError("macro name missing in #define", fileName, line);

    MacroDef def;
    def.name = name;

    // 函数宏：'(' 必须紧跟宏名（[cpp.define]： intervening 空白即对象宏）
    // demo: "#define F(x) x" 是函数宏；"#define G (x)" 是对象宏（宏体为 "(x)"）。
    if (i < rest.size() && rest[i] == '(') {
        def.functionLike = true;
        i++;
        // 逐个读参数名：',' 分隔、')' 结束，参数间允许任意空白。
        // demo: "MAX(a, b)" ⇒ params = ["a", "b"]。
        while (true) {
            while (i < rest.size() && std::isspace((unsigned char)rest[i])) i++;
            if (i < rest.size() && rest[i] == ')') { i++; break; }
            std::string param;
            i = readWord(rest, i, param);
            if (param.empty()) ppError("expected parameter name in #define", fileName, line);
            def.params.push_back(param);
            while (i < rest.size() && std::isspace((unsigned char)rest[i])) i++;
            if (i < rest.size() && rest[i] == ',') { i++; continue; }
            if (i < rest.size() && rest[i] == ')') { i++; break; }
            ppError("expected ',' or ')' in macro parameter list", fileName, line);
        }
    }
    def.body = trim(rest.substr(i)); // 读取到行尾

    // 同名宏重复定义：宏体不同则警告（教学版宽松处理——直接覆盖；
    // 标准 [cpp.define] 要求重定义的替换列表完全相同，否则应诊断）。
    auto it = m_macros.find(name);
    if (it != m_macros.end() && it->second.body != def.body) {
        std::cout << std::format("  [pp] {}:{} warning: redefining macro '{}'\n",
            fileName, line, name);
    }
    m_macros[name] = def;

    if (def.functionLike) {
        std::string ps;
        for (size_t k = 0; k < def.params.size(); k++) {
            if (k) ps += ", ";
            ps += def.params[k];
        }
        std::cout << std::format("  [pp] {}:{} #define {}({}) = {}\n",
            fileName, line, name, ps, def.body);
    } else {
        std::cout << std::format("  [pp] {}:{} #define {} = {}\n",
            fileName, line, name, def.body);
    }
}

// #undef：从宏表移除名字（名字本就不存在也不报错，与标准一致 [cpp.undef]）。
// demo: #undef N 之后，arr[N] 中的 N 不再是宏，按普通标识符留给后续阶段。
void Preprocessor::handleUndef(const std::string& rest, int line) {
    std::string name = trim(rest);
    std::cout << std::format("  [pp] line {} #undef {}\n", line, name);
    m_macros.erase(name);
}

// 包含文件搜索与递归处理；具体目录顺序由 findIncludeFile 定义。
// canonical 路径使不同相对写法共享 once 标记与循环检测。
std::string Preprocessor::handleInclude(const std::string& rest,
                                        const std::string& fileName, int line) {
    if (rest.empty()) ppError("expected filename after #include", fileName, line);
    bool angled;
    std::string name;
    if (rest[0] == '"') {
        angled = false;
        size_t end = rest.find('"', 1);
        if (end == std::string::npos) ppError("unterminated \" in #include", fileName, line);
        name = rest.substr(1, end - 1);
    } else if (rest[0] == '<') {
        angled = true;
        size_t end = rest.find('>', 1);
        if (end == std::string::npos) ppError("unterminated < in #include", fileName, line);
        name = rest.substr(1, end - 1);
    } else {
        ppError("expected \"file\" or <file> after #include", fileName, line);
    }

    std::string path = resolveInclude(name, angled, fileName);
    std::string canon = fs::weakly_canonical(path).string();

    // 日志缩进随 include 深度增加，嵌套层次一目了然
    std::string indent(2 + m_includeStack.size() * 2, ' ');
    // #pragma once 去重：canonical 路径归一化后，同一物理文件只展开一次；
    // 二次 include 直接返回空串（工程上防重复定义的标准手段）。
    if (m_pragmaOnce.count(canon)) {
        std::cout << std::format("{}[pp] #include {} → skipped (pragma once)\n", indent, name);
        return "";
    }
    // 循环检测：a.h 包含 b.h、b.h 又包含 a.h 时，a.h 已在栈上 → 立刻报错。
    // 与 #pragma once 的分工：栈防的是"正在展开时绕回来"（无限递归），
    // pragma once 防的是"展开完之后再次遇到"（重复合并）。
    for (auto& s : m_includeStack) // 检测形成了嵌套递归
        if (s == canon)
            ppError(std::format("circular #include detected: {}", name), fileName, line);

    std::cout << std::format("{}[pp] #include {} → {}\n", indent, name, path);
    m_includeCount++;
    m_includeStack.push_back(canon);
    std::string expanded = processText(readFileContents(path), path); // 形成嵌套递归
    m_includeStack.pop_back();
    return expanded;
}

// 头文件搜索顺序：引号先查当前文件目录再查 -I；尖括号先查 -I 再查系统目录。
// 参考 clang HeaderSearch::LookupFile。
std::string Preprocessor::resolveInclude(const std::string& name, bool angled,
                                         const std::string& currentFile) {
    std::vector<std::string> tried;   // 记录所有尝试过的路径，报错时一并列出
    // 试探一个候选路径：记入 tried，文件存在即命中
    auto tryPath = [&](const std::string& p) {
        tried.push_back(p);
        return fs::exists(p);
    };

    // 引号形式：先搜当前文件所在目录（[cpp.include]）
    if (!angled) {
        fs::path p = fs::path(currentFile).parent_path() / name;
        if (tryPath(p.string())) return p.string();
    }
    // -I 目录（按命令行顺序）
    for (auto& d : m_opts.includePaths) {
        fs::path p = fs::path(d) / name;
        if (tryPath(p.string())) return p.string();
    }
    // 尖括号形式：系统默认目录
    if (angled) {
        if (tryPath("/usr/include/" + name)) return "/usr/include/" + name;
    }

    std::string msg = std::format("'{}' file not found; searched:", name);
    for (auto& t : tried) msg += "\n    " + t;
    throw std::runtime_error(msg);
}

// #pragma 处理：只实现 once（其余 pragma 警告后忽略，不中断编译）。
// canonical 归一化保证 "../pp/util.h" 与 "util.h" 指向同一物理文件时只登记一次。
void Preprocessor::handlePragma(const std::string& rest, const std::string& fileName, int line) {
    if (trim(rest) == "once") {
        std::string canon = fs::weakly_canonical(fileName).string();
        m_pragmaOnce.insert(canon);
        std::cout << std::format("  [pp] {}:{} #pragma once → registered {}\n",
            fileName, line, canon);
    } else {
        std::cout << std::format("  [pp] {}:{} warning: unsupported #pragma {} ignored\n",
            fileName, line, rest);
    }
}

// 递归重扫描用 hide 集禁用当前展开链中的宏，避免自引用循环。
// 例：#define A A 不会无限展开；普通嵌套宏仍可继续展开。
std::string Preprocessor::expand(const std::string& text,
                                 const std::unordered_set<std::string>& hide,
                                 const std::string& fileName, int line) {
    // 调试：递归深度 + 缩进，让 trace 视觉上分层。
    int d = m_expandDepth++;
    std::string indent(d * 2, ' ');
    std::string enterArrow = (d == 0 ? "┌─" : "├─");
    std::string exitArrow  = (d == 0 ? "└─" : "┴─");
    { std::string hs; for (auto& h : hide) { if (!hs.empty()) hs += ","; hs += h; }
      std::cout << std::format("  [pp] {}:{} {}{} d={} enter  text=[{}]  hide={{{}}}\n",
        fileName, line, indent, enterArrow, d, text, hs); }

    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];

        // 字符串字面量原样穿过（串内不展开）
        if (c == '"') {
            std::cout << std::format("  [pp] {}:{} {}│  str-literal pass-through\n",
                fileName, line, indent);
            out += c; i++;
            while (i < text.size()) {
                out += text[i];
                if (text[i] == '\\' && i + 1 < text.size()) { out += text[i + 1]; i += 2; continue; }
                if (text[i] == '"') { i++; break; }
                i++;
            }
            continue;
        }

        if (!isWordStart(c)) { out += c; i++; continue; } // 非字母原样接受

        std::string word;
        size_t after = readWord(text, i, word);

        // 内建动态宏（[cpp.predefined]）：值取决于展开发生的位置（文件/行号），
        // 不能存进宏表，每次遇到就地生成。
        if (word == "__LINE__") {
            std::cout << std::format("  [pp] {}:{} {}│  builtin __LINE__ → {}\n",
                fileName, line, indent, line);
            out += std::to_string(line); i = after; continue;
        }
        if (word == "__FILE__") {
            std::cout << std::format("  [pp] {}:{} {}│  builtin __FILE__ → \"{}\"\n",
                fileName, line, indent, fileName);
            out += std::format("\"{}\"", fileName); i = after; continue;
        }

        // 查宏表：未定义的词原样输出；hide 中"涂蓝"的词也原样输出——
        // 后者是递归展开的刹车（[cpp.rescan] 的自引用保护）。
        auto it = m_macros.find(word);
        if (it == m_macros.end()) {
            std::cout << std::format("  [pp] {}:{} {}│  ident [{}] not a macro → keep\n",
                fileName, line, indent, word);
            out += word; i = after; continue;
        }
        if (hide.count(word)) {
            std::cout << std::format("  [pp] {}:{} {}│  ident [{}] in hide-set → keep (no re-expand)\n",
                fileName, line, indent, word);
            out += word; i = after; continue;
        }

        const MacroDef& m = it->second;
        // 对象宏：宏名 → 宏体；宏名自身进 hide 后对宏体递归展开（重扫描）。
        if (!m.functionLike) {
            std::cout << std::format("  [pp] {}:{} {}│  object-macro {} → body=[{}]\n",
                fileName, line, indent, word, m.body);
            auto hide2 = hide;
            hide2.insert(word);
            std::string sub = expand(m.body, hide2, fileName, line);
            std::cout << std::format("  [pp] {}:{} {}│  object-macro {} resolved → [{}]\n",
                fileName, line, indent, word, sub);
            out += sub;
            i = after;
            continue;
        }

        // 函数宏：'(' 不跟随则按普通标识符输出（[cpp.replace]）
        size_t j = after;
        while (j < text.size() && std::isspace((unsigned char)text[j])) j++;
        if (j >= text.size() || text[j] != '(') {
            std::cout << std::format("  [pp] {}:{} {}│  [{}] is function-like but no '(' follows → keep as ident\n",
                fileName, line, indent, word);
            out += word; i = after; continue;
        }

        // 收集实参：括号配平，顶层逗号切分
        // demo: "MAX(f(1,2), y)" ⇒ f(1,2) 内的逗号处于 depth=1 层，不切分
        //       → 实参 ["f(1,2)", "y"]（2 个，而不是 3 个）。
        std::vector<std::string> args;
        std::string cur;
        int depth = 0;
        size_t k = j + 1;
        bool sawAny = false;
        for (; k < text.size(); k++) {
            char dch = text[k];
            if (dch == '(') { depth++; cur += dch; }
            else if (dch == ')') {
                if (depth == 0) break;
                depth--; cur += dch;
            }
            else if (dch == ',' && depth == 0) { args.push_back(trim(cur)); cur.clear(); sawAny = true; }
            else { cur += dch; sawAny = true; }
        }
        if (k >= text.size())
            ppError(std::format("unterminated argument list invoking macro '{}'", word),
                    fileName, line);
        // 空实参仍保留占位，但真正的空参数表不产生实参。
        if (sawAny || !args.empty()) args.push_back(trim(cur));
        if (args.size() == 1 && args[0].empty()) args.clear();  // f() → 0 个实参

        if (args.size() != m.params.size())
            ppError(std::format("macro '{}' expects {} argument(s), got {}",
                word, m.params.size(), args.size()), fileName, line);

        // 打印实参收集结果（展开前）
        { std::string as; for (auto& a : args) { if (!as.empty()) as += " | "; as += a; }
          std::cout << std::format("  [pp] {}:{} {}│  func-macro {}({})  args(raw)=[{}]\n",
            fileName, line, indent, word, trim(text.substr(j + 1, k - j - 1)), as); }

        // 实参先展开（[cpp.subst]），再按词边界替换参数名，最后整体重扫描
        // demo: #define N 10 时调用 MAX(N, x)
        //       → 实参 "N" 先展开为 "10" → 替换得 "((10)>(x)?(10):(x))"。
        std::vector<std::string> expandedArgs;
        for (auto& a : args)
            expandedArgs.push_back(expand(a, hide, fileName, line)); // 实参各自完整展开后再代入宏体

        // 打印实参展开后结果
        { std::string as; for (auto& a : expandedArgs) { if (!as.empty()) as += " | "; as += a; }
          std::cout << std::format("  [pp] {}:{} {}│  func-macro {} args(expanded)=[{}]\n",
            fileName, line, indent, word, as); }

        std::string body_before = m.body;
        std::string body2 = substituteParams(m, expandedArgs);
        std::cout << std::format("  [pp] {}:{} {}│  func-macro {} body before=[{}] → after=[{}]\n",
            fileName, line, indent, word, body_before, body2);

        auto hide2 = hide;
        hide2.insert(word);
        std::string sub = expand(body2, hide2, fileName, line);
        std::cout << std::format("  [pp] {}:{} {}│  func-macro {} final-substitution → [{}]\n",
            fileName, line, indent, word, sub);
        out += sub;
        i = k + 1;
    }
    // 返回前打印本次 expand 的最终 body（hide 含自涂蓝的宏名）
    std::cout << std::format("  [pp] {}:{} {}{} d={} return body=[{}]\n",
        fileName, line, indent, exitArrow, d, out);
    m_expandDepth--;
    return out;
}

// 按完整标识符替换形参，防止参数 a 改写 abc。
std::string Preprocessor::substituteParams(const MacroDef& m,
                                           const std::vector<std::string>& args) {
    std::string out;
    size_t i = 0;
    while (i < m.body.size()) {
        char c = m.body[i];
        if (c == '"') {   // 字符串字面量内不替换
            out += c; i++;
            while (i < m.body.size()) {
                out += m.body[i];
                if (m.body[i] == '\\' && i + 1 < m.body.size()) { out += m.body[i + 1]; i += 2; continue; }
                if (m.body[i] == '"') { i++; break; }
                i++;
            }
            continue;
        }
        if (!isWordStart(c)) { out += c; i++; continue; }
        std::string w;
        size_t after = readWord(m.body, i, w);
        bool replaced = false;
        for (size_t k = 0; k < m.params.size(); k++) {
            if (m.params[k] == w) { out += args[k]; replaced = true; break; }
        }
        if (!replaced) out += w;
        i = after;
    }
    return out;
}

// #if 先识别 defined，再展开宏并求值；未定义标识符按 0 处理。
// 例：defined(X) 查询宏表；未定义 X 时 #if X 不选中该分支。
long Preprocessor::evalConstantExpr(const std::string& expr,
                                    const std::string& fileName, int line) {
    // ── ① defined 处理 ──
    std::string s;
    size_t i = 0;
    while (i < expr.size()) { // 这一部分用于解析 defined 运算符
        if (isWordStart(expr[i])) {
            std::string w;
            size_t after = readWord(expr, i, w);
            if (w == "defined") {
                size_t j = after;
                while (j < expr.size() && std::isspace((unsigned char)expr[j])) j++;
                bool hasParen = j < expr.size() && expr[j] == '(';
                if (hasParen) j++;
                while (j < expr.size() && std::isspace((unsigned char)expr[j])) j++;
                std::string name;
                j = readWord(expr, j, name);
                if (name.empty()) ppError("expected identifier after 'defined'", fileName, line);
                if (hasParen) {
                    while (j < expr.size() && std::isspace((unsigned char)expr[j])) j++;
                    if (j >= expr.size() || expr[j] != ')')
                        ppError("expected ')' after defined(name", fileName, line);
                    j++;
                }
                s += m_macros.count(name) ? " 1 " : " 0 ";
                i = j;
                continue;
            }
            s += w; i = after; continue;
        }
        s += expr[i]; i++;
    }

    // ── ② 宏展开 ──
    s = expand(s, {}, fileName, line);

    // 递归下降求值按 C 风格优先级：||、&&、比较、加减、乘除、一元。
    struct Parser {
        std::vector<std::string> toks;
        size_t pos = 0;
        const std::string* file;
        int ln;

        std::string peekTok() { return pos < toks.size() ? toks[pos] : std::string(); }
        bool consume(const std::string& t) {
            if (peekTok() == t) { pos++; return true; }
            return false;
        }
        long primary() {
            std::string t = peekTok();
            if (consume("(")) {
                long v = exprOr();
                if (!consume(")")) ppError("expected ')' in #if expression", *file, ln);
                return v;
            }
            if (t.empty()) ppError("unexpected end of #if expression", *file, ln);
            if (std::isdigit((unsigned char)t[0])) { pos++; return std::stol(t); }
            if (isWordStart(t[0])) { pos++; return 0; }  // [cpp.cond]：剩余标识符 → 0
            pos++;
            ppError(std::format("unexpected token '{}' in #if expression", t), *file, ln);
        }
        long unary() {
            if (consume("!")) return !unary();
            if (consume("-")) return -unary();
            return primary();
        }
        long mul() {
            long v = unary();
            while (true) {
                if (consume("*")) v *= unary();
                else if (consume("/")) {
                    long d = unary();
                    if (d == 0) ppError("division by zero in #if", *file, ln);
                    v /= d;
                }
                else break;
            }
            return v;
        }
        long add() {
            long v = mul();
            while (true) {
                if (consume("+")) v += mul();
                else if (consume("-")) v -= mul();
                else break;
            }
            return v;
        }
        long rel() {
            long v = add();
            while (true) {
                if (consume("<=")) v = v <= add();
                else if (consume(">=")) v = v >= add();
                else if (consume("<"))  v = v < add();
                else if (consume(">"))  v = v > add();
                else break;
            }
            return v;
        }
        long eq() {
            long v = rel();
            while (true) {
                if (consume("==")) v = v == rel();
                else if (consume("!=")) v = v != rel();
                else break;
            }
            return v;
        }
        long exprAnd() { long v = eq(); while (consume("&&")) { long r = eq(); v = v && r; } return v; }
        long exprOr()  { long v = exprAnd(); while (consume("||")) { long r = exprAnd(); v = v || r; } return v; }
    };

    Parser p;
    p.file = &fileName;
    p.ln = line;
    // 手写迷你分词器：数字串、标识符、两字符运算符（==/!=/<=/>=/&&/||）
    // 优先于单字符识别，其余符号（括号、算术符）按单字符出。
    size_t k = 0;
    while (k < s.size()) {
        char c = s[k];
        if (std::isspace((unsigned char)c)) { k++; continue; }
        if (std::isdigit((unsigned char)c)) {
            std::string num;
            while (k < s.size() && std::isdigit((unsigned char)s[k])) num += s[k++];
            p.toks.push_back(num);
            continue;
        }
        if (isWordStart(c)) {
            std::string w;
            k = readWord(s, k, w);
            p.toks.push_back(w);
            continue;
        }
        std::string two = s.substr(k, 2);
        if (two == "==" || two == "!=" || two == "<=" || two == ">="
            || two == "&&" || two == "||") {
            p.toks.push_back(two);
            k += 2;
            continue;
        }
        p.toks.push_back(std::string(1, c));
        k++;
    }

    return p.exprOr();
}

} // namespace minicc
