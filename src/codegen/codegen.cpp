// x86-64 AT&T 汇编生成：消费 Sema 已解析的类型、调用符号与对象布局。
// 表达式结果约定放在 rax；类对象的值以地址表示，引用别名单独间接读写。
// 调用使用 System V AMD64 约定，类值返回通过隐藏 sret 缓冲指针传递。
// 参考：clang/lib/CodeGen/CodeGenFunction.cpp、CGExpr.cpp、CGCall.cpp。

#include "codegen.h"
// 复用阶段 4 的 NameMangler：生成 _ZTV/_ZTI 标准符号名
#include "template_instantiation.h"
#include <format>
#include <algorithm>
#include <cassert>
#include <set>

namespace minicc {

// 默认构造即可用：输出缓冲为空、计数器归零、偏移量表为空，全部由类内默认值完成
CodeGen::CodeGen() = default;

// sret 仅用于按值返回类对象；引用返回不得误走对象复制通道。
static TypePtr bareClassType(const TypePtr& t) {
    if (!t) return nullptr;
    if (t->isConst()) {
        TypePtr inner = t->stripConst();
        return inner ? inner : t;      // 拿不到内层就退回原类型（判据不崩、判"否"）
    }
    return t;
}

// 可写左值引用形参按地址传递；const 左值引用和右值引用形参当前按值传递。
// 后两者尚未物化右值临时量，不具备完整别名语义。
// 同时识别 Const(LValueReference(T)) 与 LValueReference(Const(T))。
static bool isWritableLValueRefType(TypePtr t) {
    if (!t || !t->isLValueReference()) return false;
    if (t->isConst()) return false;                    // Const(T&) ⇒ 不可写
    TypePtr inner = t->referencedType;
    bool innerIsConst = false;
    while (inner && inner->isConst()) { innerIsConst = true; inner = inner->innerType; }
    if (innerIsConst) return false;                    // T& 里的 const T ⇒ const 引用
    return inner != nullptr;
}

// [dcl.ref]：局部绑定有可寻址对象时使用别名；形参仅可写左值引用保证按地址传。
// 调用点与被调方共用此判据及同一 Type，避免一端传地址、另一端按值读取。
// `int& r = a` 的槽保存 &a；`f(int& x)` 接收调用方地址。
static bool isAliasBinding(TypePtr type, bool operandHasAddress) {
    if (!type || !type->isReference()) return false;
    if (isWritableLValueRefType(type)) return true;   // 实参必是左值 ⇒ 必有地址
    return operandHasAddress;                         // 其余引用：看绑定对象有没有地址
}

static bool returnsClassByValue(const TypePtr& t) {
    TypePtr bare = bareClassType(t);
    return bare && bare->isClass();
}

// 生成全局唯一的汇编标签。
// 理论：控制流语句（if/while）的跳转目标是"位置"，汇编里用标签表示；
//       计数器单调递增，保证多个 if/while 的标签互不冲突。
// demo: 依次调用 newLabel("else") → "else_0"，newLabel("endif") → "endif_1"
std::string CodeGen::newLabel(const std::string& prefix) {
    return std::format("{}_{}", prefix, m_labelCounter++);
}

// 写一条指令进 .text 段。统一加 4 空格缩进是 GAS 的排版惯例：
// 标签顶格、指令缩进，肉眼即可区分"跳转目标"与"要执行的指令"。
void CodeGen::emit(const std::string& line) {
    m_code << "    " << line << "\n";
}

// 写一行进 .data 段（可写全局数据：vtable、RTTI 结构）
void CodeGen::emitData(const std::string& line) {
    m_data << line << "\n";
}

// 写一行进 .rodata 段（只读数据：字符串字面量、类型名字符串）
void CodeGen::emitRodata(const std::string& line) {
    m_rodata << line << "\n";
}

// 写一条 GAS 行注释（# 开头），只影响 .s 可读性、不产生机器码。
// 这是教学编译器的重要可观测性手段：把编译期决策（字段偏移、vtable 下标）
// 直接写进汇编注释，让"符号 → 数字"的降级过程肉眼可查。
void CodeGen::emitComment(const std::string& comment) {
    m_code << "    # " << comment << "\n";
}

// 登记一个字符串字面量，返回引用它的标签。
// 字符串属只读数据不能内联进指令流，要放 .rodata 再 RIP 相对取址；此处只"登记"，
// 真正发射推迟到 generate() 末尾的 emitStringLiterals()（常量池集中在一处）。
// demo: addStringLiteral("hello") → 登记 ("str_0", "hello")，返回 "str_0"
//       最终 .rodata 中出现： str_0:  .string "hello"
std::string CodeGen::addStringLiteral(const std::string& value) {
    std::string label = newLabel("str");
    m_stringLiterals.emplace_back(label, value);
    return label;
}

// 汇编标签只保留可用字符；类型名中的标点须净化，避免模板或限定名破坏标签语法。
static std::string asmSymbol(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '$') {
            out += c;
        } else {
            out += '_';  // ':' '<' '>' '&' ',' '*' ' ' 等一律下划线
        }
    }
    return out;
}

// 析构槽依据方法声明名的 `~` 前缀识别，不能搜索符号子串。
// `~Outer` 是析构，`my_dtor_helper` 是普通方法。
static bool isDtorSlot(const VTableEntry& entry) {
    return entry.method && entry.method->name.starts_with("~");
}

// 生成编译单元的函数、全局数据、虚表、RTTI 与字符串常量。
// 先收集调用所需的声明，调用点与定义点共享 Sema 已确定的符号。
std::string CodeGen::generate(
    const TranslationUnit& unit,
    const std::unordered_map<std::string, TypePtr>& classTypes,
    const std::vector<FuncDeclPtr>& functions) {

    // 保存全局类类型表指针：发射成员函数、虚调用、new 时都要回头查它
    m_classTypes = &classTypes;

    // ── 建立"符号名 → 函数声明"表 ──
    // 调用点要回答"这个实参是不是按引用传" ⇒ 需要被调函数的**形参类型**。
    //  键是【Sema 已定好的符号名】（mangledName）—— 身份只有 Sema 一处裁决，
    // CodeGen 只是把"已定的身份"翻译成"形参是不是引用"（docs/learn/54 同款思路：
    // 别处只引用，别处不算）。
    for (auto& f : functions) {
        if (f && !f->mangledName.empty()) m_funcBySymbol[f->mangledName] = f;
    }

    // ── 建立"类名 → 基类名"表：emitRTTI 的基类表要用（旧格式兼容路径）──
    // 递归遍历顶层声明（含命名空间内）：ClassLayout 里不存基类名，继承关系的唯一
    // 事实来源是 AST 的 ClassDecl.baseClassNames。
    std::unordered_map<std::string, std::string> baseClassOf;
    std::function<void(const std::vector<DeclPtr>&)> collectBases =
        [&](const std::vector<DeclPtr>& decls) {
            for (auto& decl : decls) {
                // 按节点种类分派（需要 shared_ptr 交给递归 ⇒ 标签分派，非访问者）
                switch (decl->kind) {
                    case NodeKind::Class: {
                        auto cls = std::static_pointer_cast<ClassDecl>(decl);
                        baseClassOf[cls->name] = cls->firstBase();
                        break;
                    }
                    case NodeKind::Namespace:
                        collectBases(std::static_pointer_cast<NamespaceDecl>(decl)->declarations);
                        break;
                    case NodeKind::Template:
                        // 模板蓝图本身不产码；实例化后的 ClassDecl 已在 functions/
                        // 类型表层面处理，这里只登记非模板类即可。模板实例走
                        // NameMangler 的 _ZTI 名字，其基类信息与普通类同样经由
                        // ClassDecl 路径登记（若被实例化）。
                        break;
                    default:
                        break;
                }
            }
        };
    collectBases(unit.declarations);

    // ── 生成数据段：vtable、RTTI、字符串字面量 ──

    // 为每个有虚函数的类生成 vtable 和 RTTI
    for (auto& [name, type] : classTypes) {
        if (type->classLayout.hasVTable) {
            emitVTable(name, type);
            emitRTTI(name, type, baseClassOf[name]); // 无基类时取到空串
        }
    }

    // 补发【非多态基类】的 RTTI：多态派生类的 typeinfo 会逐个引用其所有基类
    // 的 _ZTI（emitRTTI 里的 base[i] typeinfo 指针），若某基类非多态（无 vtable）
    // 上面循环就漏发了它，链接期报 undefined reference to '_ZTINonPolyBase'。
    // 只在它确实被某个多态类当基类引用时才补发，避免给孤立非多态类平白产 RTTI。
    std::set<std::string> nonPolyBaseNeeded;
    for (auto& [name, type] : classTypes) {
        if (!type->classLayout.hasVTable) continue;
        for (auto& base : type->classLayout.bases) {
            auto bIt = classTypes.find(base.baseClassName);
            if (bIt != classTypes.end() && !bIt->second->classLayout.hasVTable)
                nonPolyBaseNeeded.insert(base.baseClassName);
        }
    }
    for (auto& baseName : nonPolyBaseNeeded) {
        emitRTTI(baseName, classTypes.at(baseName), baseClassOf[baseName]);
    }

    // 为全局变量生成数据段
    std::function<void(const std::vector<DeclPtr>&)> emitGlobalVars = [&](const std::vector<DeclPtr>& decls) {
        for (auto& decl : decls) {
            if (decl->kind == NodeKind::Namespace) {
                emitGlobalVars(std::static_pointer_cast<NamespaceDecl>(decl)->declarations);
                continue;
            }
            if (decl->kind != NodeKind::GlobalVar) continue;
            {
                auto gvar = std::static_pointer_cast<GlobalVarDecl>(decl);
                // asmSymbol：命名空间内全局变量名字带 "::"（Math::g_factor），
                // 净化成合法标签（标签端与 %rip 引用端必须一致）
                std::string sym = asmSymbol(gvar->name);
                emitData(std::format("    .globl {}             # 导出全局变量符号", sym));
                emitData("    .align 8                # 8 字节对齐");
                emitData(std::format("{}:                    # 全局变量标签", sym));
                if (gvar->declaredType && gvar->declaredType->isArray()) {
                    uint32_t asz = gvar->declaredType->sizeInBytes();
                    emitData(std::format("    .zero {}              # 全局数组零初始化 ({} 字节)",
                                         asz, asz));
                    m_arrayGlobals.insert(gvar->name);
                } else if (gvar->initializer &&
                    gvar->initializer->kind == NodeKind::IntLiteral) {
                    auto lit = std::static_pointer_cast<IntLiteralExpr>(gvar->initializer);
                    emitData(std::format("    .quad {}              # 初始化值：{}",
                                         lit->value, lit->value));
                } else if (gvar->initializer &&
                           gvar->initializer->kind == NodeKind::BoolLiteral) {
                    auto b = std::static_pointer_cast<BoolLiteralExpr>(gvar->initializer);
                    emitData(std::format("    .quad {}              # 初始化值：{}",
                                         b->value ? 1 : 0, b->value ? "true" : "false"));
                } else if (gvar->initializer) {
                    emitData("    .quad 0                # 默认零初始化");
                } else {
                    emitData("    .quad 0                # 零初始化");
                }
                emitData("");
            }
        }
    };
    emitGlobalVars(unit.declarations);

    // ── 生成代码段：所有函数 ──
    for (auto& func : functions) {
        if (func->body) {
            emitFunction(func);
        }
    }

    // 若出现过 dynamic_cast，补发运行时助手 __minicc_dynamic_cast
    if (m_needsDynamicCastHelper) {
        emitDynamicCastHelper();
    }

    // ── 生成只读数据段：字符串字面量 ──
    emitStringLiterals();

    // ── 拼装最终输出 ──
    std::ostringstream output;

    output << "# ═══════════════════════════════════════════════════════════\n";
    output << "# Mini C++ Compiler - x86-64 Assembly Output\n";
    output << "# Generated by minicc\n";
    output << "# ═══════════════════════════════════════════════════════════\n\n";

    output << "    .text\n";
    output << m_code.str();

    if (!m_data.str().empty()) {
        output << "\n    .data\n";
        output << m_data.str();
    }

    if (!m_rodata.str().empty()) {
        output << "\n    .section .rodata\n";
        output << m_rodata.str();
    }

    return output.str();
}

// 虚表使用简化 Itanium 布局：前置 offset-to-top 和 type_info，随后是方法槽。
// 对象 vptr 指向第一个方法槽，虚调用按 index * 8 取项，不再跳过头部。
void CodeGen::emitVTable(const std::string& className, TypePtr classType) {
    // Itanium ABI 标准符号：_ZTV + <长度><类名>，如 Dog → _ZTV3Dog
    std::string vtableLabel = NameMangler::mangleVTable(className);
    // 配套 typeinfo 符号：_ZTI + <长度><类名>，如 Dog → _ZTI3Dog
    std::string rttiLabel = NameMangler::mangleRTTI(className);

    // .globl 使符号对链接器可见（动态分发要跨目标文件引用这张表）
    emitData(std::format("    .globl {}             # 导出 vtable 符号", vtableLabel));
    // 8 字节对齐：表项是指针，对齐保证单次 8B 读取不跨界
    emitData(std::format("    .align 8                # 8 字节对齐", vtableLabel));
    emitData(std::format("{}:                        # vtable 标签", vtableLabel));

    // ── 主表段（primary vtable segment）──
    // 槽 0：offset-to-top —— 主表恒为 0（主基类子对象就在对象起始处）
    emitData("    .quad 0                    # offset-to-top = 0（主基类子对象与对象起始重合）");
    // 槽 1：type_info 指针，即 vtable[-1]（_vptr 向前退一格即可取到，
    //       typeid/异常类型匹配用它）
    emitData(std::format("    .quad {}       # RTTI type_info 指针（vtable[-1]）",
        rttiLabel));

    // 虚表条目经 vtableEntrySymbol 读取方法声明的 mangledName，与函数定义共享同一符号。
    for (auto& entry : classType->classLayout.vtableEntries) {
        emitData(std::format("    .quad {}   # vtable[{}]: 虚函数 {}",
            asmSymbol(vtableEntrySymbol(entry)), entry.index, vtableEntrySymbol(entry)));
    }

    // ── 次表段（secondary vtable segments）──
    // 每个次基类各占一段，表头 ott=-子对象偏移，槽位覆写项用 thunk
    for (auto& base : classType->classLayout.bases) {
        if (base.isPrimary || !base.hasVTable) continue;

        emitData(std::format("    # ── secondary vtable for '{}' @ offset {} ──",
            base.baseClassName, base.offset));
        // offset-to-top = -(子对象偏移)
        emitData(std::format("    .quad {}                   # offset-to-top = {}（次基类 '{}' 子对象偏移取反）",
            -(int)base.offset, -(int)base.offset, base.baseClassName));
        emitData(std::format("    .quad {}       # RTTI type_info 指针（次表段的 typeinfo 与主表相同）", rttiLabel));

        // 次表虚函数条目（覆写项填 thunk 地址，非覆写项填基类函数地址）
        for (size_t i = 0; i < base.entries.size(); ++i) {
            auto& entry = base.entries[i];
            const std::string sym = vtableEntrySymbol(entry);
            if (entry.isOverridden && entry.thunkAdjust != 0) {
                // 发射 thunk 跳板（简化 mangling：类名_函数名_thunk偏移）
                std::string thunkLabel = asmSymbol(std::format("{}_{}_thunk{}",
                    className, entry.baseFunctionName, -entry.thunkAdjust));
                emitThunk(thunkLabel, asmSymbol(sym), entry.thunkAdjust);
                emitData(std::format("    .quad {}   # secondary[{}]: {}（经 thunk 跳板，this 调整量={}）",
                    thunkLabel, i, sym, entry.thunkAdjust));
            } else {
                // 未覆写：直接填基类函数地址（调用方传基类 this，无需调整）
                emitData(std::format("    .quad {}   # secondary[{}]: {}（直接引用基类函数）",
                    asmSymbol(sym), i, sym));
            }
        }
    }

    emitData("");
}

// 多继承覆写的 thunk 将次基类 this 调整为派生类地址，再跳转到目标方法。
// 参考：Itanium C++ ABI §2.4。
void CodeGen::emitThunk(const std::string& thunkLabel,
                        const std::string& funcLabel, int thunkAdjust) {
    emitComment(std::format("thunk {} → {} (adjust={})", thunkLabel, funcLabel, thunkAdjust));
    emit(std::format(".globl {}             # 导出 thunk 符号供链接器可见", thunkLabel));
    emit(std::format("{}:                   # thunk 跳板入口标签", thunkLabel));
    emit("movq  (%rdi), %rax         # 从 this 首 8 字节读出 _vptr");
    emit("movq  -16(%rax), %rcx      # vptr[-2] = offset-to-top（次基类→最派生偏移）");
    emit("addq  %rcx, %rdi           # this += offset-to-top，归顶到最派生对象");
    emit(std::format("jmp   {}              # 无条件跳转到真实函数（已归顶的 this 直入）", funcLabel));
    emit("");
}

// RTTI 使用计数式基类表：类型名字、基类数量及各基类 type_info/子对象偏移。
// 0、1、多个基类采用统一布局，供运行时转换助手遍历。
void CodeGen::emitRTTI(const std::string& className, TypePtr classType,
                       const std::string& baseClassName) {
    std::string rttiLabel = NameMangler::mangleRTTI(className);
    std::string nameLabel = std::format(".Ltype_name_{}", asmSymbol(className));

    // RTTI 结构（计数式布局，统一处理 0/1/N 个基类）
    // 对照真实 __vmi_class_type_info：
    //   slot 0: vptr (simplified = 0)
    //   slot 1: name pointer
    //   slot 2: base count N
    //   slots 3..3+2N: [base_ti_0, offset_0, base_ti_1, offset_1, ...]
    emitData(std::format("    .globl {}             # 导出 RTTI 符号", rttiLabel));
    emitData(std::format("    .align 8                # 8 字节对齐"));
    emitData(std::format("{}:                        # typeinfo 标签", rttiLabel));
    emitData(std::format("    .quad 0                    # type_info vtable = 0（简化版，未链接真实 RTTI）"));
    emitData(std::format("    .quad {}                 # 指向类型名称字符串", nameLabel));

    // 基类计数 + 逐基类 typeinfo 指针 + 子对象偏移
    if (!classType->classLayout.bases.empty()) {
        emitData(std::format("    .quad {}                 # 基类计数（MI 计数风格）",
            classType->classLayout.bases.size()));
        for (auto& base : classType->classLayout.bases) {
            emitData(std::format("    .quad {}                 # base[{}] typeinfo 指针",
                NameMangler::mangleRTTI(base.baseClassName), base.baseClassName));
            emitData(std::format("    .quad {}                 # base[{}] 子对象偏移（字节）",
                base.offset, base.baseClassName));
        }
    } else if (!baseClassName.empty()) {
        // 单继承兼容路径（bases 为空但 baseClassName 非空 = 旧格式）
        emitData(std::format("    .quad 1                 # 基类计数 = 1"));
        emitData(std::format("    .quad {}                 # 基类 typeinfo 指针",
            NameMangler::mangleRTTI(baseClassName)));
        emitData(std::format("    .quad 0                 # 基类子对象偏移 = 0（单继承主基类）"));
    } else {
        emitData(std::format("    .quad 0                 # 无基类（基类计数 = 0）"));
    }

    // 类型名称字符串
    emitRodata(std::format("{}:                     # 类型名称标签", nameLabel));
    emitRodata(std::format("    .string \"{}\"           # 类型名称字符串", className));

    emitData("");
}

