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

#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>

#include <filesystem>

#include "CRAX.h"

using namespace klee;

namespace s2e::plugins::crax {

CRAX *g_crax = nullptr;

S2E_DEFINE_PLUGIN(CRAX, "Modular Exploit Generation System", "",
                  "LinuxMonitor", "MemoryMap", "ModuleMap");

pybind11::scoped_interpreter CRAX::s_pybind11;
pybind11::module CRAX::s_pwnlib(pybind11::module::import("pwnlib.elf"));

CRAX::CRAX(S2E *s2e)
    : Plugin(s2e),
      beforeInstruction(),
      afterInstruction(),
      beforeSyscall(),
      afterSyscall(),
      onStateForkModuleDecide(),
      beforeExploitGeneration(),
      m_currentState(),
      m_linuxMonitor(),
      m_showInstructions(CRAX_CONFIG_GET_BOOL(".showInstructions", false)),
      m_showSyscalls(CRAX_CONFIG_GET_BOOL(".showSyscalls", true)),
      m_concolicMode(CRAX_CONFIG_GET_BOOL(".concolicMode", false)),
      m_exploitForm(CRAX::ExploitForm::SCRIPT),
      m_proxy(),
      m_register(),
      m_memory(),
      m_disassembler(),
      m_exploit(CRAX_CONFIG_GET_STRING(".elfFilename", DEFAULT_BINARY_FILENAME),
                CRAX_CONFIG_GET_STRING(".libcFilename", DEFAULT_LIBC_FILENAME),
                CRAX_CONFIG_GET_STRING(".ldFilename", DEFAULT_LD_FILENAME)),
      m_exploitGenerator(),
      m_modules(),
      m_techniques(),
      m_targetProcessPid(),
      m_allowedForkingStates() {}


void CRAX::initialize() {
    g_crax = this;
    m_register.initialize();
    m_memory.initialize();

    m_linuxMonitor = s2e()->getPlugin<LinuxMonitor>();

    m_linuxMonitor->onProcessLoad.connect(
            sigc::mem_fun(*this, &CRAX::onProcessLoad));

    // Install symbolic RIP handler.
    s2e()->getCorePlugin()->onSymbolicAddress.connect(
            sigc::mem_fun(*this, &CRAX::onSymbolicRip));

    s2e()->getCorePlugin()->onStateForkDecide.connect(
            sigc::mem_fun(*this, &CRAX::onStateForkDecide));

    // Run `ROPgadget <elf>` on the following ELF files in a worker thread
    // and cache their outputs.
    std::vector<const ELF *> elfFiles = {
        &m_exploit.getElf(),
        &m_exploit.getLibc()
    };
    m_exploitGenerator.getRopGadgetResolver().buildRopGadgetOutputCacheAsync(elfFiles);

    // Initialize modules.
    for (const auto &name : CRAX_CONFIG_GET_STRING_LIST(".modules")) {
        log<INFO>() << "Creating module: " << name << '\n';
        m_modules.push_back(Module::create(name));
    }

    // Initialize techniques.
    for (const auto &name : CRAX_CONFIG_GET_STRING_LIST(".techniques")) {
        log<INFO>() << "Creating technique: " << name << '\n';
        m_techniques.push_back(Technique::create(name));
    }
}


void CRAX::onSymbolicRip(S2EExecutionState *state,
                         ref<Expr> symbolicRip,
                         uint64_t concreteRip,
                         bool &concretize,
                         CorePlugin::symbolicAddressReason reason) {
    if (reason != CorePlugin::symbolicAddressReason::PC) {
        return;
    }

    // Set m_currentState to state.
    // All subsequent calls to reg() and mem() will operate on m_currentState.
    setCurrentState(state);

    log<WARN>()
        << "Detected symbolic RIP: " << hexval(concreteRip)
        << ", original value was: " << hexval(reg().readConcrete(Register::X64::RIP))
        << '\n';

    reg().setRipSymbolic(symbolicRip);

    // Dump CPU registers and virtual memory mappings.
    reg().showRegInfo();
    mem().showMapInfo();

    // Let the modules do whatever that needs to be done.
    beforeExploitGeneration.emit(state);

    // Generate the exploit.
    m_exploitGenerator.run(state);

    s2e()->getExecutor()->terminateState(*state, "End of exploit generation");
}

void CRAX::onProcessLoad(S2EExecutionState *state,
                         uint64_t cr3,
                         uint64_t pid,
                         const std::string &imageFileName) {
    setCurrentState(state);

    log<WARN>() << "onProcessLoad: " << imageFileName << '\n';

    if (m_proxy.getType() == Proxy::Type::NONE) {
        m_proxy.maybeDetectProxy(imageFileName);
    }

    // If the user provides "./target" instead of "target" as the elf filename,
    // then we use std::filesystem::path to discard the leading "./"
    if (imageFileName == std::filesystem::path(m_exploit.getElf().getFilename()).filename()) {
        m_targetProcessPid = pid;

        m_linuxMonitor->onModuleLoad.connect(
                sigc::mem_fun(*this, &CRAX::onModuleLoad));

        s2e()->getCorePlugin()->onTranslateInstructionStart.connect(
                sigc::mem_fun(*this, &CRAX::onTranslateInstructionStart));

        s2e()->getCorePlugin()->onTranslateInstructionEnd.connect(
                sigc::mem_fun(*this, &CRAX::onTranslateInstructionEnd));
    }
}

void CRAX::onModuleLoad(S2EExecutionState *state,
                        const ModuleDescriptor &md) {
    setCurrentState(state);

    log<WARN>() << "onModuleLoad: " << md.Name << '\n';

    // Resolve ELF base if the target binary has PIE.
    if (md.Name == "target" && m_exploit.getElf().checksec.hasPIE) {
        assert(md.Sections.size());

        uint64_t elfBase = md.Sections.front().runtimeLoadBase;
        elfBase = Memory::roundDownToPageBoundary(elfBase);

        log<WARN>() << "ELF loaded at: " << hexval(elfBase) << '\n';
        m_exploit.getElf().setBase(elfBase);
    }
}

void CRAX::onTranslateInstructionStart(ExecutionSignal *onInstructionExecute,
                                       S2EExecutionState *state,
                                       TranslationBlock *tb,
                                       uint64_t pc) {
    if (m_linuxMonitor->isKernelAddress(pc)) {
        return;
    }

    // Register the instruction hook which will be called
    // before the instruction is executed.
    onInstructionExecute->connect(
            sigc::mem_fun(*this, &CRAX::onExecuteInstructionStart));
}

void CRAX::onTranslateInstructionEnd(ExecutionSignal *onInstructionExecute,
                                     S2EExecutionState *state,
                                     TranslationBlock *tb,
                                     uint64_t pc) {
    if (m_linuxMonitor->isKernelAddress(pc)) {
        return;
    }

    // Register the instruction hook which will be called
    // after the instruction is executed.
    onInstructionExecute->connect(
            sigc::mem_fun(*this, &CRAX::onExecuteInstructionEnd));
}

void CRAX::onExecuteInstructionStart(S2EExecutionState *state,
                                     uint64_t pc) {
    setCurrentState(state);

    std::optional<Instruction> i = m_disassembler.disasm(pc);

    if (!i) {
        return;
    }

    auto craxState = getPluginState(state);
    auto &pending = craxState->m_pendingOnExecuteSyscallEnd;

    if (pending.size()) {
        auto it = pending.find(pc);
        if (it != pending.end()) {
            onExecuteSyscallEnd(state, *i, it->second);
            pending.erase(pc);
        }
    }

    if (m_showInstructions && !m_linuxMonitor->isKernelAddress(pc)) {
        log<INFO>()
            << hexval(i->address) << ": "
            << i->mnemonic << ' ' << i->opStr
            << '\n';
    }

    if (i->mnemonic == "syscall") {
        onExecuteSyscallStart(state, *i);
    }

    // Execute instruction hooks installed by the user.
    beforeInstruction.emit(state, *i);
}

void CRAX::onExecuteInstructionEnd(S2EExecutionState *state,
                                   uint64_t pc) {
    setCurrentState(state);

    std::optional<Instruction> i = m_disassembler.disasm(pc);

    if (!i) {
        return;
    }

    // Execute instruction hooks installed by the user.
    afterInstruction.emit(state, *i);
}

void CRAX::onExecuteSyscallStart(S2EExecutionState *state,
                                 const Instruction &i) {
    constexpr bool verbose = false;
    SyscallCtx syscall;
    syscall.ret = 0;
    syscall.nr = reg().readConcrete(Register::X64::RAX, verbose);
    syscall.arg1 = reg().readConcrete(Register::X64::RDI, verbose);
    syscall.arg2 = reg().readConcrete(Register::X64::RSI, verbose);
    syscall.arg3 = reg().readConcrete(Register::X64::RDX, verbose);
    syscall.arg4 = reg().readConcrete(Register::X64::R10, verbose);
    syscall.arg5 = reg().readConcrete(Register::X64::R8, verbose);
    syscall.arg6 = reg().readConcrete(Register::X64::R9, verbose);

    if (m_showSyscalls) {
        log<INFO>() << "syscall: "
            << hexval(syscall.nr) << " ("
            << hexval(syscall.arg1) << ", "
            << hexval(syscall.arg2) << ", "
            << hexval(syscall.arg3) << ", "
            << hexval(syscall.arg4) << ", "
            << hexval(syscall.arg5) << ", "
            << hexval(syscall.arg6) << ")\n";
    }

    auto craxState = getPluginState(state);
    auto &pending = craxState->m_pendingOnExecuteSyscallEnd;

    // Schedule the syscall hook to be called before the next instruction is executed.
    uint64_t nextInsnAddr = i.address + i.size;
    pending[nextInsnAddr] = syscall;

    // Execute syscall hooks installed by the user.
    beforeSyscall.emit(state, pending[nextInsnAddr]);
}

void CRAX::onExecuteSyscallEnd(S2EExecutionState *state,
                               const Instruction &i,
                               SyscallCtx &syscall) {
    constexpr bool verbose = false;

    // The kernel has finished serving the system call,
    // and the return value is now placed in RAX.
    syscall.ret = reg().readConcrete(Register::X64::RAX, verbose);

    // Execute syscall hooks installed by the user.
    afterSyscall.emit(state, syscall);
}

void CRAX::onStateForkDecide(S2EExecutionState *state,
                             const ref<Expr> &condition,
                             bool &allowForking) {
    if (!m_concolicMode) {
        return;
    }

    setCurrentState(state);

    // The user sets `m_concolicMode` to true,
    // so we should disallow this fork by default.
    allowForking = false;

    // Let the modules of CRAX++ decide whether this fork should be done.
    onStateForkModuleDecide.emit(state, condition, allowForking);

    // We'll also check if current state forking was requested by CRAX.
    // If yes, then `state` should be in `m_allowedForkingStates`.
    allowForking |= m_allowedForkingStates.erase(state) == 1;
}

}  // namespace s2e::plugins::crax