// 字符串统一发射到 .rodata，以标签供表达式引用，避免函数代码中夹入数据。
void CodeGen::emitStringLiterals() {
    for (auto& [label, value] : m_stringLiterals) {
        emitRodata(std::format("{}:                     # 字符串字面量标签", label));
        emitRodata(std::format("    .string \"{}\"        # 字符串内容（自动追加 '\\0'）", value));
    }
}

// 函数序言建立栈帧并将寄存器形参 spill 到栈槽，统一后续变量访问。
// sret 隐藏指针优先占 rdi；成员 this 与显式参数依次右移。
// 参考：clang/lib/CodeGen/CodeGenFunction.cpp:CodeGenFunction::GenerateCode。
void CodeGen::emitFunction(FuncDeclPtr func) {
    // 复位每函数状态：局部变量表、栈分配指针、所属类上下文
    m_localVars.clear();
    m_classLocals.clear();
    m_arrayLocals.clear();
    //  引用表是【作用域链】：层 0 = 函数作用域（形参 + 函数体顶层声明），
    // 每进一层 BlockStmt 压一层、出块弹一层（见 visit(BlockStmt)）。
    m_refScopes.clear();
    m_refScopes.push_back({});
    m_blockDtorStack.clear();
    m_currentStackOffset = 0;
    m_tempUsed = 0;            // 临时对象区：每函数重新接龙（槽位由 allocTempSlot 按序分配）
    m_currentClassName = func->ownerClassName;

    // 查找当前类类型
    m_currentClassType = nullptr;
    if (!m_currentClassName.empty() && m_classTypes) {
        auto it = m_classTypes->find(m_currentClassName);
        if (it != m_classTypes->end()) {
            m_currentClassType = it->second;
        }
    }

    // 确定函数名
    // 模板实例等带 mangledName 的优先用修饰名（阶段 4 NameMangler 产出），
    // 普通函数/方法直接用名字（成员方法的"类名_方法名"由 Sema 阶段填好）；
    // 最后统一过 asmSymbol 净化——命名空间函数的名字带 "::"（Sema 阶段
    // processNamespaceDecl 改名而来），不是合法汇编标签。
    std::string funcName = asmSymbol(func->mangledName.empty()
        ? func->name : func->mangledName);

    // .globl 导出符号：main 入口与跨函数/跨文件调用都靠它被链接器找到
    emit(std::format("    .globl {}             # 导出函数符号，使链接器可见", funcName));
    emit(std::format("{}:                       # 函数入口标签", funcName));

    emitComment(std::format("Function: {} (params: {})",
        func->name, func->parameters.size()));

    // 函数序言（Prologue）：pushq %rbp 保存调用者的帧基址（%rbp 是 callee-saved，
    //   离开前必须还原）；movq %rsp,%rbp 让新帧获得稳定锚点 —— 此后所有局部访问都是
    //   %rbp+常数偏移，与 %rsp 的瞬时位置（push/pop 变化）无关。
    emit("pushq %rbp                    # 保存调用者的帧基址到栈上");
    emit("movq %rsp, %rbp               # 建立新栈帧：rbp = rsp（此后用 rbp+偏移访问局部）");

    // 预估局部槽位后一次保留栈空间；类对象按布局大小占位，普通标量使用固定槽。
    uint32_t frameSize = estimateFrameSize(func);
    if (frameSize < 64) frameSize = 64;
    frameSize = (frameSize + 15) / 16 * 16;   // 16 字节对齐（System V）
    emit(std::format("subq ${}, %rsp              # 预留局部变量栈空间（16B 对齐）", frameSize));

    // 临时对象区起点 = 帧主体末尾（局部声明区 + 序言槽之后），
    // 构造调用的临时槽从这里的低地址侧接龙（estimateFrameSize 已为其预留字节）。
    m_tempBase = static_cast<int>(estimateFrameBodyBytes(func));

    // 保存参数到栈上
    // System V 整数参数寄存器序列；第 7 个及以后的参数应经栈传递
    // （本项目不支持 >6 参数）
    static const char* paramRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    int paramOffset = -8;

    // 成员函数的 this 占首个可用参数寄存器；有 sret 时位于隐藏返回指针之后。
    const bool hasThis = !func->ownerClassName.empty() && !func->isStatic;

    // 类值返回的隐藏缓冲指针先占 rdi，再排列 this 与显式参数。
    // 调用点与定义点检查同一返回 Type，保持传参约定一致。
    m_fnReturnsClassByValue = returnsClassByValue(func->returnType);
    m_sretPtrSlot = -1;
    m_sretValueSize = 0;
    if (m_fnReturnsClassByValue) {
        m_sretPtrSlot = paramOffset;                 // -8(%rbp)
        m_sretValueSize = sretBufferBytes(func->returnType);
        if (m_sretValueSize == 0) {
            // 返回类型是类却查不到布局 ⇒ 类没进类表（没实例化 / 前置声明未定义）。
            // 响亮报错：继续发代码只会静默拷 0 字节（返回垃圾）。
            throw std::runtime_error(std::format(
                "[CodeGen Error] sret return type '{}' not in class table (line {})",
                func->returnType ? func->returnType->toString() : "?",
                func->location.line));
        }
        emit(std::format("movq %{}, {}(%rbp)         # 保存隐藏返回缓冲指针（sret 第 0 参数）",
            paramRegs[0], m_sretPtrSlot));
        paramOffset -= 8;
    }

    if (hasThis) {
        m_localVars["this"] = paramOffset;
        //  sret 时 this 走 rsi（rdi 已被返回缓冲指针占用）
        emit(std::format("movq %{}, {}(%rbp)         # 保存 this 指针到栈槽",
            paramRegs[m_fnReturnsClassByValue ? 1 : 0], paramOffset));
        paramOffset -= 8;
    }

    // 形参 spill：无 this 的（自由函数 / static 成员）形参 i 用第 i 个寄存器；
    // 有 this 的整体右移一位（regIdx = i+1），因为 rdi 已被隐式第 0 参数占用；
    //  sret 的再右移一位（rdi 是返回缓冲指针）⇒ 最多 5 个显式实参。
    for (size_t i = 0; i < func->parameters.size() && i < 6; i++) {
        size_t regIdx = (hasThis ? 1 : 0) + (m_fnReturnsClassByValue ? 1 : 0) + i;
        if (regIdx < 6) {
            m_localVars[func->parameters[i].name] = paramOffset;
            emit(std::format("movq %{}, {}(%rbp)         # 形参 {} 从寄存器 spill 到栈",
                paramRegs[regIdx], paramOffset, func->parameters[i].name));
            //  引用形参：只有【可写的左值引用】（非 const 的 T&）才是别名 ——
            // 槽里放被绑定对象的地址。调用点用同一个判据（paramNeedsAddress）
            // 决定传地址还是传值 ⇒ 两侧同源，不会"一边传地址一边当值用"。
            if (isAliasBinding(func->parameters[i].type, false)) {
                m_refScopes.back()[func->parameters[i].name] = RefLocalInfo{
                    paramOffset, func->parameters[i].type->referencedType};
            }
            paramOffset -= 8;
        }
    }

    // 栈分配指针接到形参区末尾，之后的局部变量从这里继续向低地址分配
    m_currentStackOffset = paramOffset;

    // 构造函数：如果是构造函数，安装所有 vptr（多继承时可能有多个），并执行初始化列表
    if (func->kind == NodeKind::Constructor) {
        auto ctor = std::static_pointer_cast<ConstructorDecl>(func);
        std::string vtableLabel;
        if (m_currentClassType && m_currentClassType->classLayout.hasVTable) {
            vtableLabel = NameMangler::mangleVTable(m_currentClassName);
            emit("movq -8(%rbp), %rax         # 加载 this 指针");

            // 主 vptr：偏移 0，指向主表段（vtableLabel + 16）
            emit(std::format("leaq {}(%rip), %rcx    # 取 vtable 首地址", vtableLabel));
            emit("addq $16, %rcx              # 跳过 offset-to-top 与 RTTI，指向 vtable[0]");
            emit("movq %rcx, (%rax)           # 安装主 _vptr 到对象首 8 字节");

            // 次 vptr：每个次基类各一个，指向对应次表段
            for (auto& base : m_currentClassType->classLayout.bases) {
                if (base.isPrimary || !base.hasVTable) continue;
                uint32_t vptrOffset = base.vtableSegmentOffset + 16;
                emit(std::format("leaq {}(%rip), %rcx    # 取次表首地址（基类 '{}'）",
                    vtableLabel, base.baseClassName));
                emit(std::format("addq ${}, %rcx          # 跳过次表头，指向 secondary[0]", vptrOffset));
                emit(std::format("movq %rcx, {}(%rax)     # 安装次 _vptr（偏移 {} 处）",
                    base.offset, base.baseClassName));
            }
        }
        // 收集是否有基类构造调用
        bool hasBaseCtors = false;
        for (auto& init : ctor->initList) {
            for (auto& base : m_currentClassType->classLayout.bases) {
                if (init.memberName == base.baseClassName) { hasBaseCtors = true; break; }
            }
            if (hasBaseCtors) break;
        }

        for (auto& init : ctor->initList) {
            if (!m_currentClassType) continue;

            // 检查是否为基类构造调用（memberName 匹配 baseClassName）
            bool isBaseCtor = false;
            for (auto& base : m_currentClassType->classLayout.bases) {
                if (init.memberName == base.baseClassName) {
                    isBaseCtor = true;
                    // 调用基类构造函数（base_className_N 格式）
                    std::string baseCtorName = base.baseClassName + "_" + base.baseClassName;
                    if (!init.arguments.empty()) {
                        baseCtorName += "_" + std::to_string(init.arguments.size());
                    }
                    // 计算实参
                    for (size_t ai = 0; ai < init.arguments.size() && ai < 5; ai++) {
                        emitExpr(init.arguments[ai]);
                        emit("pushq %rax               # 暂存实参值到栈上");
                    }
                    for (int ai = static_cast<int>(init.arguments.size()) - 1; ai >= 0 && ai < 5; ai--) {
                        static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
                        emit(std::format("popq %{}              # 逆序弹出实参到寄存器", regs[ai]));
                    }
                    // this 指针：主基类用原始 this，次基类用 this + offset
                    emit("movq -8(%rbp), %rdi       # 加载 this 指针作为基类构造第 0 参数");
                    if (!base.isPrimary) {
                        emit(std::format("addq ${}, %rdi          # 次基类偏移调整（'{}'）",
                            base.offset, base.baseClassName));
                    }
                    emit(std::format("callq {}                # 调用基类构造函数 '{}'",
                        baseCtorName, base.baseClassName));
                    break;
                }
            }

            if (!isBaseCtor) {
                // 字段初始化
                const FieldInfo* field = m_currentClassType->classLayout.findField(init.memberName);
                if (field && !init.arguments.empty()) {
                    if (field->type && field->type->isClass()) {
                        // 类字段内联于 this + fieldOffset；构造时传子对象地址，不能当标量赋值。
                        const std::string& cn = field->type->name;
                        std::string ctorName = cn + "_" + cn + "_" +
                            std::to_string(init.arguments.size());
                        for (size_t ai = 0; ai < init.arguments.size() && ai < 5; ai++) {
                            emitExpr(init.arguments[ai]);
                            emit("pushq %rax               # 暂存嵌套构造实参");
                        }
                        for (int ai = static_cast<int>(init.arguments.size()) - 1; ai >= 0 && ai < 5; ai--) {
                            static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
                            emit(std::format("popq %{}              # 逆序弹出实参到寄存器", regs[ai]));
                        }
                        emit("movq -8(%rbp), %rdi       # 加载 this 指针");
                        emit(std::format("addq ${}, %rdi          # 调整到嵌套子对象 '{}'（偏移 {}）",
                            field->offset, field->name, field->offset));
                        emit(std::format("callq {}                # 调用嵌套类构造函数", ctorName));
                    } else {
                        // 字段存储按宽度选择 movb/movl/movq，避免覆盖相邻字段。
                        // 参考：clang/lib/CodeGen/CGExpr.cpp:CodeGenFunction::EmitStoreOfScalar。
                        emitExpr(init.arguments[0]);
                        emit("movq -8(%rbp), %rcx       # 加载 this 指针");
                        if (field->size == 1) {
                            emit(std::format("movb %al, {}(%rcx)    # 初始化字段 {}（偏移 {}，1B）",
                                field->offset, field->name, field->offset));
                        } else if (field->size <= 4) {
                            emit(std::format("movl %eax, {}(%rcx)    # 初始化字段 {}（偏移 {}，4B）",
                                field->offset, field->name, field->offset));
                        } else {
                            emit(std::format("movq %rax, {}(%rcx)    # 初始化字段 {}（偏移 {}，8B）",
                                field->offset, field->name, field->offset));
                        }
                    }
                }
            }
        }

        // 基类构造后重新安装 D 的 vptr（基类构造会覆盖为基类 vptr）
        if (hasBaseCtors && m_currentClassType->classLayout.hasVTable) {
            emitComment("re-install derived vptrs after base constructors");
            emit("movq -8(%rbp), %rax         # 加载 this 指针");
            emit(std::format("leaq {}(%rip), %rcx    # 取 vtable 首地址", vtableLabel));
            emit("addq $16, %rcx              # 跳过 offset-to-top 与 RTTI，指向 vtable[0]");
            emit("movq %rcx, (%rax)           # 重新安装主 _vptr（覆盖基类写入的）");
            for (auto& base : m_currentClassType->classLayout.bases) {
                if (base.isPrimary || !base.hasVTable) continue;
                uint32_t vptrOff = base.vtableSegmentOffset + 16;
                emit("movq -8(%rbp), %rax         # 加载 this 指针");
                emit(std::format("leaq {}(%rip), %rcx    # 取次表首地址（基类 '{}'）",
                    vtableLabel, base.baseClassName));
                emit(std::format("addq ${}, %rcx          # 跳过次表头，指向 secondary[0]", vptrOff));
                emit(std::format("movq %rcx, {}(%rax)     # 重新安装次 _vptr（偏移 {} 处）",
                    base.offset, base.baseClassName));
            }
        }
    }

    // 函数体顶层类对象也登记在析构层中，正常落尾时逆序析构。
    // 提前 return 由 emitDtorsOnReturn 就近处理。
    m_blockDtorStack.push_back({});
    if (func->body) {
        for (auto& stmt : func->body->statements) {
            emitStmt(stmt);
        }
    }

    // 落尾路径：析构函数层登记的栈对象（逆序）
    {
        std::vector<std::string> toDestroy = std::move(m_blockDtorStack.back());
        m_blockDtorStack.pop_back();
        for (auto it = toDestroy.rbegin(); it != toDestroy.rend(); ++it) {
            auto ci = m_classLocals.find(*it);
            if (ci == m_classLocals.end()) continue;
            emitComment(std::format("~{}() auto at function end (RAII)",
                ci->second.className));
            emitClassDtorCall(ci->second.className, ci->second.offset);
        }
    }

    // 检查最后一条语句是否是 return（避免重复 leave/ret）
    bool endsWithReturn = false;
    if (func->body && !func->body->statements.empty()) {
        endsWithReturn =
            func->body->statements.back()->kind == NodeKind::Return;
    }

    // 如果函数没有显式 return，添加默认返回
    if (!endsWithReturn) {
        if (func->kind == NodeKind::Constructor) {
            emit("movq -8(%rbp), %rax         # 构造函数返回 this 指针");
        } else if (func->returnType && func->returnType->isVoid()) {
            emit("movq $0, %rax               # void 函数返回 0");
        }

        // 函数尾声（Epilogue）
        // leave = movq %rbp,%rsp; popq %rbp（一步回收栈帧+恢复旧帧基址）
        // ret   = 弹出返回地址跳回调用方；返回值已按约定留在 rax
        emit("leave                         # 恢复栈帧（movq %rbp,%rsp; popq %rbp）");
        emit("ret                           # 返回调用者（从栈上弹出返回地址）");
    }
    emit("");
}

// 语句通过 AstVisitor 分派到对应 visit，避免重复维护节点类型判定。
void CodeGen::emitStmt(const StmtPtr& stmt) {
    if (!stmt) return;
    // 一次虚表跳转就落到对应的 visit（改造前是逐级 dynamic_pointer_cast 试探）。
    stmt->accept(*this);
}

// [class.dtor]：每个块记录本块构造的对象，块结束时按构造的逆序析构。
void CodeGen::visit(BlockStmt& block) {
    m_blockDtorStack.push_back({});   // enter scope：本块专属析构层
//  引用表同步压一层（作用域链）：块内声明的引用变量只在本块可见，
// 出块即失效 ⇒ 外层同名变量重新生效（见 findRefLocal 的注释）。
    m_refScopes.push_back({});

    for (auto& stmt : block.statements) {
        emitStmt(stmt);
    }

    // exit scope：取出本块登记的类对象，逆序析构（构造的镜像顺序）
    std::vector<std::string> toDestroy = std::move(m_blockDtorStack.back());
    m_blockDtorStack.pop_back();
    m_refScopes.pop_back();          // 引用表出块：内层遮蔽随之解除

    for (auto it = toDestroy.rbegin(); it != toDestroy.rend(); ++it) {
        auto ci = m_classLocals.find(*it);
        if (ci == m_classLocals.end()) continue;
        emitComment(std::format("~{}() auto at block end (RAII, reverse order)",
            ci->second.className));
        emitClassDtorCall(ci->second.className, ci->second.offset);
    }
}

// 块尾与提前退出共用栈对象析构入口；虚析构经虚表调用，普通析构直接调用声明符号。
void CodeGen::emitClassDtorCall(const std::string& className, int rbpOffset) {
    emit(std::format("leaq {}(%rbp), %rdi       # this = 栈上对象地址（直接取址，无指针变量）", rbpOffset));

    // 虚析构检查槽中的方法声明名，不以符号内是否含 dtor 判断。
    if (m_classTypes) {
        auto it = m_classTypes->find(className);
        if (it != m_classTypes->end()) {
            for (auto& entry : it->second->classLayout.vtableEntries) {
                if (isDtorSlot(entry)) {
                    emitVirtualCall(className, "dtor", {}, entry.index);
                    return;
                }
            }
        }
    }
    emit(std::format("callq {}            # 静态调用析构函数（非虚析构路径）",
        asmSymbol(className + "_dtor")));
}

// return 会绕过正常块尾，必须析构当前仍存活的局部对象。
// 析构调用可能覆盖返回寄存器，因此需保护已经求出的返回值。
bool CodeGen::emitDtorsOnReturn() {
    // 没有待析构对象时不生成额外保存/恢复指令。
    bool any = false;
    for (const auto& layer : m_blockDtorStack) {
        for (const auto& name : layer) {
            if (m_classLocals.count(name)) { any = true; break; }
        }
        if (any) break;
    }
    if (!any) return false;

    emit("pushq %rax                  # 保护返回值（析构调用会踩掉 rax）");
    for (auto layer = m_blockDtorStack.rbegin(); layer != m_blockDtorStack.rend(); ++layer) {
        for (auto it = layer->rbegin(); it != layer->rend(); ++it) {
            auto ci = m_classLocals.find(*it);
            if (ci == m_classLocals.end()) continue;
            emitComment(std::format("~{}() before return (RAII, scope exit)",
                ci->second.className));
            emitClassDtorCall(ci->second.className, ci->second.offset);
        }
    }
    emit("popq %rax                   # 恢复返回值");
    return true;
}

// 类对象判定需剥除顶层 const/引用，避免 `const S` 与 `S&` 被误当标量。
static TypePtr stripTopCvRef(TypePtr t) {
    while (t && (t->isConst() || t->isReference())) {
        t = t->isConst() ? t->stripConst() : t->referencedType;
    }
    return t;
}

// 按 AST 递归估算局部存储；分支与循环里的声明也需要预留槽位。
uint32_t CodeGen::estimateBlockSize(std::shared_ptr<BlockStmt> block) {
    if (!block) return 0;
    uint32_t total = 0;
    for (auto& stmt : block->statements) {
        // 按节点种类分派。这是【取值型】递归（累加 total），且各分支都要
        // shared_ptr 下钻 ⇒ NodeKind 标签分派 —— 判据同 Sema / Instantiator。
        switch (stmt->kind) {
            case NodeKind::VarDecl: {
                auto v = std::static_pointer_cast<VarDeclStmt>(stmt);
                uint32_t size = 8;
                if (v->declaredType && v->declaredType->isReference()) {
                    //  引用变量：槽里放被绑定对象的地址（8 字节）。
                    // 初始化式不可寻址时退化为按值 —— 仍是同一个 8 字节槽。
                    size = 8;
                } else if (v->declaredType && v->declaredType->isArray()) {
                    uint32_t asz = v->declaredType->sizeInBytes();
                    if (asz < 8) asz = 8;
                    size = (asz + 7) / 8 * 8;
                } else if (v->declaredType && !v->declaredType->isPointer() && m_classTypes) {
                    auto cit = m_classTypes->find(stripTopCvRef(v->declaredType)->name);
                    if (cit != m_classTypes->end() && cit->second->isClass()) {
                        size = cit->second->classLayout.totalSize;
                        if (size < 8) size = 8;
                        size = (size + 7) / 8 * 8;
                    }
                }
                total += size;
                break;
            }
            case NodeKind::Block:
                total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(stmt));
                break;
            case NodeKind::If: {
                auto i = std::static_pointer_cast<IfStmt>(stmt);
                if (i->thenBranch && i->thenBranch->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(i->thenBranch));
                if (i->elseBranch && i->elseBranch->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(i->elseBranch));
                break;
            }
            case NodeKind::While: {
                auto w = std::static_pointer_cast<WhileStmt>(stmt);
                if (w->body && w->body->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(w->body));
                break;
            }
            case NodeKind::For: {
                // for-init 若是声明（`for (int i = ...)`），变量作用域覆盖整个
                // 循环体，栈槽属于本帧：展开 init 与 body 两个块。
                //  不能只算 body —— init 的 VarDeclStmt 才是最常见的帧占用来源。
                auto f = std::static_pointer_cast<ForStmt>(stmt);
                if (f->init && f->init->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(f->init));
                if (f->init && f->init->kind == NodeKind::VarDecl) {
                    auto v = std::static_pointer_cast<VarDeclStmt>(f->init);
                    uint32_t size = 8;
                    if (v->declaredType && !v->declaredType->isPointer() && m_classTypes) {
                        auto cit = m_classTypes->find(stripTopCvRef(v->declaredType)->name);
                        if (cit != m_classTypes->end() && cit->second->isClass()) {
                            size = cit->second->classLayout.totalSize;
                            if (size < 8) size = 8;
                            size = (size + 7) / 8 * 8;
                        }
                    }
                    total += size;
                }
                if (f->body && f->body->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(f->body));
                break;
            }
            case NodeKind::DoWhile: {
                auto d = std::static_pointer_cast<DoWhileStmt>(stmt);
                if (d->body && d->body->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(d->body));
                break;
            }
            case NodeKind::Switch: {
                auto sw = std::static_pointer_cast<SwitchStmt>(stmt);
                if (sw->body && sw->body->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(sw->body));
                break;
            }
            case NodeKind::Case: {
                auto c = std::static_pointer_cast<CaseStmt>(stmt);
                if (c->body && c->body->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(c->body));
                break;
            }
            case NodeKind::Default: {
                auto d = std::static_pointer_cast<DefaultStmt>(stmt);
                if (d->body && d->body->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(d->body));
                break;
            }
            default:
                break;   // 其余语句（表达式/return/delete…）不占栈槽
        }
    }
    return total;
}

// 帧大小包含局部对象、形参 spill 与序言保留区，并满足栈对齐要求。
uint32_t CodeGen::estimateFrameBodyBytes(FuncDeclPtr func) {
    uint32_t total = estimateBlockSize(func->body);

    uint32_t slots = 1;                            // 帧基：paramOffset 从 -8 起
    if (!func->ownerClassName.empty()) slots++;    // this 先占 rdi，spill 一个槽
//  sret：隐藏返回缓冲指针也占一个 spill 槽（rdi ⇒ 形参与 this 整体右移）
// 判据与 emitFunction 的 m_fnReturnsClassByValue【必须同源】——
// 两处各判一次必漂（这里数少一个槽 ⇒ 形参 spill 写穿帧底）。
    const bool hasSret = returnsClassByValue(func->returnType);
    if (hasSret) slots++;
    for (size_t i = 0; i < func->parameters.size() && i < 6; i++) {
        size_t regIdx = (func->ownerClassName.empty() ? 0 : 1) + (hasSret ? 1 : 0) + i;
        if (regIdx < 6) slots++;
    }
    total += slots * 8;
    return total;
}

uint32_t CodeGen::estimateFrameSize(FuncDeclPtr func) {
    // 帧主体之外再加【临时对象区】：表达式级构造调用（Box<int>(7)）物化的
    // 临时对象占槽。数法与发射端 allocTempSlot 同序 —— 见 countCtorTempBytes。
    //  再加【sret 返回缓冲区】：按值返回类的调用在调用点要一块缓冲。
    // 与构造临时对象【分开计数】：现存程序一个这种调用都没有 ⇒ 帧大小不变。
    return estimateFrameBodyBytes(func) + countCtorTempBytes(func->body)
         + countSretBytes(func->body);
}

// ─── sret 返回缓冲：字节数与帧估算 ─────────────────────────────────────────
// 返回缓冲的大小 = 返回类型（类）的对象大小，按 8 字节对齐。
// 类不在类表里（未实例化 / 前置声明）⇒ 返回 0 ⇒ 调用点不按 sret 处理
// （此时多半是别处的错误，响亮报在语义层，不在这里猜）。
uint32_t CodeGen::sretBufferBytes(const TypePtr& returnType) {
    if (!returnsClassByValue(returnType) || !m_classTypes) return 0;
    TypePtr bare = bareClassType(returnType);
    auto it = m_classTypes->find(bare->name);
    if (it == m_classTypes->end()) return 0;
    uint32_t size = it->second->classLayout.totalSize;
    return (size + 7) / 8 * 8;
}

// 估算类值返回调用所需的缓冲空间，与构造临时对象的空间分别统计。
// `S x = f();` 顶层调用复用变量槽；嵌套实参的返回缓冲仍需计入。
uint32_t CodeGen::countSretBytes(std::shared_ptr<BlockStmt> block) {
    if (!block) return 0;
    uint32_t total = 0;
    for (auto& stmt : block->statements) {
        switch (stmt->kind) {
            case NodeKind::VarDecl: {
                auto v = std::static_pointer_cast<VarDeclStmt>(stmt);
                if (v->initializer) {
                    total += countSretBytesInExpr(v->initializer, v->initIsSretCall);
                }
                for (auto& a : v->ctorArgs) total += countSretBytesInExpr(a);
                break;
            }
            case NodeKind::Assign: {
                auto s = std::static_pointer_cast<AssignStmt>(stmt);
                total += countSretBytesInExpr(s->value);
                total += countSretBytesInExpr(s->target);
                break;
            }
            case NodeKind::ExprStmt: {
                auto s = std::static_pointer_cast<ExprStmt>(stmt);
                total += countSretBytesInExpr(s->expr);
                break;
            }
            case NodeKind::Return: {
                auto s = std::static_pointer_cast<ReturnStmt>(stmt);
                total += countSretBytesInExpr(s->value);
                break;
            }
            case NodeKind::If: {
                auto s = std::static_pointer_cast<IfStmt>(stmt);
                total += countSretBytesInExpr(s->condition);
                if (s->thenBranch && s->thenBranch->kind == NodeKind::Block)
                    total += countSretBytes(std::static_pointer_cast<BlockStmt>(s->thenBranch));
                if (s->elseBranch && s->elseBranch->kind == NodeKind::Block)
                    total += countSretBytes(std::static_pointer_cast<BlockStmt>(s->elseBranch));
                break;
            }
            case NodeKind::While: {
                auto s = std::static_pointer_cast<WhileStmt>(stmt);
                total += countSretBytesInExpr(s->condition);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countSretBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::DoWhile: {
                auto s = std::static_pointer_cast<DoWhileStmt>(stmt);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countSretBytes(std::static_pointer_cast<BlockStmt>(s->body));
                total += countSretBytesInExpr(s->condition);
                break;
            }
            case NodeKind::For: {
                auto s = std::static_pointer_cast<ForStmt>(stmt);
                if (s->init && s->init->kind == NodeKind::Block) {
                    total += countSretBytes(std::static_pointer_cast<BlockStmt>(s->init));
                } else if (s->init && s->init->kind == NodeKind::Assign) {
                    auto a = std::static_pointer_cast<AssignStmt>(s->init);
                    total += countSretBytesInExpr(a->value);
                    total += countSretBytesInExpr(a->target);
                }
                total += countSretBytesInExpr(s->condition);
                if (s->increment && s->increment->kind == NodeKind::Assign) {
                    auto a = std::static_pointer_cast<AssignStmt>(s->increment);
                    total += countSretBytesInExpr(a->value);
                    total += countSretBytesInExpr(a->target);
                }
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countSretBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::Switch: {
                auto s = std::static_pointer_cast<SwitchStmt>(stmt);
                total += countSretBytesInExpr(s->condition);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countSretBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::Case: {
                auto s = std::static_pointer_cast<CaseStmt>(stmt);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countSretBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::Default: {
                auto s = std::static_pointer_cast<DefaultStmt>(stmt);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countSretBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::Block:
                total += countSretBytes(std::static_pointer_cast<BlockStmt>(stmt));
                break;
            default:
                break;
        }
    }
    return total;
}

// 表达式子树里的 sret 缓冲计数（自顶向下、自左向右，与发射序一致）。
// 顺序与 countCtorTempBytesInExpr 同款：调用自身先算，再递归 callee，再递归实参。
uint32_t CodeGen::countSretBytesInExpr(const ExprPtr& expr, bool skipOwnBuffer) {
    if (!expr) return 0;
    uint32_t total = 0;
    switch (expr->kind) {
        case NodeKind::Call: {
            auto e = std::static_pointer_cast<CallExpr>(expr);
            if (!skipOwnBuffer && !e->isCtorCall && e->resolvedType) {
                uint32_t sz = sretBufferBytes(e->resolvedType);
                if (sz > 0) total += sz;
            }
            total += countSretBytesInExpr(e->callee);
            for (auto& a : e->arguments) total += countSretBytesInExpr(a);
            break;
        }
        case NodeKind::Binary: {
            auto e = std::static_pointer_cast<BinaryExpr>(expr);
            total += countSretBytesInExpr(e->left);
            total += countSretBytesInExpr(e->right);
            break;
        }
        case NodeKind::Unary: {
            auto e = std::static_pointer_cast<UnaryExpr>(expr);
            total += countSretBytesInExpr(e->operand);
            break;
        }
        case NodeKind::Conditional: {
            auto e = std::static_pointer_cast<ConditionalExpr>(expr);
            total += countSretBytesInExpr(e->condition);
            total += countSretBytesInExpr(e->thenExpr);
            total += countSretBytesInExpr(e->elseExpr);
            break;
        }
        case NodeKind::Comma: {
            auto e = std::static_pointer_cast<CommaExpr>(expr);
            total += countSretBytesInExpr(e->left);
            total += countSretBytesInExpr(e->right);
            break;
        }
        case NodeKind::Member: {
            auto e = std::static_pointer_cast<MemberExpr>(expr);
            total += countSretBytesInExpr(e->object);
            break;
        }
        case NodeKind::Index: {
            auto e = std::static_pointer_cast<IndexExpr>(expr);
            total += countSretBytesInExpr(e->object);
            total += countSretBytesInExpr(e->index);
            break;
        }
        case NodeKind::New: {
            auto e = std::static_pointer_cast<NewExpr>(expr);
            for (auto& a : e->constructorArgs) total += countSretBytesInExpr(a);
            break;
        }
        default:
            break;
    }
    return total;
}

// 临时对象槽的预估与实际发射必须使用相同遍历顺序，保证每次构造分到有效空间。
uint32_t CodeGen::countCtorTempBytes(std::shared_ptr<BlockStmt> block) {
    if (!block) return 0;
    uint32_t total = 0;
    for (auto& stmt : block->statements) {
        switch (stmt->kind) {
            case NodeKind::VarDecl: {
                auto v = std::static_pointer_cast<VarDeclStmt>(stmt);
                for (auto& a : v->ctorArgs) {
                    total += countCtorTempBytesInExpr(a);
                }
                break;
            }
            case NodeKind::Assign: {
                auto s = std::static_pointer_cast<AssignStmt>(stmt);
                total += countCtorTempBytesInExpr(s->value);
                total += countCtorTempBytesInExpr(s->target);
                break;
            }
            case NodeKind::ExprStmt: {
                auto s = std::static_pointer_cast<ExprStmt>(stmt);
                total += countCtorTempBytesInExpr(s->expr);
                break;
            }
            case NodeKind::Return: {
                auto s = std::static_pointer_cast<ReturnStmt>(stmt);
                total += countCtorTempBytesInExpr(s->value);
                break;
            }
            case NodeKind::Delete: {
                auto s = std::static_pointer_cast<DeleteStmt>(stmt);
                total += countCtorTempBytesInExpr(s->pointerExpr);
                break;
            }
            case NodeKind::If: {
                auto s = std::static_pointer_cast<IfStmt>(stmt);
                total += countCtorTempBytesInExpr(s->condition);
                if (s->thenBranch && s->thenBranch->kind == NodeKind::Block)
                    total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(s->thenBranch));
                if (s->elseBranch && s->elseBranch->kind == NodeKind::Block)
                    total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(s->elseBranch));
                break;
            }
            case NodeKind::While: {
                auto s = std::static_pointer_cast<WhileStmt>(stmt);
                total += countCtorTempBytesInExpr(s->condition);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::DoWhile: {
                auto s = std::static_pointer_cast<DoWhileStmt>(stmt);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(s->body));
                total += countCtorTempBytesInExpr(s->condition);
                break;
            }
            case NodeKind::For: {
                auto s = std::static_pointer_cast<ForStmt>(stmt);
                if (s->init && s->init->kind == NodeKind::Block) {
                    total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(s->init));
                } else if (s->init && s->init->kind == NodeKind::Assign) {
                    auto a = std::static_pointer_cast<AssignStmt>(s->init);
                    total += countCtorTempBytesInExpr(a->value);
                    total += countCtorTempBytesInExpr(a->target);
                }
                total += countCtorTempBytesInExpr(s->condition);
                // increment 是语句槽（本语言赋值是语句）：只展开赋值形态
                if (s->increment && s->increment->kind == NodeKind::Assign) {
                    auto a = std::static_pointer_cast<AssignStmt>(s->increment);
                    total += countCtorTempBytesInExpr(a->value);
                    total += countCtorTempBytesInExpr(a->target);
                }
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::Switch: {
                auto s = std::static_pointer_cast<SwitchStmt>(stmt);
                total += countCtorTempBytesInExpr(s->condition);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::Case: {
                auto s = std::static_pointer_cast<CaseStmt>(stmt);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::Default: {
                auto s = std::static_pointer_cast<DefaultStmt>(stmt);
                if (s->body && s->body->kind == NodeKind::Block)
                    total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(s->body));
                break;
            }
            case NodeKind::Block:
                total += countCtorTempBytes(std::static_pointer_cast<BlockStmt>(stmt));
                break;
            default:
                break;
        }
    }
    return total;
}

// 表达式子树里的构造调用计数（自顶向下、自左向右，与发射序一致）。
// 构造调用自身先占槽（发射时 visit(CallExpr) 入口即分配），再递归其实参。
uint32_t CodeGen::countCtorTempBytesInExpr(const ExprPtr& expr) {
    if (!expr) return 0;
    uint32_t total = 0;
    switch (expr->kind) {
        case NodeKind::Call: {
            auto e = std::static_pointer_cast<CallExpr>(expr);
            if (e->isCtorCall) {
                uint32_t sz = 8;
                if (m_classTypes) {
                    auto cit = m_classTypes->find(e->ctorClassType->name);
                    if (cit != m_classTypes->end()) {
                        sz = cit->second->classLayout.totalSize;
                    }
                }
                total += (sz + 7) / 8 * 8;
            }
            total += countCtorTempBytesInExpr(e->callee);
            for (auto& a : e->arguments) total += countCtorTempBytesInExpr(a);
            break;
        }
        case NodeKind::Binary: {
            auto e = std::static_pointer_cast<BinaryExpr>(expr);
            total += countCtorTempBytesInExpr(e->left);
            total += countCtorTempBytesInExpr(e->right);
            break;
        }
        case NodeKind::Unary: {
            auto e = std::static_pointer_cast<UnaryExpr>(expr);
            total += countCtorTempBytesInExpr(e->operand);
            break;
        }
        case NodeKind::Conditional: {
            auto e = std::static_pointer_cast<ConditionalExpr>(expr);
            total += countCtorTempBytesInExpr(e->condition);
            total += countCtorTempBytesInExpr(e->thenExpr);
            total += countCtorTempBytesInExpr(e->elseExpr);
            break;
        }
        case NodeKind::Comma: {
            auto e = std::static_pointer_cast<CommaExpr>(expr);
            total += countCtorTempBytesInExpr(e->left);
            total += countCtorTempBytesInExpr(e->right);
            break;
        }
        case NodeKind::Member: {
            auto e = std::static_pointer_cast<MemberExpr>(expr);
            total += countCtorTempBytesInExpr(e->object);
            break;
        }
        case NodeKind::Index: {
            auto e = std::static_pointer_cast<IndexExpr>(expr);
            total += countCtorTempBytesInExpr(e->object);
            total += countCtorTempBytesInExpr(e->index);
            break;
        }
        case NodeKind::New: {
            auto e = std::static_pointer_cast<NewExpr>(expr);
            for (auto& a : e->constructorArgs) total += countCtorTempBytesInExpr(a);
            break;
        }
        default:
            break;
    }
    return total;
}

// 分配一个临时对象槽：临时区从帧主体末尾向低地址接龙，返回对象起始的 rbp 负偏移。
// 与 countCtorTempBytesInExpr 同序调用 ⇒ 槽位恰好落在已预留的临时区内。
int CodeGen::allocTempSlot(uint32_t size) {
    uint32_t aligned = (size + 7) / 8 * 8;
    if (aligned < 8) aligned = 8;
    m_tempUsed += aligned;
    return -(m_tempBase + static_cast<int>(m_tempUsed));
}

// 沿继承图累计子对象偏移，支持多层派生类到基类的地址调整。
uint32_t CodeGen::getBaseOffset(const std::string& derivedClassName,
                                const std::string& targetBase) const {
    if (derivedClassName == targetBase) return 0;
    if (!m_classTypes) return 0;

    // BFS 队列：(当前类名, 累积偏移)
    std::vector<std::pair<std::string, uint32_t>> queue;
    queue.push_back({derivedClassName, 0});
    size_t head = 0;

    while (head < queue.size()) {
        auto [currentName, accOffset] = queue[head++];

        auto typeIt = m_classTypes->find(currentName);
        if (typeIt == m_classTypes->end()) continue;

        for (const auto& base : typeIt->second->classLayout.bases) {
            uint32_t newOffset = accOffset + base.offset;

            if (base.baseClassName == targetBase) {
                return newOffset;
            }

            queue.push_back({base.baseClassName, newOffset});
        }
    }

    return 0;  // 无继承关系或主基类
}

// 变量存储区分类对象、引用别名与标量：类按布局分配，别名存地址，标量存值。
void CodeGen::visit(VarDeclStmt& decl) {
    // 局部引用绑定到可寻址对象时槽里保存地址，后续读写作用于原对象。
    // 例如 `int a = 5; int& r = a; r = 30;` 修改 a。
    if (isAliasBinding(decl.declaredType,
                       decl.initializer && isAddressableExpr(decl.initializer))) {
        m_currentStackOffset -= 8;
        int refOffset = m_currentStackOffset;
        m_localVars[decl.name] = refOffset;
        m_refScopes.back()[decl.name] = RefLocalInfo{
            refOffset, decl.declaredType->referencedType};
        emitComment(std::format("ref {} : {} (alias → 被绑定对象的地址)",
            decl.name, decl.declaredType->toString()));
        emitAddressOf(decl.initializer);              // 绑到初始化式的地址
        emit(std::format("movq %rax, {}(%rbp)    # 引用 {} 绑定到该地址",
            refOffset, decl.name));
        return;
    }

    // 类变量拥有布局大小的对象存储；引用不拥有被绑定对象，不能重复构造或析构。
    if (decl.declaredType && !decl.declaredType->isPointer()
        && !decl.declaredType->isReference()
        && m_classTypes) {
        auto cit = m_classTypes->find(stripTopCvRef(decl.declaredType)->name);
        if (cit != m_classTypes->end() && cit->second->isClass()) {
            auto& layout = cit->second->classLayout;

            // 尺寸：布局算好的 totalSize，向上对齐 8（至少 8，保证
            // leaq 出来的地址 8 字节对齐，满足 System V 约定）
            uint32_t size = layout.totalSize;
            if (size < 8) size = 8;
            size = (size + 7) / 8 * 8;

            // 分配：栈向低地址增长，按对象实际尺寸推进分配指针
            // （不再固定 8 字节/变量——这是类对象支持的关键一步）
            m_currentStackOffset -= static_cast<int>(size);
            int offset = m_currentStackOffset;

            // 双表登记：m_localVars 供"名字 → 偏移"通用查询；
            // m_classLocals 额外记住"这是类对象"，供 emitVar 取址、
            // 块尾析构、清零三处使用
            m_localVars[decl.name] = offset;
            m_classLocals[decl.name] = ClassLocalInfo{
                cit->second->name, offset, size};
            if (!m_blockDtorStack.empty()) {
                m_blockDtorStack.back().push_back(decl.name);
            }

            emitComment(std::format("stack object {} : {} ({} bytes, RAII)",
                decl.name, cit->second->name, size));

            // 平凡拷贝初始化逐字节复制同类对象，如 `Box<int> b = a;`。
            // 无用户拷贝构造时使用对象布局大小，成员源对象须先求得真实地址。
            if (decl.initIsSretCall) {
                emitComment(std::format("sret init {} ← 调用直接写入本变量槽（拷贝省略）",
                    decl.name));
                m_sretTargetSlot = offset;
                emitExpr(decl.initializer);
                m_sretTargetSlot = kNoSretTarget;
                return;
            }

            if (decl.isTrivialCopy) {
                emitComment(std::format("trivial copy {} ← init ({} bytes)", decl.name, size));
                emitClassAddress(decl.initializer);   // rax = 源对象地址
                emit("movq %rax, %rsi          # 源对象地址");
                emit(std::format("leaq {}(%rbp), %rdi    # 目标 = &{}（变量槽）", offset, decl.name));
                // 逐字节内联复制：自研链接器只内置 malloc/free，memcpy 无定义可绑
                // （[builtin] 阶段注册的只是原型）⇒ 用 rep movsb 展开，等价 memcpy。
                emit(std::format("movq ${}, %rcx         # 复制字节数 = sizeof({})",
                    size, cit->second->name));
                emit("rep movsb                  # 平凡拷贝：逐字节复制对象表示");
                return;
            }

            // ① 零初始化整个对象（8 字节一拍）——构造函数写入字段前，
            //    其余字节必须是确定的 0（对应真实编译器的"默认成员初始化
            //    + 填充清零"；int 字段若构造未写就是 0，行为可观测）
            for (uint32_t i = 0; i < size; i += 8) {
                emit(std::format("movq $0, {}(%rbp)    # 零初始化 {} 偏移 +{}",
                    offset + static_cast<int>(i), decl.name, i));
            }

            // ② 安装 _vptr（仅多态类）：与 emitNew/构造函数内部安装同一公式
            //    ——对象偏移 0 处 = &_ZTV + 16（跳过 offset-to-top 与 RTTI）
            if (layout.hasVTable) {
                std::string vtableLabel = NameMangler::mangleVTable(cit->second->name);
                emit(std::format("leaq {}(%rip), %rcx    # 取 vtable 首地址", vtableLabel));
                emit("addq $16, %rcx              # 跳过 offset-to-top 与 RTTI，指向 vtable[0]");
                emit(std::format("movq %rcx, {}(%rbp)    # 安装主 _vptr 到栈对象",
                    offset));

                // 次 vptr：每个次基类各一个
                for (auto& base : layout.bases) {
                    if (base.isPrimary || !base.hasVTable) continue;
                    uint32_t vptrOffset = base.vtableSegmentOffset + 16;
                    emit(std::format("leaq {}(%rip), %rcx    # 取次表首地址（基类 '{}'）",
                        vtableLabel, base.baseClassName));
                    emit(std::format("addq ${}, %rcx          # 跳过次表头，指向 secondary[0]", vptrOffset));
                    emit(std::format("movq %rcx, {}(%rbp)    # 安装次 _vptr（偏移 {} 处）",
                        offset + static_cast<int>(base.offset), base.baseClassName));
                }
            }

            // 构造调用直接使用 Sema 回填的 ctorSymbol，不能在调用点重新推算符号。
            if (!decl.ctorArgs.empty() && !decl.ctorSymbol.empty()) {
                //  引用形参 ⇒ 传地址（ctorSymbol 是 Sema 选定的构造函数符号）
                FuncDeclPtr ctorDecl2 = lookupCallee(decl.ctorSymbol);
                for (size_t i = 0; i < decl.ctorArgs.size() && i < 5; i++) {
                    emitArgument(decl.ctorArgs[i], paramNeedsAddress(ctorDecl2, i));
                    emit("pushq %rax                  # 构造实参压栈暂存");
                }
                for (int i = static_cast<int>(decl.ctorArgs.size()) - 1; i >= 0 && i < 5; i--) {
                    static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
                    emit(std::format("popq %{}                   # 逆序弹出实参到寄存器", regs[i]));
                }
                emit(std::format("leaq {}(%rbp), %rdi       # this = 栈对象地址 &{}",
                    offset, decl.name));
                emit(std::format("callq {}               # 调用构造函数（带参重载）",
                    asmSymbol(decl.ctorSymbol)));
                return;
            }

            const std::string ctorSym = decl.ctorSymbol.empty()
                ? cit->second->name + "_" + cit->second->name   // 兜底（旧约定）
                : decl.ctorSymbol;
            emit(std::format("leaq {}(%rbp), %rdi       # this = 栈对象地址 &{}",
                offset, decl.name));
            emit(std::format("callq {}               # 调用构造函数",
                asmSymbol(ctorSym)));
            return;
        }
    }

    // ── 路径①-arr：数组 —— 按元素总尺寸对齐分配 ──
    if (decl.declaredType && decl.declaredType->isArray()) {
        uint32_t asz = decl.declaredType->sizeInBytes();
        if (asz < 8) asz = 8;
        asz = (asz + 7) / 8 * 8;
        m_currentStackOffset -= asz;
        int offset = m_currentStackOffset;
        m_localVars[decl.name] = offset;
        m_arrayLocals[decl.name] = offset;
        emitComment(std::format("stack array {} : {} ({} bytes)",
            decl.name, decl.declaredType->toString(), asz));
        for (uint32_t i = 0; i < asz; i += 8) {
            emit(std::format("movq $0, {}(%rbp)    # 零初始化数组 {} +{}",
                offset + static_cast<int>(i), decl.name, i));
        }
        return;
    }

    // ── 路径①：标量 —— 8 字节槽 ──
    m_currentStackOffset -= 8;
    m_localVars[decl.name] = m_currentStackOffset;

    if (decl.initializer) {
        emitComment(std::format("var {} = ...", decl.name));
        emitExpr(decl.initializer);

        // Upcast 指针调整：D* → B* 时需加偏移（多继承次基类）
        if (decl.declaredType && decl.declaredType->isPointer() &&
            decl.initializer->resolvedType && decl.initializer->resolvedType->isPointer()) {

            std::string baseName = decl.declaredType->pointeeType->name;
            std::string derivedName = decl.initializer->resolvedType->pointeeType->name;

            uint32_t adjust = getBaseOffset(derivedName, baseName);

            if (adjust > 0) {
                emitComment(std::format("upcast adjust: {} -> {} (+{} bytes)",
                    derivedName, baseName, adjust));
                emit(std::format("addq ${}, %rax          # 指针 upcast 偏移调整（+{} 字节）", adjust, adjust));
            }
        }

        emit(std::format("movq %rax, {}(%rbp)       # 存储到局部变量 {}",
            m_currentStackOffset, decl.name));
    } else {
        // 零初始化
        emit(std::format("movq $0, {}(%rbp)         # {} 零初始化",
            m_currentStackOffset, decl.name));
    }
}

// 赋值目标分别计算变量、成员或下标的写入地址；右值求值不得破坏目标地址。
void CodeGen::emitClassAddress(const ExprPtr& expr) {
    if (expr->kind == NodeKind::Var) {
        auto var = std::static_pointer_cast<VarExpr>(expr);
        //  引用变量 / 引用形参：它持有的就是被绑定对象的地址（类值即地址）
        auto rit = findRefLocal(var->name);
        if (rit) {
            emit(std::format("movq {}(%rbp), %rax    # 引用变量 → 被绑定对象的地址",
                rit->offset, var->name));
            return;
        }
        if (m_localVars.count(var->name)) {
            emit(std::format("leaq {}(%rbp), %rax    # 类对象地址 &{}（栈槽）",
                m_localVars[var->name], var->name));
            return;
        }
        // 方法体内的裸字段（this->成员，类类型）：局部表 miss 且当前类有该字段
        if (!m_currentClassName.empty() && m_currentClassType) {
            auto field = m_currentClassType->classLayout.findField(var->name);
            auto thisIt = m_localVars.find("this");
            if (field && thisIt != m_localVars.end()) {
                emit(std::format("movq {}(%rbp), %rax    # 加载 this 指针", thisIt->second));
                emit(std::format("leaq {}(%rax), %rax    # &this->{}（字段偏移 +{}）",
                    field->offset, var->name, field->offset));
                return;
            }
        }
        // 全局类对象
        emit(std::format("leaq {}(%rip), %rax    # 类对象地址 &{}（全局）",
            asmSymbol(var->name), var->name));
        return;
    }
    if (expr->kind == NodeKind::Member) {
        auto mem = std::static_pointer_cast<MemberExpr>(expr);
        TypePtr objType = mem->object->resolvedType;
        bool objIsPtr = objType && objType->isPointer();
        if (objIsPtr) {
            emitExpr(mem->object);            // 指针值本身就是对象地址
        } else {
            emitClassAddress(mem->object);    // 递归取子对象地址
        }
        // 字段偏移：Sema 回填优先（限定名成员 B21），否则查布局表
        int fieldOff = mem->resolvedFieldOffset;
        if (fieldOff < 0) {
            //  剥顶层 const/引用再查 —— const 对象的成员访问
            TypePtr cls = stripTopCvRef(objIsPtr ? objType->pointeeType : objType);
            if (cls && cls->isClass()) {
                auto f = cls->classLayout.findField(mem->memberName);
                if (f) fieldOff = static_cast<int>(f->offset);
            }
        }
        if (fieldOff > 0) {
            emit(std::format("leaq {}(%rax), %rax    # + 字段 .{} 偏移 {}", fieldOff, mem->memberName, fieldOff));
        }
        return;
    }
    // 其余形态：Sema 判据保证类值只来自 Var/Member/构造调用，落不到这里
    emitExpr(expr);
}

// [dcl.ref]：别名槽保存对象地址，标量读写经间接访问；类对象继续以地址表示。
// 引用形参是否按地址传由 isAliasBinding(type, false) 统一决定。
// `int& r = a` 后对 r 的赋值写回 a；const/右值引用形参仍有按值传递的边界。
// 参考：clang/lib/CodeGen/CGExpr.cpp:CodeGenFunction::EmitDeclRefLValue。
bool CodeGen::paramNeedsAddress(const FuncDeclPtr& fn, size_t argIndex) const {
    if (!fn || argIndex >= fn->parameters.size()) return false;
    return isAliasBinding(fn->parameters[argIndex].type, false);
}

// 查被调函数的声明（按已定符号名）；查不到返回 nullptr（调用点退回"全按值传"）。
FuncDeclPtr CodeGen::lookupCallee(const std::string& symbol) const {
    auto it = m_funcBySymbol.find(symbol);
    return it != m_funcBySymbol.end() ? it->second : nullptr;
}

// 标量别名读取被绑定对象的值；类引用直接返回对象地址，避免把首字段误作地址。
const CodeGen::RefLocalInfo* CodeGen::findRefLocal(const std::string& name) const {
    for (auto scopeIt = m_refScopes.rbegin(); scopeIt != m_refScopes.rend(); ++scopeIt) {
        const auto& scope = *scopeIt;          // 该层的表
        auto found = scope.find(name);
        if (found != scope.end()) return &found->second;
    }
    return nullptr;
}

void CodeGen::emitRefValue(const std::string& name, const RefLocalInfo& info) {
    TypePtr rt = info.referencedType;
    while (rt && rt->isConst()) rt = rt->innerType;
    if (rt && rt->isClass()) {
        emit(std::format("movq {}(%rbp), %rax    # 引用 {} → 被绑定对象的地址（类值即地址）",
            info.offset, name));
        return;
    }
    emit(std::format("movq {}(%rbp), %rcx    # 引用 {} → 取被绑定对象的地址",
        info.offset, name));
    emit("movq (%rcx), %rax              # 间接加载被绑定对象的值");
}

// 取一个左值的地址：rax = &obj。
// 局部标量 → leaq 槽；局部类对象 → leaq 槽；全局 → leaq 标签；
// 引用变量/形参 → 读它持有的地址；成员 → 对象地址 + 字段偏移；*p → 指针值本身。
void CodeGen::emitAddressOf(const ExprPtr& expr) {
    if (!expr) {
        emit("movq $0, %rax              # 无表达式（占位）");
        return;
    }
    if (expr->kind == NodeKind::Var) {
        auto var = std::static_pointer_cast<VarExpr>(expr);
        auto rit = findRefLocal(var->name);
        if (rit) {
            emit(std::format("movq {}(%rbp), %rax    # 引用 {} 持有的地址就是 &被绑定对象",
                rit->offset, var->name));
            return;
        }
        auto it = m_localVars.find(var->name);
        if (it != m_localVars.end()) {
            emit(std::format("leaq {}(%rbp), %rax    # &{}（局部槽）", it->second, var->name));
            return;
        }
        emit(std::format("leaq {}(%rip), %rax    # &{}（全局标签）",
            asmSymbol(var->name), var->name));
        return;
    }
    if (expr->kind == NodeKind::Member) {
        auto mem = std::static_pointer_cast<MemberExpr>(expr);
        TypePtr objType = mem->object->resolvedType;
        bool objIsPtr = objType && objType->isPointer();
        if (objIsPtr) {
            emitExpr(mem->object);             // 指针值本身就是对象地址
        } else {
            emitClassAddress(mem->object);     // 类对象 → 地址
        }
        int fieldOff = mem->resolvedFieldOffset;
        if (fieldOff < 0) {
            TypePtr cls = stripTopCvRef(objIsPtr && objType ? objType->pointeeType : objType);
            if (cls && cls->isClass()) {
                auto f = cls->classLayout.findField(mem->memberName);
                if (f) fieldOff = static_cast<int>(f->offset);
            }
        }
        if (fieldOff > 0) {
            emit(std::format("leaq {}(%rax), %rax    # + 字段 .{} 偏移 {}",
                fieldOff, mem->memberName, fieldOff));
        }
        return;
    }
    if (expr->kind == NodeKind::Unary
        && std::static_pointer_cast<UnaryExpr>(expr)->op == UnaryOp::Deref) {
        emitExpr(std::static_pointer_cast<UnaryExpr>(expr)->operand);  // 指针值本身就是地址
        return;
    }
    // 其余形态不可寻址（字面量 / 调用结果 / 容器下标）⇒ 由调用点先物化临时
    emitExpr(expr);
}

// 发一个实参。passByRef 为真 ⇒ 传【地址】（引用绑定）；为假 ⇒ 传【值】。
void CodeGen::emitArgument(const ExprPtr& arg, bool passByRef) {
    if (!passByRef) {
        emitExpr(arg);
        return;
    }
    if (isAddressableExpr(arg)) {
        emitAddressOf(arg);
        return;
    }
    // 要求地址传递的实参若无法寻址就诊断，不能静默改为传值。
    throw std::runtime_error(std::format(
        "[CodeGen Error] cannot bind non-addressable expression to a reference "
        "parameter (line {})", arg ? arg->location.line : 0));
}

void CodeGen::visit(AssignStmt& stmt) {
    // 类实例赋值按对象布局逐字节复制，由 Sema 的 isClassAssign 标记决定。
    // 右值可为同类变量、成员子对象或构造临时对象；临时对象在复制后析构。
    if (stmt.isClassAssign) {
        uint32_t size = 8;
        if (m_classTypes) {
            auto cit = m_classTypes->find(stmt.assignClassType->name);
            if (cit != m_classTypes->end()) size = cit->second->classLayout.totalSize;
        }
        emitComment(std::format("class assign: {} ({} bytes)",
            stmt.assignClassType->name, size));

        // 右值 → 源地址（rsi）
        bool rhsIsTemp = stmt.value->kind == NodeKind::Call
            && std::static_pointer_cast<CallExpr>(stmt.value)->isCtorCall;
        if (rhsIsTemp) m_suppressTempDtor++;
        if (rhsIsTemp || stmt.value->kind == NodeKind::Member) {
            emitExpr(stmt.value);            // 临时对象 / 子对象：rax = 地址
        } else {
            emitClassAddress(stmt.value);    // 同类 Var：取址
        }
        if (rhsIsTemp) m_suppressTempDtor--;
        emit("movq %rax, %rsi            # 源对象地址");

        // 左值 → 目标地址（rdi）
        emitClassAddress(stmt.target);
        emit("movq %rax, %rdi            # 目标对象地址");

        // 逐字节内联复制（rep movsb）——自研链接器只内置 malloc/free，memcpy 无定义可绑
        emit(std::format("movq ${}, %rcx           # 复制字节数 = sizeof({})",
            size, stmt.assignClassType->name));
        emit("rep movsb                  # 类实例赋值：逐字节复制对象表示");

        // 构造临时对象在拷贝完成后析构（全表达式结束，[class.dtor]/2）
        if (rhsIsTemp) {
            emitClassDtorCall(m_lastCtorTempClass, m_lastCtorTempOffset);
        }
        return;
    }

    // 计算右值到 rax
    emitExpr(stmt.value);

    // 赋值目标有三态：① 下标 v[i]（糖化成 v.set(i,value) 调用）
    // ② 裸名（局部变量 / 裸字段 / 全局变量，按作用域优先级）
    // ③ 成员访问 o.f（按 ClassLayout 查到偏移量后直接写内存）。
    // 三者互斥 —— 按节点种类一次 switch，跳表分派替代 RTTI 试探链。
    switch (stmt.target->kind) {
        case NodeKind::Unary: {
            // [expr.ass]：`*p = v` 按所指类型宽度写入；求地址时需保护已求出的右值。
            auto un = std::static_pointer_cast<UnaryExpr>(stmt.target);
            if (un->op != UnaryOp::Deref) {
                throw std::runtime_error(std::format(
                    "[CodeGen Error] unsupported unary assignment target (line {})",
                    stmt.target->location.line));
            }
            emitComment("*p = ...  (解引用左值，[expr.ass]/3)");
            emit("pushq %rax                  # 暂存右值（待写入的值）到栈上");
            emitExpr(un->operand);
            emit("movq %rax, %rcx                # 目标地址存入 rcx");
            emit("popq %rax                    # 弹出右值回 rax");
            // 宽度按被指类型选（与写字段、读解引用同一条判据，避免踩坏邻居）
            TypePtr pt = un->operand->resolvedType;
            TypePtr pointee = (pt && pt->isPointer()) ? pt->pointeeType : nullptr;
            if (pointee && pointee->isBool()) {
                emit("movb %al, (%rcx)             # 按 1 字节写入");
            } else if (pointee && pointee->isInt()) {
                emit("movl %eax, (%rcx)            # 按 4 字节写入");
            } else {
                emit("movq %rax, (%rcx)            # 按 8 字节写入");
            }
        } break;
        case NodeKind::Index: {
            auto idx = std::static_pointer_cast<IndexExpr>(stmt.target);
            TypePtr objType = idx->object->resolvedType;
            if (objType && objType->isReference()) objType = objType->referencedType;
            bool isRaw = objType && (objType->isArray() || objType->isPointer());

            if (isRaw) {
                // 原生数组/指针下标赋值：a[i] = rhs
                emitComment("raw array/pointer assign a[i] = rhs");
                emit("pushq %rax                  # 暂存右值到栈上");
                emitExpr(idx->object);
                emit("pushq %rax                  # 暂存基址到栈上");
                emitExpr(idx->index);
                emit("popq %rcx                   # 弹出基址到 rcx");
                TypePtr elemType = objType->isArray() ? objType->elementType : objType->pointeeType;
                uint32_t elemSize = elemType ? elemType->sizeInBytes() : 8;
                if (elemSize == 1) {
                    emit("leaq (%rcx, %rax, 1), %rcx # 目标地址 = base + i * 1");
                } else if (elemSize == 2) {
                    emit("leaq (%rcx, %rax, 2), %rcx # 目标地址 = base + i * 2");
                } else if (elemSize == 4) {
                    emit("leaq (%rcx, %rax, 4), %rcx # 目标地址 = base + i * 4");
                } else if (elemSize == 8) {
                    emit("leaq (%rcx, %rax, 8), %rcx # 目标地址 = base + i * 8");
                } else {
                    emit(std::format("imulq ${}, %rax               # i * elemSize", elemSize));
                    emit("addq %rax, %rcx                 # 目标地址 = base + 偏移");
                }
                emit("popq %rax                   # 弹出右值到 rax");
                if (elemSize <= 1) {
                    emit("movb %al, (%rcx)             # 写入 1 字节");
                } else if (elemSize <= 4) {
                    emit("movl %eax, (%rcx)            # 写入 4 字节");
                } else {
                    emit("movq %rax, (%rcx)            # 写入 8 字节");
                }
                break;
            }

            // ─── 下标赋值（写形态）→ 糖化为 v.set(i, value) ───
            // 读走 at()、写走 set()，是 at()/set() 约定的另一半
            // （[expr.ass] 左值语义的降级：赋值目标必须是函数调用形态）。
            // 此刻 rax 已是右值（函数开头统一求值）：压栈保序，
            // 依次取 this 与下标，再按 System V 装参 rdi/rsi/rdx。
            emitComment("subscript assign v[i] = ... → desugar to v.set(i, value)");
            objType = idx->object->resolvedType;
            if (objType && objType->isPointer()) {
                objType = objType->pointeeType;
            }
            objType = stripTopCvRef(objType);   //  const 容器的下标写
            std::string className = (objType && objType->isClass()) ? objType->name : "";

            emit("pushq %rax                  # 暂存右值（待写入的值）到栈上");
            emitExpr(idx->index);
            emit("pushq %rax                  # 暂存下标值到栈上");
            emitExpr(idx->object);
            emit("movq %rax, %rdi             # this = 容器对象地址（第 0 参数）");
            emit("popq %rsi                   # arg1: 弹出下标 i 到 rsi");
            emit("popq %rdx                   # arg2: 弹出右值 value 到 rdx");
            //  符号取 Sema 回填值 —— 不能硬拼 `类名_set`：Sema 给带参成员方法名
            // 加了"参数个数"后缀（IntVec_set_2），硬拼得到的是 IntVec_set ⇒
            // 链接期 undefined reference。
            // 宽度仍按"硬拼时的旧名字"算 ⇒ 未回填路径的列对齐逐字节不变。
            std::string setSymbol = idx->setSymbol.empty()
                                  ? className + "_set" : idx->setSymbol;
            emit(std::format("callq {:<{}}# 调用 v.set(i, value) 完成写入",
                             setSymbol, 18 + className.size()));
        } break;
        case NodeKind::Var: {
            auto var = std::static_pointer_cast<VarExpr>(stmt.target);
            //  引用赋值：写的是【被绑定对象】，不是引用自己的槽
            // （[dcl.ref]/1：引用是别名）。槽里存的是地址 ⇒ 间接存储。
            auto rit = findRefLocal(var->name);
            if (rit) {
                emit(std::format("movq {}(%rbp), %rcx    # 取引用 {} 持有的地址",
                    rit->offset, var->name));
                emit("movq %rax, (%rcx)           # 写被绑定对象");
                break;
            }
            //  裸字段名赋值：方法体内写 age = a; 等价于 this->age = a;
            // （与 emitVar 的"裸字段读"路径对称）。必须在全局兜底之前检查，
            // 否则降级成 movq %rax, age(%rip) 全局写 → 链接期 undefined reference。
            // 注意顺序：局部表优先（this 指针也登记在 m_localVars），
            // 之后才是字段，最后才轮到全局。
            bool fieldHandled = false;
            if (m_localVars.find(var->name) == m_localVars.end()
                && !m_currentClassName.empty() && m_currentClassType) {
                auto thisIt = m_localVars.find("this");
                // 限定字段赋值优先使用 Sema 回填的偏移和宽度，避免命中被隐藏的同名字段。
                if (var->resolvedFieldOffset >= 0 && thisIt != m_localVars.end()) {
                    emit("pushq %rax                  # 暂存右值到栈上");
                    emit(std::format("movq {}(%rbp), %rax    # 加载 this 指针", thisIt->second));
                    emit("movq %rax, %rcx                # this 地址存入 rcx");
                    emit("popq %rax                    # 弹出右值回 rax");
                    if (var->resolvedFieldSize == 1) {
                        emit(std::format("movb %al, {}(%rcx)    # .{} = ...（偏移 {}，1 字节写入）",
                            var->resolvedFieldOffset, var->name, var->resolvedFieldOffset));
                    } else if (var->resolvedFieldSize <= 4) {
                        emit(std::format("movl %eax, {}(%rcx)    # .{} = ...（偏移 {}，4 字节写入）",
                            var->resolvedFieldOffset, var->name, var->resolvedFieldOffset));
                    } else {
                        emit(std::format("movq %rax, {}(%rcx)    # .{} = ...（偏移 {}，8 字节写入）",
                            var->resolvedFieldOffset, var->name, var->resolvedFieldOffset));
                    }
                    fieldHandled = true;
                }
                auto field = fieldHandled
                           ? nullptr : m_currentClassType->classLayout.findField(var->name);
                if (field && thisIt != m_localVars.end()) {
                    // rax 已是右值（emitAssign 开头统一求值）：压栈暂存 →
                    // 取 this → 弹出右值 → 写入 [this + offset]
                    emit("pushq %rax                  # 暂存右值到栈上");
                    emit(std::format("movq {}(%rbp), %rax    # 加载 this 指针", thisIt->second));
                    emit("movq %rax, %rcx                # this 地址存入 rcx");
                    emit("popq %rax                    # 弹出右值回 rax");
                    // 字段按实际宽度选择 movb/movl/movq，指针与 long 等 8 字节字段不得截断。
                    // 参考：clang/lib/CodeGen/CGExpr.cpp:CodeGenFunction::EmitStoreOfScalar。
                    if (field->size == 1) {
                        emit(std::format("movb %al, {}(%rcx)    # .{} = ...（偏移 {}，1 字节写入）",
                            field->offset, var->name, field->offset));
                    } else if (field->size <= 4) {
                        emit(std::format("movl %eax, {}(%rcx)    # .{} = ...（偏移 {}，4 字节写入）",
                            field->offset, var->name, field->offset));
                    } else {
                        emit(std::format("movq %rax, {}(%rcx)    # .{} = ...（偏移 {}，8 字节写入）",
                            field->offset, var->name, field->offset));
                    }
                    fieldHandled = true;
                }
            }
            if (!fieldHandled) {
                auto it = m_localVars.find(var->name);
                if (it != m_localVars.end()) {
                    emit(std::format("movq %rax, {}(%rbp)       # 赋值局部变量 {}",
                        it->second, var->name));
                } else {
                    // 全局变量赋值（asmSymbol 净化 :: 等非法标签字符，
                    // 与 .data 段标签发射端一致）
                    emit(std::format("movq %rax, {}(%rip)       # 全局变量 {} = ...",
                        asmSymbol(var->name), var->name));
                }
            }
        } break;
        case NodeKind::Member: {
            auto mem = std::static_pointer_cast<MemberExpr>(stmt.target);
            // ─── static 数据成员写：obj.count = v / obj.A::count = v ───
            // 与读路径对称：直接写全局符号，不碰对象（赋值目标语境里对象
            // 表达式不求值——[expr.ass] 的左值不需要对象值，static 成员尤其
            // 如此：它没有 this 语义）。rax 已是右值（函数开头统一求值）。
            if (!mem->resolvedStaticSymbol.empty()) {
                emitComment(std::format("static member assign: .{} = ...", mem->memberName));
                emit(std::format("movq %rax, {}(%rip)       # 静态成员 {} = ...（全局符号）",
                    asmSymbol(mem->resolvedStaticSymbol), mem->memberName));
                return;
            }
            // ─── 字段访问的消除 ───
            // 将 obj.field = value 转化为 [objAddr + fieldOffset] = value
            emitComment(std::format("member assign: .{} = ...", mem->memberName));

            // 先计算对象地址到 rcx
            emitExpr(mem->object);
            emit("movq %rax, %rcx                # 对象地址存入 rcx");

            // 右值求值及调用可覆盖 rcx，须保护已算出的成员地址再写回。
            emit("pushq %rcx                   # 保护对象地址（右值求值会踩 rcx）");
            emitExpr(stmt.value);
            emit("popq %rcx                    # 对象地址回 rcx");

            // 查找字段偏移量
            if (mem->object->resolvedType) {
                TypePtr objType = mem->object->resolvedType;
                if (mem->isArrow && objType->isPointer()) {
                    objType = objType->pointeeType;
                }
                objType = stripTopCvRef(objType);   //  const 对象的成员写
                if (objType->isClass()) {
                    //  限定名成员 `obj.Q::m = ...`：Sema 回填的偏移优先。
                    if (mem->resolvedFieldOffset >= 0) {
                        if (mem->resolvedFieldSize <= 4) {
                            emit(std::format("movl %eax, {}(%rcx)    # 写入限定名成员 .{}（偏移 +{}）",
                                mem->resolvedFieldOffset, mem->memberName, mem->resolvedFieldOffset));
                        } else {
                            emit(std::format("movq %rax, {}(%rcx)    # 写入限定名成员 .{}（偏移 +{}，8B）",
                                mem->resolvedFieldOffset, mem->memberName, mem->resolvedFieldOffset));
                        }
                        return;
                    }
                    auto field = objType->classLayout.findField(mem->memberName);
                    if (field) {
                        // 成员写入使用实际字段宽度；8 字节指针/long 不能用 movl 截掉高位。
                        if (field->size <= 4) {
                            emit(std::format("movl %eax, {}(%rcx)    # 写入字段 .{}（偏移 +{}）",
                                field->offset, mem->memberName, field->offset));
                        } else {
                            emit(std::format("movq %rax, {}(%rcx)    # 写入字段 .{}（偏移 +{}，8B）",
                                field->offset, mem->memberName, field->offset));
                        }
                        return;
                    }
                }
            }

            // 如果找不到偏移量，生成通用代码
            emit("movq %rax, (%rcx)              # 成员赋值（偏移未知，默认偏移 0）");
        } break;
        default:
            break;
    }
}

// 普通返回值放入 rax；类值返回复制到隐藏缓冲，引用返回不走 sret。
// 返回前的局部析构不能破坏已算好的返回结果。
void CodeGen::visit(ReturnStmt& stmt) {
    if (stmt.value) {
        emitComment("return expr");
        if (m_fnReturnsClassByValue) {
            //  sret：按值返回类对象 ⇒ 把对象表示【拷进调用者给的返回缓冲】，
            // 并按约定把缓冲地址留在 rax（Itanium 的 sret 返回值就是缓冲地址）。
            // 拷贝必须发生在【析构之前】—— 返回对象先落到缓冲，局部对象再死
            // （emitDtorsOnReturn 在下面），顺序反了就是把死对象交出去。
            emitClassAddress(stmt.value);        // rax = 源对象地址
            emit("movq %rax, %rsi            # 源对象地址");
            emit(std::format("movq {}(%rbp), %rdi   # 目标 = 调用者的返回缓冲",
                m_sretPtrSlot));
            emit(std::format("movq ${}, %rcx         # 复制字节数 = sizeof(返回类型)",
                m_sretValueSize));
            emit("rep movsb                  # 逐字节复制对象表示到返回缓冲");
            emit(std::format("movq {}(%rbp), %rax   # sret 约定：返回值 = 返回缓冲地址",
                m_sretPtrSlot));
        } else {
            emitExpr(stmt.value);
            // 返回值已经在 rax 中
        }
    } else {
        emit("movq $0, %rax               # void 返回，结果置 0");
    }
    emitDtorsOnReturn();   //  return 是提前出口，析构必须在这里补
    emit("leave                         # 恢复栈帧（movq %rbp,%rsp; popq %rbp）");
    emit("ret                           # 返回调用者（从栈上弹出返回地址）");
}

// ─────────────────────────────────────────────────────────────────────────────
// delete 语句：析构（vtableEntries 有 "dtor" ⇒ 虚析构走三部曲，否则静态 callq）
//   + callq free。对应真 C++ 的 delete = 析构 + operator delete。
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::visit(DeleteStmt& stmt) {
    emitComment("delete pointer");
    emitExpr(stmt.pointerExpr);
    emit("movq %rax, %rdi             # 待删除指针传入第 0 参数寄存器");
    emit("pushq %rdi                  # 暂存指针（析构后还要 free）");

    // 检查是否需要调用析构函数
    TypePtr ptrType = stmt.pointerExpr->resolvedType;
    if (ptrType && ptrType->isPointer() && ptrType->pointeeType && ptrType->pointeeType->isClass()) {
        std::string className = ptrType->pointeeType->name;
        if (m_classTypes) {
            auto it = m_classTypes->find(className);
            if (it != m_classTypes->end()) {
                auto& layout = it->second->classLayout;
                bool hasVirtualDtor = false;
                uint32_t dtorIndex = 0;
                for (auto& entry : layout.vtableEntries) {
                    if (isDtorSlot(entry)) {
                        hasVirtualDtor = true;
                        dtorIndex = entry.index;
                        break;
                    }
                }
                if (hasVirtualDtor) {
                    emitVirtualCall(className, "dtor", {}, dtorIndex);
                } else {
                    emit(std::format("callq {}           # 静态调用析构函数",
                        asmSymbol(className + "_dtor")));
                }
            }
        }
    }

    emit("popq %rdi                   # 恢复指针准备 free");
    emit("callq free                  # 释放堆内存");
}

// if 用条件跳转连接 then/else 与汇合标签；无 else 时直接跳到汇合点。
void CodeGen::visit(IfStmt& stmt) {
    std::string elseLabel = newLabel("else");
    std::string endLabel = newLabel("endif");

    emitComment("if condition");
    emitExpr(stmt.condition);
    emit("testq %rax, %rax             # 条件值与自身按位与，设置 ZF 标志位");
    emit(std::format("je {}                     # 条件为假（ZF=1）跳转到 else/endif", stmt.elseBranch ? elseLabel : endLabel));

    emitComment("then branch");
    emitStmt(stmt.thenBranch);

    if (stmt.elseBranch) {
        emit(std::format("jmp {}                    # then 分支结束，无条件跳转到 endif", endLabel));
        emit(std::format("{}:                        # else 分支标签", elseLabel));
        emitComment("else branch");
        emitStmt(stmt.elseBranch);
    }

    emit(std::format("{}:                        # endif 标签", endLabel));
}

// while 的条件标签也是 continue 目标；break 跳到循环出口。
void CodeGen::visit(WhileStmt& stmt) {
    std::string beginLabel = newLabel("while_begin");
    std::string endLabel = newLabel("while_end");

    // 循环上下文入栈：continue 跳回条件式（while 的"增量步"就是它）
    m_loopStack.push_back({endLabel, beginLabel});

    emit(std::format("{}:                        # while 循环开始标签", beginLabel));
    emitComment("while condition");
    emitExpr(stmt.condition);
    emit("testq %rax, %rax             # 条件值与自身按位与，设置 ZF 标志位");
    emit(std::format("je {}                     # 条件为假（ZF=1）跳出循环", endLabel));

    emitComment("while body");
    emitStmt(stmt.body);
    emit(std::format("jmp {}                    # 无条件跳回循环开始（回边）", beginLabel));

    emit(std::format("{}:                        # while 循环结束标签", endLabel));
    m_loopStack.pop_back();
}

// for 循环降级（[stmt.for] 的结构化翻译，对照 clang/lib/CodeGen/CGStmt.cpp:CodeGenFunction::EmitForStmt）：
//   init 恰好一次 → cond 测试（假则去 end）→ body → incr → 回边 jmp cond。
// 五个标签各司其职，continue 落 incr（不是 cond ——  incr 还没跑），break 落 end。
void CodeGen::visit(ForStmt& stmt) {
    std::string initLabel  = newLabel("for_init");
    std::string condLabel  = newLabel("for_cond");
    std::string bodyLabel  = newLabel("for_body");
    std::string incrLabel  = newLabel("for_incr");
    std::string endLabel   = newLabel("for_end");

    m_loopStack.push_back({endLabel, incrLabel});

    emit(std::format("{}:                        # for 初始化（只执行一次）", initLabel));
    emitStmt(stmt.init);

    emit(std::format("{}:                        # for 条件测试", condLabel));
    if (stmt.condition) {
        emitExpr(stmt.condition);
        emit("testq %rax, %rax             # 条件值与自身按位与，设置 ZF 标志位");
        emit(std::format("je {}                     # 条件为假（ZF=1）跳出循环", endLabel));
    } else {
        emitComment("for(;;)：无条件 —— 死循环，只能靠 break 出口");
    }

    emit(std::format("{}:                        # for 循环体", bodyLabel));
    emitStmt(stmt.body);

    emit(std::format("{}:                        # for 增量步（continue 落点）", incrLabel));
    if (stmt.increment && stmt.increment->kind != NodeKind::Empty) emitStmt(stmt.increment);
    emit(std::format("jmp {}                    # 回边：无条件跳回条件测试", condLabel));

    emit(std::format("{}:                        # for 循环结束标签（break 落点）", endLabel));
    m_loopStack.pop_back();
}

// do-while [stmt.do]：体【先于条件执行一轮】 —— 条件测试代码放在体之后。
// continue 落条件式（与 while 相同；for 的增量步约定不适用）。
void CodeGen::visit(DoWhileStmt& stmt) {
    std::string bodyLabel = newLabel("do_body");
    std::string condLabel = newLabel("do_cond");
    std::string endLabel  = newLabel("do_end");

    // continue → 条件式；break → 出口
    m_loopStack.push_back({endLabel, condLabel});

    emit(std::format("{}:                        # do-while 体（无条件先执行一轮）", bodyLabel));
    emitStmt(stmt.body);

    emit(std::format("{}:                        # do-while 条件测试（continue 落点）", condLabel));
    emitExpr(stmt.condition);
    emit("testq %rax, %rax             # 条件值与自身按位与，设置 ZF 标志位");
    emit(std::format("jne {}                    # 条件为真回边继续循环", bodyLabel));

    emit(std::format("{}:                        # do-while 出口（break 落点）", endLabel));
    m_loopStack.pop_back();
}

// switch [stmt.switch]：比较链降级。条件值压栈保存，逐条 cmp-je 分发到
// 各 case 标号；无匹配落 default（无 default 直落出口）。case 标号按体
// 内语句顺序【顺序放置】—— 不穿 break 的 case 自然贯穿到下一条 case 的
// 代码（这正是 C 的 fall-through 语义，对照 clang 的 LowerSwitch 之
// 比较链形态；跳表形态见 ROADMAP 选做）。
void CodeGen::visit(SwitchStmt& stmt) {
    std::string endLabel = newLabel("sw_end");

    // ── 第一遍：收集体里【直接子语句】层的 case/default，分配标号 ──
    // 嵌套块里的 case 属于内层 switch（若那里没有 switch 则是 C++ 里
    // 把外层标号写进了嵌套块 —— 罕见写法，本实现不支持分发到它，文档注明）。
    std::vector<CaseStmt*>  cases;
    DefaultStmt*            def    = nullptr;
    auto collect = [&](StmtPtr& s) {
        if (s && s->kind == NodeKind::Case) {
            auto c = std::static_pointer_cast<CaseStmt>(s).get();
            c->asmLabel = newLabel("sw_case");
            cases.push_back(c);
        } else if (s && s->kind == NodeKind::Default) {
            auto d = std::static_pointer_cast<DefaultStmt>(s).get();
            d->asmLabel = newLabel("sw_def");
            def = d;
        }
    };
    if (stmt.body->kind == NodeKind::Block) {
        for (auto& s : std::static_pointer_cast<BlockStmt>(stmt.body)->statements)
            collect(s);
    } else {
        collect(stmt.body);
    }

    // ── 条件求值 → 栈顶暂存（比较链要反复取用）──
    emitExpr(stmt.condition);
    emit("pushq %rax                # switch 条件值暂存栈顶");

    // ── 分发链：cmp-je 逐条比对 ──
    for (auto* c : cases) {
        emit("movq (%rsp), %rax         # 取回条件值");
        emit(std::format("cmpq ${}, %rax          # 与 case 常量比对", c->constValue));
        emit(std::format("je {}                  # 命中 ⇒ 落入该 case", c->asmLabel));
    }
    if (def) emit(std::format("jmp {}                    # 无匹配 ⇒ default", def->asmLabel));
    else     emit(std::format("jmp {}                    # 无匹配且无 default ⇒ 出口", endLabel));

    // break 在 switch 内合法：压 {end, end}。continue 到不了这里
    // （Sema 只在 m_loopDepth>0 时放行 continue，内层循环自管自的栈）。
    m_loopStack.push_back({endLabel, endLabel});
    emitStmt(stmt.body);
    m_loopStack.pop_back();

    emit(std::format("{}:                        # switch 出口（先清暂存再落点）", endLabel));
    emit("addq $8, %rsp             # 释放条件值暂存槽");
}

void CodeGen::visit(CaseStmt& stmt) {
    // 标号在 visit(SwitchStmt) 第一遍已分配（asmLabel）
    emit(std::format("{}:                        # case {}", stmt.asmLabel, stmt.constValue));
    emitStmt(stmt.body);
}

void CodeGen::visit(DefaultStmt& stmt) {
    emit(std::format("{}:                        # default", stmt.asmLabel));
    emitStmt(stmt.body);
}

// break [stmt.jump]：跳最近一层循环的出口。栈空不可能出现 —— Sema 已
// 把"not in loop" 拦在编译期；这里再守一次（防御 Sema 守卫被意外绕过的
// UB 场景：栈空时 back() 是未定义行为，宁可响亮报错不可静默崩）。
void CodeGen::visit(BreakStmt&) {
    if (m_loopStack.empty()) {
        throw std::runtime_error(
            "[CodeGen Error] 'break' 无匹配的循环上下文（Sema 的循环守卫未生效？）");
    }
    emit(std::format("jmp {}                    # break：跳出最近一层循环", m_loopStack.back().breakLabel));
}

// continue [stmt.cont]：跳过本轮剩余体代码，跳到循环的"增量步"
// （for 的 incr 标签；while 那里是条件式标签 —— 由入栈时的约定区分）。
void CodeGen::visit(ContinueStmt&) {
    if (m_loopStack.empty()) {
        throw std::runtime_error(
            "[CodeGen Error] 'continue' 无匹配的循环上下文（Sema 的循环守卫未生效？）");
    }
    emit(std::format("jmp {}                    # continue：跳转到循环增量步", m_loopStack.back().continueLabel));
}

// 表达式语句：只求值、结果（rax）丢弃；价值在求值过程产生的副作用
// （典型如 ptr->speak() 这类调用语句）。
void CodeGen::visit(ExprStmt& stmt) {
    emitExpr(stmt.expr);
}

// 表达式统一将结果放入 rax；类对象结果是地址，普通标量结果是值。
void CodeGen::emitExpr(const ExprPtr& expr) {
    if (!expr) return;
    // 同上：accept 虚表分派。
    // 注意 NullptrLiteralExpr 在此就地发射（无独立 helper），故在下面以
    // visit(NullptrLiteralExpr&) 的形式保留 —— 改造前它也在这条链的末尾。
    expr->accept(*this);
}

// nullptr 字面量：x86 惯用自异或清零（比 movq $0 少一个字节且更快）
void CodeGen::visit(NullptrLiteralExpr&) {
    emit("xorq %rax, %rax             # nullptr = 0（x86 惯用自异或清零）");
}

// 整数字面量：立即数直接进 rax。
// demo: 42 → movq $42, %rax
void CodeGen::visit(IntLiteralExpr& expr) {
    emit(std::format("movq ${}, %rax           # 整数字面量载入 rax", expr.value));
}

// 字符字面量：值与整数字面量同形（立即数进 rax），差别在【类型是 char】——
// 落到存储点时按 1 字节写（宽度分派由声明/字段类型决定，见 emitStore）。
// demo: 'a' → movq $97, %rax
void CodeGen::visit(CharLiteralExpr& expr) {
    emit(std::format("movq ${}, %rax           # 字符字面量（char, 1 字节）", expr.value));
}

// 布尔字面量：本项目 bool 按整数 0/1 表示，
// 与比较运算 setcc/movzbq 的产出形式天然一致。
// demo: true → movq $1, %rax      false → movq $0, %rax
void CodeGen::visit(BoolLiteralExpr& expr) {
    emit(std::format("movq ${}, %rax           # 布尔字面量",
        expr.value ? 1 : 0));
}

// 字符串字面量：登记进常量池取标签，再用 RIP 相对寻址取地址。
// leaq str_N(%rip),%rax 是位置无关的地址计算：目标 = 本条指令的 RIP + 汇编器算好的
// 偏移，由重定位在汇编/链接期填回，代码段无需知道绝对地址。
// demo: "hello" → .rodata 中 str_0: .string "hello"
//                 此处发射 leaq str_0(%rip), %rax
void CodeGen::visit(StringLiteralExpr& expr) {
    std::string label = addStringLiteral(expr.value);
    emit(std::format("leaq {}(%rip), %rax      # 加载字符串字面量地址（PIC）", label));
}

// 变量访问优先识别类对象和引用别名，再处理普通局部/全局存储。
// 类对象返回地址；别名间接读取；标量加载值。
void CodeGen::visit(VarExpr& expr) {
    //  引用变量 / 引用形参：值 = 被绑定对象的值（[dcl.ref]/1：引用是别名）。
    // 槽里放的是地址 ⇒ 读要间接加载（类引用例外：类值即地址，见 emitRefValue）。
    // 必须排在普通局部加载【之前】：同名槽里装的是地址不是值。
    auto rit = findRefLocal(expr.name);
    if (rit) {
        emitRefValue(expr.name, *rit);
        return;
    }

    //  栈上类对象：值语义 = 对象地址（与指针表达式的值一致）——
    // 后续 emitMember/emitCall 对 "." 的约定就是"地址已在 rax"，
    // 用 leaq 取址即可复用整套成员访问代码，无需解引用。
    // （必须放在普通局部加载之前：类对象槽里存的是对象内容首 8 字节，
    // 直接加载会得到 _vptr 而非对象地址——虚调用碰巧也能跑，
    // 但字段访问与析构取址会错，统一按地址语义发射才自洽。）
    auto ci = m_classLocals.find(expr.name);
    if (ci != m_classLocals.end()) {
        emit(std::format("leaq {}(%rbp), %rax    # 取栈对象地址 &{}",
            ci->second.offset, expr.name));
        return;
    }

    // 局部数组：decay 为首元素地址
    auto ai = m_arrayLocals.find(expr.name);
    if (ai != m_arrayLocals.end()) {
        emit(std::format("leaq {}(%rbp), %rax    # 局部数组 {} decay 为首元素地址",
            ai->second, expr.name));
        return;
    }

    // 先查局部变量
    auto it = m_localVars.find(expr.name);
    if (it != m_localVars.end()) {
        emit(std::format("movq {}(%rbp), %rax    # 加载局部变量 {} 到 rax",
            it->second, expr.name));
        return;
    }

    // 如果在类方法中，查类字段（通过 this 指针访问）
    if (!m_currentClassName.empty() && m_currentClassType) {
        auto thisIt = m_localVars.find("this");
        // 限定字段读取优先使用 Sema 回填位置，避免按裸名误命中派生类隐藏字段。
        if (expr.resolvedFieldOffset >= 0 && thisIt != m_localVars.end()) {
            emit(std::format("movq {}(%rbp), %rax    # 加载 this 指针", thisIt->second));
            emit(std::format("movl {}(%rax), %eax    # 读取限定名成员 .{}（偏移 +{} 字节）",
                expr.resolvedFieldOffset, expr.name, expr.resolvedFieldOffset));
            return;
        }
        auto field = m_currentClassType->classLayout.findField(expr.name);
        if (field) {
            // 通过 this 指针访问字段
            if (thisIt != m_localVars.end()) {
                emit(std::format("movq {}(%rbp), %rax    # 加载 this 指针", thisIt->second));
                if (field->type && field->type->isArray()) {
                    emit(std::format("leaq {}(%rax), %rax    # 数组字段 .{} 取首地址（偏移 +{} 字节）",
                        field->offset, expr.name, field->offset));
                } else if (field->type && field->type->isClass()) {
                    emit(std::format("leaq {}(%rax), %rax    # 嵌套类字段 .{} 取地址（偏移 +{} 字节）",
                        field->offset, expr.name, field->offset));
                } else {
                    emit(std::format("movl {}(%rax), %eax    # 读取字段 .{}（偏移 +{} 字节）",
                        field->offset, expr.name, field->offset));
                }
                return;
            }
        }
    }

    // 全局数组：decay 为首元素地址
    if (m_arrayGlobals.count(expr.name)) {
        emit(std::format("leaq {}(%rip), %rax    # 全局数组 {} decay 为首元素地址（RIP 相对寻址）",
            asmSymbol(expr.name), expr.name));
        return;
    }

    // 查全局变量或常量符号（RIP 寻址；asmSymbol 净化 :: 等非法标签字符）
    emit(std::format("movq {}(%rip), %rax    # 加载全局变量 {} 到 rax（RIP 相对寻址）",
        asmSymbol(expr.name), expr.name));
}

// 二元运算需保留左操作数，避免右操作数求值覆盖 rax。
// 比较产生布尔值；短路逻辑由专用分支保证右操作数按需求值。
void CodeGen::visit(BinaryExpr& expr) {
    emitComment("binary expr");

    // 左操作数 → rax → 压栈
    emitExpr(expr.left);
    emit("pushq %rax                  # 左操作数压栈暂存（求右值会覆盖 rax）");

    // 右操作数 → rax → rcx
    emitExpr(expr.right);
    emit("movq %rax, %rcx             # 右操作数从 rax 转移到 rcx");

    // 恢复左操作数到 rax
    emit("popq %rax                   # 弹出左操作数回到 rax");

    switch (expr.op) {
        case BinaryOp::Add:
            emit("addq %rcx, %rax             # 加法：rax = rax + rcx");
            break;
        case BinaryOp::Sub:
            emit("subq %rcx, %rax             # 减法：rax = rax - rcx");
            break;
        case BinaryOp::Mul:
            emit("imulq %rcx, %rax            # 乘法：rax = rax * rcx（有符号）");
            break;
        // ── 除法族 ──
        // x86 的 idivq %rcx 计算的是 128 位被除数 rdx:rax ÷ rcx：
        // 商 → rax，余数 → rdx。cqto 先把 rax 符号扩展到 rdx:rax，
        // 否则 rdx 的垃圾值会毁掉被除数（经典陷阱）。
        case BinaryOp::Div:
            emit("cqto                        # 符号扩展 rax → rdx:rax（128 位被除数）");
            emit("idivq %rcx                  # 有符号除法：rax=商，rdx=余数");
            break;
        case BinaryOp::Mod:
            emit("cqto                        # 符号扩展 rax → rdx:rax（128 位被除数）");
            emit("idivq %rcx                  # 有符号除法：rax=商，rdx=余数");
            emit("movq %rdx, %rax             # 取模：结果在 rdx，移回 rax");
            break;
        // ── 比较族 ──
        // 三步缺一不可：cmpq 做减法只置标志位 → setcc 把标志写成 8 位
        // 0/1（setcc 只能写 %al 等 8 位寄存器）→ movzbq 零扩展回 64 位，
        // 与"结果统一在 rax"的约定对齐。
        case BinaryOp::Eq:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("sete %al                    # 相等时 al = 1（ZF=1）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Neq:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setne %al                   # 不等时 al = 1（ZF=0）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Lt:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setl %al                    # 小于时 al = 1（SF≠OF）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Gt:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setg %al                    # 大于时 al = 1（ZF=0 且 SF=OF）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Le:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setle %al                   # 小于等于时 al = 1（ZF=1 或 SF≠OF）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Ge:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setge %al                   # 大于等于时 al = 1（SF=OF）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        // ── 逻辑与/或 ──
        // 本实现直接按位与/或（要求操作数已规范化为 0/1，比较指令恰好
        // 保证这一点）。 理论点：这放弃了短路求值（short-circuit）——
        // 真正的 &&/|| 必须用条件跳转实现"左值为假则不算右值"，
        // 其降级形状等价于两个嵌套 if（可作课后练习）。
        case BinaryOp::And:
            emit("andq %rcx, %rax             # 逻辑与：rax = rax & rcx");
            break;
        case BinaryOp::Or:
            emit("orq %rcx, %rax              # 逻辑或：rax = rax | rcx");
            break;
    }
}

// 条件（三元）表达式 [expr.cond] 的降级 ——  短路语义就藏在这两行跳转里：
// 先算条件、testq；假则【跳过 then 支】（它的副作用不会发生），真则跳过 else 支。
// 两支的值都落在 rax（Sema 已保证同型），用 end 标签汇合 ——
// 这是 if-else 的表达式版（clang/lib/CodeGen/CGExpr.cpp:CodeGenFunction::EmitConditionalOperator）。
// 逗号表达式 [expr.comma]：左操作数只求副作用（结果丢弃），值与类型取右操作数。
void CodeGen::visit(CommaExpr& expr) {
    emitComment(", ：左操作数只求副作用");
    emitExpr(expr.left);
    emitComment(", ：值取右操作数");
    emitExpr(expr.right);
}

void CodeGen::visit(ConditionalExpr& expr) {
    std::string elseLabel = newLabel("cond_else");
    std::string endLabel  = newLabel("cond_end");

    emitComment("?: condition");
    emitExpr(expr.condition);
    emit("testq %rax, %rax             # 条件值与自身按位与，设置 ZF 标志位");
    emit(std::format("je {}                     # 假 ⇒ 走 else 支（then 支不求值）", elseLabel));

    emitComment("?: then-branch");
    emitExpr(expr.thenExpr);
    emit(std::format("jmp {}                    # then 支算完，跳过 else 支", endLabel));

    emit(std::format("{}:                        # else 支", elseLabel));
    emitExpr(expr.elseExpr);

    emit(std::format("{}:                        # ?: 汇合点（rax = 被选中的值）", endLabel));
}


// 做什么：一元运算。Neg 直接 negq 取负（先求值再原地取负）；
//       Not 用 testq + sete 实现"等于 0 则为 1"的逻辑非。
// demo: -x → <加载 x 进 rax>; negq %rax
//       !x → <加载 x 进 rax>; testq %rax, %rax; sete %al; movzbq %al, %rax
void CodeGen::visit(UnaryExpr& expr) {
    // `&x` 求地址而非加载值，不能先按普通表达式读取 x。
    // 当前入口仅支持已实现的可寻址形态，其余明确诊断。
    // 参考：clang/lib/CodeGen/CGExpr.cpp 的左值地址生成。
    if (expr.op == UnaryOp::Addr) {
        if (expr.operand->kind == NodeKind::Var) {
            auto var = std::static_pointer_cast<VarExpr>(expr.operand);
            auto ci = m_classLocals.find(var->name);
            if (ci != m_classLocals.end()) {
                emit(std::format("leaq {}(%rbp), %rax    # &{} = 栈对象地址",
                    ci->second.offset, var->name));
                return;
            }
            auto it = m_localVars.find(var->name);
            if (it != m_localVars.end()) {
                emit(std::format("leaq {}(%rbp), %rax    # &{} = 局部变量栈地址",
                    it->second, var->name));
                return;
            }
        }
        // 其它形态（字段地址、数组元素地址…）尚未实现 —— 明确报错而非静默给错值
        throw std::runtime_error(std::format(
            "[CodeGen Error] address-of is only supported on local variables "
            "for now (line {})", expr.location.line));
    }

    // [expr.unary.op]：`*p` 读取指针所指的值；`&x` 计算地址，不加载 x 的值。
    if (expr.op == UnaryOp::Deref) {
        emitExpr(expr.operand);   // 解引用按所指类型宽度加载，不能固定读取 8 字节而越过窄字段。
// 参考：clang::CodeGenFunction::EmitLoadOfLValue。
        TypePtr pt = expr.operand->resolvedType;
        TypePtr pointee = (pt && pt->isPointer()) ? pt->pointeeType : nullptr;
        if (pointee && pointee->isBool()) {
            emit("movzbq (%rax), %rax        # 解引用：按 1 字节读 + 零扩展");
        } else if (pointee && pointee->isInt()) {
            emit("movl (%rax), %eax          # 解引用：按 4 字节读（movl 自动清高 32 位）");
        } else {
            emit("movq (%rax), %rax          # 解引用：按 8 字节读");
        }
        return;
    }

    emitExpr(expr.operand);

    switch (expr.op) {
        case UnaryOp::Neg:
            emit("negq %rax                   # 取负：rax = -rax（二进制补码）");
            break;
        case UnaryOp::Not:
            emit("testq %rax, %rax            # 测试 rax 是否为 0（设置 ZF）");
            emit("sete %al                    # 逻辑非：ZF=1 时 al=1");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case UnaryOp::Addr:
            // 上面已提前 return（取地址不走"先求值"路径），此处不可达
            throw std::runtime_error(
                "[CodeGen Error] UnaryOp::Addr should be handled before emitExpr");
        case UnaryOp::Deref:
            // 同上：解引用也已提前 return（它要的是地址而非值），此处不可达
            throw std::runtime_error(
                "[CodeGen Error] UnaryOp::Deref should be handled before emitExpr");
    }
}

// System V AMD64 参数寄存器为 rdi/rsi/rdx/rcx/r8/r9，普通返回值在 rax。
// sret、this 与引用实参的地址约定必须与函数序言一致。
void CodeGen::visit(CallExpr& expr) {
    // `Box<int>(7)` 物化临时对象：按实例类布局分配槽位，再调用 Sema 选定的构造符号。
    if (expr.isCtorCall) {
        if (!m_classTypes || m_classTypes->find(expr.ctorClassType->name) == m_classTypes->end()) {
            throw std::runtime_error(std::format(
                "[CodeGen Error] ctor temp class '{}' not in class table (line {})",
                expr.ctorClassType->name, expr.location.line));
        }
        auto& layout = m_classTypes->find(expr.ctorClassType->name)->second->classLayout;
        uint32_t size = layout.totalSize;
        int offset = allocTempSlot(size);

        emitComment(std::format("ctor temp {} : {} ({} bytes)",
            expr.ctorClassType->name, expr.ctorClassType->name, size));

        // ① 清零（8 字节一拍，与栈对象路径同公式）
        uint32_t aligned = (size + 7) / 8 * 8;
        for (uint32_t i = 0; i < aligned; i += 8) {
            emit(std::format("movq $0, {}(%rbp)    # 零初始化临时对象 偏移 +{}",
                offset + static_cast<int>(i), i));
        }

        // ② 安装 _vptr（仅多态类；公式与 visit(VarDeclStmt)/emitNew 同一来源）
        if (layout.hasVTable) {
            std::string vtableLabel = NameMangler::mangleVTable(expr.ctorClassType->name);
            emit(std::format("leaq {}(%rip), %rcx    # 取 vtable 首地址", vtableLabel));
            emit("addq $16, %rcx              # 跳过 offset-to-top 与 RTTI，指向 vtable[0]");
            emit(std::format("movq %rcx, {}(%rbp)    # 安装主 _vptr 到临时对象", offset));
            for (auto& base : layout.bases) {
                if (base.isPrimary || !base.hasVTable) continue;
                uint32_t vptrOffset = base.vtableSegmentOffset + 16;
                emit(std::format("leaq {}(%rip), %rcx    # 取次表首地址（基类 '{}'）",
                    vtableLabel, base.baseClassName));
                emit(std::format("addq ${}, %rcx          # 跳过次表头，指向 secondary[0]", vptrOffset));
                emit(std::format("movq %rcx, {}(%rbp)    # 安装次 _vptr（偏移 {} 处）",
                    offset + static_cast<int>(base.offset), base.baseClassName));
            }
        }

        // ③ 调用构造函数：实参压栈暂存 → 逆序弹入 rsi..r9（rdi 留给 this）
        //  引用形参 ⇒ 传地址（被调符号由 Sema 定好，见 m_funcBySymbol）
        std::string ctorSym = (expr.callee->kind == NodeKind::Var)
            ? std::static_pointer_cast<VarExpr>(expr.callee)->name : "";
        FuncDeclPtr ctorDecl = lookupCallee(ctorSym);
        for (size_t i = 0; i < expr.arguments.size() && i < 5; i++) {
            emitArgument(expr.arguments[i], paramNeedsAddress(ctorDecl, i));
            emit("pushq %rax                  # 构造实参压栈暂存");
        }
        for (int i = static_cast<int>(expr.arguments.size()) - 1; i >= 0 && i < 5; i--) {
            static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
            emit(std::format("popq %{}                   # 逆序弹出实参到寄存器", regs[i]));
        }
        emit(std::format("leaq {}(%rbp), %rdi       # this = 临时对象地址", offset));
        emit(std::format("callq {}               # 调用构造函数（临时对象）",
            asmSymbol(ctorSym)));

        // ④ 表达式值 = 对象地址；随后就地析构（全表达式结束），rax 保护/恢复。
        // 例外：赋值 RHS 语境（m_suppressTempDtor > 0）—— 临时对象要先 memcpy 进
        // 左值再析构，此处只登记槽位，析构由 visit(AssignStmt) 拷完后补发。
        m_lastCtorTempOffset = offset;
        m_lastCtorTempClass = expr.ctorClassType->name;
        emit(std::format("leaq {}(%rbp), %rax       # 表达式的值 = 临时对象地址", offset));
        if (m_suppressTempDtor == 0) {
            emit("pushq %rax                  # 保护对象地址（析构调用会踩 rax）");
            emitClassDtorCall(expr.ctorClassType->name, offset);
            emit("popq %rax                   # 恢复对象地址");
        }
        return;
    }

    emitComment("function call");

    // 类值返回的隐藏缓冲指针占 rdi，显式参数整体右移。
    // `S x = f();` 可直接使用 x 的槽作返回缓冲，避免中间复制。
    bool sretCall = false;
    uint32_t sretSize = 0;
    int sretBuf = kNoSretTarget;
    if (expr.resolvedType && returnsClassByValue(expr.resolvedType)) {
        sretSize = sretBufferBytes(expr.resolvedType);
        if (sretSize > 0) {
            sretCall = true;
            sretBuf = (m_sretTargetSlot != kNoSretTarget)
                ? m_sretTargetSlot : allocTempSlot(sretSize);
            m_sretTargetSlot = kNoSretTarget;   // 只给最外层这一次；嵌套调用自己分配
        }
    }

    // 检查是否是方法调用
    // ── 路径①/②：callee 形如 obj.method —— 先解析对象类型再定虚实 ──
    bool staticMemberCall = false;
    if (expr.callee->kind == NodeKind::Member) {
        auto member = std::static_pointer_cast<MemberExpr>(expr.callee);
        auto declaration = lookupCallee(member->resolvedCalleeSymbol);
        staticMemberCall = declaration && declaration->isStatic;
        if (staticMemberCall && !member->isTypeAccess) emitExpr(member->object);
    }
    if (expr.callee->kind == NodeKind::Member && !staticMemberCall &&
        !std::static_pointer_cast<MemberExpr>(expr.callee)->isTypeAccess) {
        auto mem = std::static_pointer_cast<MemberExpr>(expr.callee);
        if (mem->isMethodCall || (mem->object && mem->object->resolvedType)) {
            TypePtr objType = mem->object->resolvedType;
            if (mem->isArrow && objType && objType->isPointer()) {
                objType = objType->pointeeType;
            }
            objType = stripTopCvRef(objType);   //  const 对象的方法调用
// 检查是否是虚函数调用
            if (objType && objType->isClass() && m_classTypes) {
                auto it = m_classTypes->find(objType->name);
                if (it != m_classTypes->end()) {
                    auto& layout = it->second->classLayout;
                    // 虚表命中依据本次调用已选中的方法声明，使用 Sema 固定的槽索引。
                    for (auto& entry : layout.vtableEntries) {
                        const std::string slotSym = vtableEntrySymbol(entry);
                        bool hit = !mem->resolvedCalleeSymbol.empty()
                            ? slotSym == mem->resolvedCalleeSymbol
                            : slotSym.find(mem->memberName) != std::string::npos;
                        if (hit) {
                            // emitVirtualCall 入口接收对象地址；实参求值期间必须保护 this。
                            emitExpr(mem->object);
                            emit(std::format("movq %rax, %{}            # this = 对象地址{}",
                                sretCall ? "rsi" : "rdi",
                                sretCall ? "（sret：rdi 让给返回缓冲）" : "（第 0 参数）"));
                            emitVirtualCall(objType->name, mem->memberName,
                                          expr.arguments, entry.index, sretBuf,
                                          entry.method);
                            if (sretCall) {
                                emit(std::format("leaq {}(%rbp), %rax       # 表达式的值 = 返回缓冲地址",
                                    sretBuf));
                            }
                            return;
                        }
                    }
                }
            }

            // 普通方法调用（非虚函数）
            // 先计算对象地址作为第一个参数（this 指针）
            // Itanium C++ ABI：this 是隐式第 0 参数走 rdi，
            // 显式实参从 rsi 起 —— 故下面实参最多取 5 个（this + 5 = 6 寄存器）
            //  sret 时 rdi 归返回缓冲指针 ⇒ this 挪到 rsi、显式实参从 rdx 起
            // （最多 4 个）。两侧（定义点 emitFunction / 调用点此处）必须同口径。
            const size_t maxArgs = sretCall ? 4 : 5;
            emitExpr(mem->object);
            emit("pushq %rax                   # 暂存 this 指针到栈上");

            // 计算参数
            // （逐个压栈暂存，随后逆序弹入 rsi/rdx/rcx/r8/r9）
            //  引用形参 ⇒ 传地址（被调身份由 Sema 定好的符号给出）
            std::string memberCalleeSymbol =
                !mem->resolvedCalleeSymbol.empty()
                    ? mem->resolvedCalleeSymbol
                    : (objType && objType->isClass()
                           ? objType->name + "_" + mem->memberName : std::string());
            FuncDeclPtr memberCallee = lookupCallee(memberCalleeSymbol);
            for (size_t i = 0; i < expr.arguments.size() && i < maxArgs; i++) {
                emitArgument(expr.arguments[i], paramNeedsAddress(memberCallee, i));
                emit("pushq %rax                  # 实参值压栈暂存");
            }

            // 恢复参数到寄存器
            static const char* memberArgRegs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
            for (int i = static_cast<int>(expr.arguments.size()) - 1; i >= 0 && i < static_cast<int>(maxArgs); i--) {
                emit(std::format("popq %{}                   # 逆序弹出实参到寄存器",
                    memberArgRegs[sretCall ? i + 1 : i]));
            }

            // this 指针到 rdi（sret 时到 rsi）
            emit(std::format("popq %{}                   # 弹出 this 指针到 {}（{}）",
                sretCall ? "rsi" : "rdi", sretCall ? "rsi" : "rdi",
                sretCall ? "sret：this 让到 rsi" : "第 0 参数"));
            if (sretCall) {
                emit(std::format("leaq {}(%rbp), %rdi       # rdi = 返回缓冲（sret 隐藏参数）", sretBuf));
            }

            // 调用
            // 符号名构造：简化 mangling "类名_方法名"（如 Dog_speak），
            // 与 emitFunction 发射的标签、vtable 表项保持一致；
            // 标准 GCC ABI 下应为 _ZN3Dog5speakEv 形式
            std::string funcName;
            if (!mem->resolvedCalleeSymbol.empty()) {
                // 成员模板调用使用 Sema 回填的实例符号，且与定义端共用 asmSymbol 净化限定名。
                funcName = asmSymbol(mem->resolvedCalleeSymbol);
            } else if (objType && objType->isClass()) {
                funcName = objType->name + "_" + mem->memberName;
            } else {
                funcName = mem->memberName;
            }
            emit(std::format("callq {}                  # 调用方法 {}", funcName, funcName));
            if (sretCall) {
                emit(std::format("leaq {}(%rbp), %rax       # 表达式的值 = 返回缓冲地址", sretBuf));
            }
            return;
        }
    }

    // 普通函数调用
    //  先定函数名：实参要不要按【地址】传取决于被调函数的形参是不是引用，
    // 而被调身份由 Sema 定好的符号名给出（见 m_funcBySymbol）。
    std::string funcName;
    std::string calleeSymbol;
    if (expr.callee->kind == NodeKind::Var) {
        auto var = std::static_pointer_cast<VarExpr>(expr.callee);
        calleeSymbol = var->name;
        // asmSymbol：限定名调用（Math::scale）在源码层带 "::"，
        // 净化后与标签发射端一致（见 emitFunction）
        funcName = asmSymbol(var->name);
    }
    if (expr.callee->kind == NodeKind::Member) {
        auto member = std::static_pointer_cast<MemberExpr>(expr.callee);
        calleeSymbol = member->resolvedCalleeSymbol;
        funcName = asmSymbol(calleeSymbol);
    }
    FuncDeclPtr calleeDecl = lookupCallee(calleeSymbol);

    // 计算参数并放入对应寄存器
    //  sret 时 rdi 是隐藏的返回缓冲指针 ⇒ 显式实参从 rsi 起（最多 5 个）
    const size_t maxArgs = sretCall ? 5 : 6;
    for (size_t i = 0; i < expr.arguments.size() && i < maxArgs; i++) {
        emitArgument(expr.arguments[i], paramNeedsAddress(calleeDecl, i));
        emit("pushq %rax                  # 实参值压栈暂存");
    }

    // 恢复参数到寄存器（逆序）
    static const char* regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    for (int i = static_cast<int>(expr.arguments.size()) - 1; i >= 0; i--) {
        if (i < static_cast<int>(maxArgs)) {
            emit(std::format("popq %{}                   # 逆序弹出实参到寄存器",
                regs[sretCall ? i + 1 : i]));
        }
    }

    if (sretCall) {
        emit(std::format("leaq {}(%rbp), %rdi       # rdi = 返回缓冲（sret 隐藏参数）", sretBuf));
    }
    emit(std::format("callq {}                  # 调用函数 {}", funcName, funcName));
    if (sretCall) {
        emit(std::format("leaq {}(%rbp), %rax       # 表达式的值 = 返回缓冲地址", sretBuf));
    }
}

// 虚调用通过对象 vptr 和编译期槽索引寻找运行时目标。
// vptr 已指向首方法槽，偏移是 index * 8；不能再次加上两个虚表头槽。
// 入口对象地址须遵守 emitVirtualCall 的 this 约定，sret 调用还需保留隐藏返回指针。
void CodeGen::emitVirtualCall(
    const std::string& className,
    const std::string& methodName,
    const std::vector<ExprPtr>& args,
    uint32_t vtableIndex,
    int sretBuf,
    FuncDeclPtr callee) {

    emitComment(std::format("VIRTUAL CALL: {}::{} (vtable[{}])",
        className, methodName, vtableIndex));

    //  sret（sretBuf != kNoSretTarget）：隐藏返回缓冲占 rdi ⇒ this 挪到 rsi、
    // 显式实参从 rdx 起。非 sret：this 在 rdi、显式实参从 rsi 起（老口径）。
    const bool sret = (sretBuf != kNoSretTarget);
    const char* thisReg = sret ? "rsi" : "rdi";
    const size_t maxArgs = sret ? 4 : 5;

    // ─── 保存对象地址 ───
    // this 进入时在 rdi（sret 时在 rsi）；先压栈保护（下面求/弹实参会反复使用
    // rax 与参数寄存器），最后弹回兼作隐式第 0 实参
    emit(std::format("pushq %{}                   # 暂存 this（对象地址）到栈上保护", thisReg));

    // ─── 准备参数 ───
    // 第一个参数是 this 指针（对象地址）

    // 先计算参数（跳过 this）
    // 显式实参最多 5 个（sret 时 4 个）：rdi 留给 this，rsi/rdx/rcx/r8/r9 装实参
    for (size_t i = 0; i < args.size() && i < maxArgs; i++) {
        //  引用形参 ⇒ 传地址。被调声明直接取自 vtable 槽里的声明句柄
        // （learn/54：槽里挂的是声明，不是名字）⇒ 形参类型与定义点同源。
        emitArgument(args[i], paramNeedsAddress(callee, i));
        emit("pushq %rax                  # 实参值压栈暂存");
    }

    // 恢复参数到寄存器
    static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
    for (int i = static_cast<int>(args.size()) - 1; i >= 0 && i < static_cast<int>(maxArgs); i--) {
        emit(std::format("popq %{}                   # 逆序弹出实参到寄存器",
            regs[sret ? i + 1 : i]));
    }

    // this 指针回位：从栈中弹回函数入口时保存的对象地址
    // （弹栈同时把调用方的栈指针复原——push/pop 严格配对，
    //   虚析构经块尾反复调用也不会漂移）
    emit(std::format("popq %{}                   # 弹出 this 指针（恢复对象地址）", thisReg));

    if (sret) {
        emit(std::format("leaq {}(%rbp), %rdi       # rdi = 返回缓冲（sret 隐藏参数）", sretBuf));
    }

    // ─── 虚函数调用三部曲 ───

    // (a) 从对象偏移量 +0 处读出 _vptr 指针
    emit("# ═══ 虚函数调用 (a)：从对象读出 _vptr ═══");
    emit(std::format("movq (%{}), %rax            # 从对象首 8 字节读出 _vptr", thisReg));

    // (b) 从 vtable 中加上 index 偏移量，读出真正的函数地址
    //     _vptr 已指向 vtable[0]（emitNew/构造函数写入 表首+16），
    //     所以从 _vptr 起第 index 项 = 偏移 index*8
    uint32_t vtableOffset = vtableIndex * 8;
    emit("# ═══ 虚函数调用 (b)：从 vtable 加载函数地址 ═══");
    emit(std::format("movq {}(%rax), %rax       # 从 vtable[{}] 读出函数地址（偏移 {}）",
        vtableOffset, vtableIndex, vtableOffset));

    // (c) 间接跳转到该地址执行
    emit("# ═══ 虚函数调用 (c)：间接跳转到真实函数 ═══");
    emit("callq *%rax                  # 经 vtable 间接调用（跳转到 rax 所指地址）");

    emitComment(std::format("END VIRTUAL CALL {}::{}", className, methodName));
}

// [expr.ref]：成员地址 = 对象地址 + Sema 回填的字段偏移。
// `.` 取对象地址，`->` 使用指针值；标量读取后加载，类字段保留地址。
void CodeGen::visit(MemberExpr& expr) {
    if (expr.isTypeAccess && expr.staticConstFolded) {
        emit(std::format("movq ${}, %rax", expr.staticConstValue));
        return;
    }
    emitComment(std::format("member access: .{}", expr.memberName));

    // 静态成员使用 resolvedStaticSymbol 全局寻址，不走字段偏移。
    // 读取保留对象表达式副作用，如 `make().count` 仍会调用 make()。
    if (!expr.resolvedStaticSymbol.empty()) {
        if (!expr.isTypeAccess) emitExpr(expr.object);
        // 类内初始化的 static const：没有存储，编译期折叠成立即数
        // （Sema 的 lookupStaticConst 命中后回填 staticConstFolded/Value）。
        if (expr.staticConstFolded) {
            emit(std::format("movq ${}, %rax    # 静态常量折叠 .{}（无存储，对象值弃用）",
                expr.staticConstValue, expr.memberName));
            return;
        }
        emit(std::format("movq {}(%rip), %rax    # 读取静态成员 .{}（全局符号，对象值弃用）",
            asmSymbol(expr.resolvedStaticSymbol), expr.memberName));
        return;
    }

    // 计算对象地址
    emitExpr(expr.object);

    TypePtr objType = expr.object->resolvedType;
    if (expr.isArrow && objType && objType->isPointer()) {
        // 对于 -> 操作，对象本身就是一个指针
        // 不需要额外解引用
    } else if (objType && !objType->isPointer()) {
        // 对于 . 操作，如果对象不是指针，需要获取其地址
        // 这里简化处理
    }

    if (objType) {
        TypePtr actualType = objType;
        if (expr.isArrow && objType->isPointer()) {
            actualType = objType->pointeeType;
        }
        actualType = stripTopCvRef(actualType);   //  const 对象的成员读

        if (actualType && actualType->isClass()) {
            // 限定字段优先使用 Sema 回填偏移，避免按裸名误选派生类同名字段。
            if (expr.resolvedFieldOffset >= 0) {
                if (expr.resolvedFieldSize <= 4) {
                    emit(std::format("movl {}(%rax), %eax    # 读取限定名成员 .{}（偏移 +{}，4B int）",
                        expr.resolvedFieldOffset, expr.memberName, expr.resolvedFieldOffset));
                } else {
                    emit(std::format("movq {}(%rax), %rax    # 读取限定名成员 .{}（偏移 +{}，8B）",
                        expr.resolvedFieldOffset, expr.memberName, expr.resolvedFieldOffset));
                }
                return;
            }
            auto field = actualType->classLayout.findField(expr.memberName);
            if (field) {
                // 类字段内联在父对象中，访问结果应为 obj + fieldOffset 地址。
                // `o->f.a` 的 f 不能加载首字段当指针，后续 .a 还需使用子对象地址。
                if (field->type && field->type->isArray()) {
                    emit(std::format("leaq {}(%rax), %rax    # 数组字段 .{} 取首地址（偏移 +{}）",
                        field->offset, expr.memberName, field->offset));
                    return;
                }
                if (field->type && field->type->isClass()) {
                    emit(std::format("leaq {}(%rax), %rax    # 嵌套类字段 .{} 取地址（子对象偏移 +{}）",
                        field->offset, expr.memberName, field->offset));
                    return;
                }
                // 直接用偏移量访问字段
                //  按字段宽度选指令：int(4B) 用 movl（32 位加载自动零
                // 扩展进 rax，避免 movq 连相邻字段/对象外字节一起读进
                // 高 32 位）；指针/8B 用 movq；bool(1B) 用 movzbq。
                if (field->size <= 4) {
                    emit(std::format("movl {}(%rax), %eax    # 读取字段 .{}（偏移 +{}，4B int）",
                        field->offset, expr.memberName, field->offset));
                } else if (field->size == 1) {
                    emit(std::format("movzbq {}(%rax), %rax  # 读取字段 .{}（偏移 +{}，1B bool）",
                        field->offset, expr.memberName, field->offset));
                } else {
                    emit(std::format("movq {}(%rax), %rax    # 读取字段 .{}（偏移 +{}，8B 指针/long）",
                        field->offset, expr.memberName, field->offset));
                }
                return;
            }
        }
    }

    // 默认：偏移量 0
    emit("movq (%rax), %rax            # 成员访问（偏移未知，默认 0）");
}

// 容器 `v[i]` 读操作调用 at(i)；原生数组使用元素大小计算地址。
void CodeGen::visit(IndexExpr& expr) {
    TypePtr objType = expr.object->resolvedType;
    if (objType && objType->isReference()) objType = objType->referencedType;
    bool isRaw = objType && (objType->isArray() || objType->isPointer());

    if (isRaw) {
        emitComment("raw array/pointer subscript a[i]");
        emitExpr(expr.object);
        emit("pushq %rax                  # 暂存基址");
        emitExpr(expr.index);
        emit("popq %rcx                   # 弹出基址到 rcx");
        TypePtr elemType = objType->isArray() ? objType->elementType : objType->pointeeType;
        uint32_t elemSize = elemType ? elemType->sizeInBytes() : 8;
        if (elemSize == 1) {
            emit("leaq (%rcx, %rax, 1), %rcx");
        } else if (elemSize == 2) {
            emit("leaq (%rcx, %rax, 2), %rcx");
        } else if (elemSize == 4) {
            emit("leaq (%rcx, %rax, 4), %rcx");
        } else if (elemSize == 8) {
            emit("leaq (%rcx, %rax, 8), %rcx");
        } else {
            emit(std::format("imulq ${}, %rax", elemSize));
            emit("addq %rax, %rcx");
        }
        if (elemSize <= 1) {
            emit("movzbq (%rcx), %rax         # 按 1 字节读 + 零扩展");
        } else if (elemSize <= 4) {
            emit("movl (%rcx), %eax           # 按 4 字节读");
        } else {
            emit("movq (%rcx), %rax           # 按 8 字节读");
        }
        return;
    }

    emitComment("subscript v[i] → desugar to v.at(i)");

    // 容器类名：resolvedType 由语义阶段填充（指针容器先解引用一层）
    objType = expr.object->resolvedType;
    if (objType && objType->isPointer()) {
        objType = objType->pointeeType;
    }
    std::string className = (objType && objType->isClass()) ? objType->name : "";

    // this = 对象地址（栈对象经 emitVar 走 leaq 取址；指针直接取值）
    emitExpr(expr.object);
    emit("pushq %rax                  # 保存 this 指针（容器对象地址）");

    // 下标表达式 → rax，随后按 System V 调用约定装参
    emitExpr(expr.index);
    emit("pushq %rax                  # 下标值压栈暂存");

    emit("popq %rsi                   # arg1: 弹出下标到 rsi");
    emit("popq %rdi                   # this: 弹出容器地址到 rdi");
    //  同上：符号取 Sema 回填值（IntVec_at_1），空则退回硬拼。
    std::string atSymbol = expr.atSymbol.empty()
                         ? className + "_at" : expr.atSymbol;
    emit(std::format("callq {:<{}}# 调用 v.at(i) 读取元素",
                     atSymbol, 19 + className.size()));
}

// new 按类布局大小调用 malloc，初始化 vptr 后调用 Sema 选定的构造函数。
// 构造调用不得丢失分配结果：new 表达式最终返回对象指针。
void CodeGen::visit(NewExpr& expr) {
    emitComment(std::format("new {}()", expr.className));

    // 查找类的大小
    uint32_t size = 8; // 默认大小
    std::string vtableLabel;
    bool hasVTable = false;

    // 在全局类表中查布局：totalSize 决定分配字节数（含 _vptr 槽，
    // 由语义阶段的 ClassLayout 计算），hasVTable 决定是否要安装 _vptr
    if (m_classTypes) {
        auto it = m_classTypes->find(expr.className);
        if (it != m_classTypes->end()) {
            size = it->second->classLayout.totalSize;
            if (size == 0) size = 8;
            hasVTable = it->second->classLayout.hasVTable;
            if (hasVTable) {
                vtableLabel = NameMangler::mangleVTable(expr.className);
            }
        }
    }

    // 调用 malloc
    emit(std::format("movq ${}, %rdi             # malloc 分配大小：{} 字节", size, size));
    emit("callq malloc                  # 调用 malloc 分配堆内存");
    emit("pushq %rax                    # 暂存返回的对象指针到栈上");

    if (hasVTable) {
        // 设置主 _vptr：对象偏移量 0 处 = vtable 地址 + 16
        emit(std::format("leaq {}(%rip), %rcx    # 取 vtable 首地址", vtableLabel));
        emit("addq $16, %rcx              # 跳过 offset-to-top 与 RTTI，指向 vtable[0]");
        emit("movq (%rsp), %rax           # 从栈上取回对象指针");
        emit("movq %rcx, (%rax)           # 安装主 _vptr 到对象首 8 字节");

        // 次 vptr：每个次基类各一个
        if (m_classTypes) {
            auto it = m_classTypes->find(expr.className);
            if (it != m_classTypes->end()) {
                for (auto& base : it->second->classLayout.bases) {
                    if (base.isPrimary || !base.hasVTable) continue;
                    uint32_t vptrOffset = base.vtableSegmentOffset + 16;
                    emit("movq (%rsp), %rax           # 从栈上取回对象指针");
                    emit(std::format("leaq {}(%rip), %rcx    # 取次表首地址（基类 '{}'）",
                        vtableLabel, base.baseClassName));
                    emit(std::format("addq ${}, %rcx          # 跳过次表头，指向 secondary[0]", vptrOffset));
                    emit(std::format("movq %rcx, {}(%rax)     # 安装次 _vptr（偏移 {} 处）",
                        base.offset, base.baseClassName));
                }
            }
        }
    }

    // 调用构造函数
    // 先计算参数（实参最多 5 个，this 占用 rdi）
    for (size_t i = 0; i < expr.constructorArgs.size() && i < 5; i++) {
        emitExpr(expr.constructorArgs[i]);
        emit("pushq %rax                  # 构造实参压栈暂存");
    }

    // 恢复参数到 rsi, rdx, rcx, r8, r9
    for (int i = static_cast<int>(expr.constructorArgs.size()) - 1; i >= 0 && i < 5; i--) {
        static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
        emit(std::format("popq %{}                   # 逆序弹出实参到寄存器", regs[i]));
    }

    // 从栈顶取回 objPtr 传给 rdi
    emit("movq (%rsp), %rdi             # this = 已分配对象指针（栈顶取出）");
    // 构造调用使用 Sema 回填的 ctorSymbol；未回填时保留现有兜底约定。
    // 调用与定义均经 asmSymbol 净化限定名，避免符号拼写不一致。
    std::string ctorName = expr.ctorSymbol;
    if (ctorName.empty()) {
        ctorName = expr.className + "_" + expr.className;   // 兜底（旧约定）
        if (!expr.constructorArgs.empty()) {
            ctorName += "_" + std::to_string(expr.constructorArgs.size());
        }
    }
    ctorName = asmSymbol(ctorName);
    emit(std::format("callq {}                 # 调用构造函数 {}", ctorName, ctorName));

    emit("popq %rax                     # 弹出对象指针作为 new 表达式返回值");
    emitComment(std::format("end new {}()", expr.className));
}

// this 已在函数序言 spill 到栈槽，表达式从槽中读取对象指针。
void CodeGen::visit(ThisExpr&) {
    auto it = m_localVars.find("this");
    if (it != m_localVars.end()) {
        emit(std::format("movq {}(%rbp), %rax       # 加载 this 指针", it->second));
    } else {
        emit("# WARNING: 'this' 在类方法外部使用");
        emit("xorq %rax, %rax             # 错误路径：清零返回");
    }
}

// [expr.dynamic.cast]：运行时通过自定义 RTTI 查找目标基类并调整指针，失败返回空指针。
void CodeGen::visit(DynamicCastExpr& expr) {
    m_needsDynamicCastHelper = true;

    emitComment(std::format("dynamic_cast<{}*>(operand) —— 运行时 RTTI 检查",
        expr.targetClassName));
    emitExpr(expr.operand);                     // %rax = 源对象指针
    emit(std::format("movq %rax, %rdi              # 参数 1：源对象指针"));
    emit(std::format("leaq {}(%rip), %rsi    # 参数 2：目标 typeinfo",
        NameMangler::mangleRTTI(expr.targetClassName)));
    emit("callq __minicc_dynamic_cast  # 调用运行时 RTTI 遍历检查类型兼容性");
    emitComment(std::format("%rax = 成功:原指针 / 失败:0 ({}) ",
        expr.targetClassName));
}

// 运行时转换助手每个编译单元只发射一次；输入对象指针和目标 type_info，返回调整后的指针或 0。
void CodeGen::emitDynamicCastHelper() {
    // 运行时转换助手递归遍历 RTTI 基类表，累积子对象偏移以支持多继承。
    // 类型匹配时返回调整后指针，未找到目标类型时返回 0。

    emitComment("__minicc_dynamic_cast(obj, target_typeinfo) —— DFS RTTI 树匹配（多继承版）");
    emit("__minicc_dynamic_cast:        # 运行时助手入口：rdi=对象指针，rsi=目标 typeinfo");
    emit("testq %rdi, %rdi             # 检测对象指针是否为空");
    emit("je .Ldc_fail                 # 空指针直接走失败路径");
    emit("movq (%rdi), %rcx            # 从对象首 8 字节读出 vptr");
    emit("movq -16(%rcx), %rdx         # vptr[-2] = offset-to-top（次表偏移量）");
    emit("leaq (%rdi,%rdx), %rdi       # top = obj + ott（归顶到最派生对象）");
    emit("movq -8(%rcx), %rdx          # vptr[-1] = 最派生 typeinfo 地址");
    emit("xorq %rcx, %rcx              # acc_offset = 0（累积偏移清零）");
    emit("callq .Ldc_dfs               # 进入 DFS：DFS(ti, target, top, acc)");
    emit("ret                          # 助手返回（rax = 转型结果或 0）");

    // ── DFS 递归搜索 ──
    // 参数约定：rdx = current ti, rsi = target, rdi = top, rcx = acc_offset
    // 返回：rax = 结果指针或 0
    emit(".Ldc_dfs:                    # DFS 递归入口：rdx=当前 ti，rsi=目标");
    emit("cmpq %rsi, %rdx              # 当前 typeinfo 是否 == 目标？");
    emit("je .Ldc_dfs_hit              # 命中：返回 top + acc");
    emit("movq 16(%rdx), %rax          # 读取基类计数 N = ti[+16]");
    emit("testq %rax, %rax             # N == 0 表示无基类（叶子节点）");
    emit("je .Ldc_dfs_ret0             # 叶子节点直接返回 0");
    // 建立栈帧，保存 caller-saved 寄存器
    emit("pushq %rbp                   # 保存旧栈帧基址");
    emit("movq %rsp, %rbp              # 建立新栈帧");
    emit("pushq %rdi                   # 保存 top（最派生对象地址）");
    emit("pushq %rsi                   # 保存 target（目标 typeinfo）");
    emit("pushq %rdx                   # 保存 current ti（当前 typeinfo）");
    emit("pushq %rcx                   # 保存 acc_offset（累积偏移）");
    emit("xorq %r12, %r12              # i = 0（基类遍历下标）");
    emit("movq 16(%rdx), %r13          # N = base count（callee-saved 保存计数）");
    emit(".Ldc_dfs_loop:               # 基类遍历循环开始");
    emit("cmpq %r13, %r12              # i >= N？检查是否遍历完所有基类");
    emit("jge .Ldc_dfs_end             # 遍历完成，跳到收尾");
    // 加载 bases[i]：ti 在 +24+16*i，offset 在 +24+16*i+8
    emit("movq -24(%rbp), %rdx         # 从栈帧恢复 current ti");
    emit("leaq 24(%rdx), %rax          # 计算 &bases[0]（ti + 24）");
    emit("imulq $16, %r12, %r14        # i * 16（每个 base 条目 16 字节）");
    emit("addq %r14, %rax              # &bases[i] = &bases[0] + i*16");
    emit("movq (%rax), %rdx            # 加载 bases[i].typeinfo");
    emit("movq 8(%rax), %rcx           # 加载 bases[i].offset");
    emit("addq -32(%rbp), %rcx         # acc = parent_acc + bases[i].offset（累积偏移）");
    emit("callq .Ldc_dfs               # 递归搜索子树");
    emit("testq %rax, %rax             # 检测递归结果是否为 0");
    emit("jne .Ldc_dfs_found           # 子树命中！直接透传");
    emit("incq %r12                    # i++（继续下一个基类）");
    emit("jmp .Ldc_dfs_loop            # 循环继续");
    // 所有基类都没命中
    emit(".Ldc_dfs_end:                # 所有基类都没命中，返回 0");
    emit("xorq %rax, %rax              # 结果清零");
    emit("addq $32, %rsp               # 释放 4 个保存槽（acc, ti, target, top）");
    emit("popq %rbp                    # 恢复栈帧基址");
    emit("ret                          # 返回失败");
    // 命中：返回 top + acc_offset
    emit(".Ldc_dfs_hit:                # 当前 ti == target，命中路径");
    emit("movq %rdi, %rax              # rax = top（最派生对象地址）");
    emit("addq %rcx, %rax              # rax = top + acc_offset（调整后指针）");
    emit("ret                          # 返回转型成功");
    // 递归返回命中：直接透传 rax
    emit(".Ldc_dfs_found:              # 子树递归命中，透传结果");
    emit("addq $32, %rsp               # 释放 4 个保存槽");
    emit("popq %rbp                    # 恢复栈帧基址");
    emit("ret                          # 返回命中结果");
    // 叶子节点失败
    emit(".Ldc_dfs_ret0:               # 叶子节点（无基类）失败路径");
    emit("xorq %rax, %rax              # 结果清零");
    emit("ret                          # 返回 0");
    emit(".Ldc_fail:                   # 入口空指针失败路径");
    emit("xorq %rax, %rax              # 失败：返回 0");
    emit("ret                          # 返回");
    emit("");
}

} // namespace minicc
