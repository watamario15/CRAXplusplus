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

#include <unistd.h>

#include "IOStates.h"

using namespace klee;

namespace s2e::plugins::crax {

const std::array<std::string, IOStates::LeakType::LAST> IOStates::s_leakTypes = {{
    "unknown", "code", "libc", "heap", "stack", "canary"
}};

IOStates::IOStates(CRAX &ctx)
    : Module(ctx),
      m_canary(),
      m_leakTargets() {
    // Install input state syscall hook.
    ctx.beforeSyscallHooks.connect(
            sigc::mem_fun(*this, &IOStates::inputStateHookTopHalf));

    ctx.afterSyscallHooks.connect(
            sigc::mem_fun(*this, &IOStates::inputStateHookBottomHalf));

    // Install output state syscall hook.
    ctx.afterSyscallHooks.connect(
            sigc::mem_fun(*this, &IOStates::outputStateHook));

    // Determine which base address(es) must be leaked
    // according to checksec of the target binary.
    const auto &checksec = m_ctx.getExploit().getElf().getChecksec();
    if (checksec.hasCanary) {
        m_leakTargets.push_back(IOStates::LeakType::CANARY);
    }
    if (checksec.hasPIE) {
        m_leakTargets.push_back(IOStates::LeakType::CODE);
    }
    // XXX: ASLR -> libc

    // If stack canary is enabled, install a hook to intercept canary values.
    if (checksec.hasCanary) {
        ctx.afterInstructionHooks.connect(
                sigc::mem_fun(*this, &IOStates::maybeInterceptStackCanary));

        ctx.beforeInstructionHooks.connect(
                sigc::mem_fun(*this, &IOStates::onStackChkFailed));

        ctx.onStateForkModuleDecide.connect(
                sigc::mem_fun(*this, &IOStates::onStateForkModuleDecide));
    }
}


void IOStates::inputStateHookTopHalf(S2EExecutionState *inputState,
                                     SyscallCtx &syscall) {
    if (syscall.nr != 0 || syscall.arg1 != STDIN_FILENO) {
        return;
    }

    m_ctx.setCurrentState(inputState);
    auto bufInfo = analyzeLeak(inputState, syscall.arg2, syscall.arg3);

    auto &os = log<WARN>();
    os << " ---------- Analyzing input state ----------\n";
    for (size_t i = 0; i < bufInfo.size(); i++) {
        os << "[" << IOStates::s_leakTypes[i] << "]: ";
        for (uint64_t offset : bufInfo[i]) {
            os << klee::hexval(offset) << ' ';
        }
        os << '\n';
    }


    // Now we assume that the first offset can help us
    // successfully leak the address we want.
    auto modState = m_ctx.getPluginModuleState(inputState, this);

    if (modState->currentLeakTargetIdx >= m_leakTargets.size()) {
        log<WARN>() << "No more leak targets :^)\n";
        return;
    }

    IOStates::LeakType currentLeakType
        = m_leakTargets[modState->currentLeakTargetIdx];

    log<WARN>() << "Current leak target: " << s_leakTypes[currentLeakType] << '\n';

    if (bufInfo[currentLeakType].empty()) {
        log<WARN>() << "No leak targets in current input state.\n";
        return;
    }

    // For each offset, fork a new state
    // XXX: Optimize this with a custom searcher (?)
    for (uint64_t offset : bufInfo[currentLeakType]) {
        // If we're leaking the canary, then we need to overwrite
        // the least significant bit of the canary, so offset++.
        if (currentLeakType == IOStates::LeakType::CANARY) {
            offset++;
        }

        S2EExecutionState *forkedState = m_ctx.fork(*inputState);

        log<WARN>()
            << "forked a new state for offset " << hexval(offset)
            << "(id=" << forkedState->getID() << ")\n";

        // Hijack sys_read(0, buf, len), setting len to `value`.
        // Note that the forked state is currently in symbolic mode,
        // so we have to write a klee::ConstantExpr instead of uint64_t.
        ref<Expr> value = ConstantExpr::create(offset, Expr::Int64);
        forkedState->regs()->write(CPU_OFFSET(regs[Register::X64::RDX]), value);

        auto *forkedModState = m_ctx.getPluginModuleState(forkedState, this);
        modState->leakableOffset = offset;
        forkedModState->leakableOffset = offset;
    }
}

void IOStates::inputStateHookBottomHalf(S2EExecutionState *inputState,
                                        const SyscallCtx &syscall) {
    if (syscall.nr != 0 || syscall.arg1 != STDIN_FILENO) {
        return;
    }

    m_ctx.setCurrentState(inputState);

    std::vector<uint8_t> buf
        = m_ctx.mem().readConcrete(syscall.arg2, syscall.arg3, /*concretize=*/false);

    auto modState = m_ctx.getPluginModuleState(inputState, this);
    InputStateInfo stateInfo;
    stateInfo.buf = std::move(buf);

    if (modState->leakableOffset) {
        // `inputStateHookBottomHalf()` -> `analyzeLeak()` has found
        // something that can be leaked and stored it in `modState->leakableOffset`.
        stateInfo.offset = modState->leakableOffset;
    } else {
        // Nothing to leak, set the offset as the original length of sys_read().
        stateInfo.offset = syscall.arg3;
    }

    modState->leakableOffset = 0;
    modState->lastInputStateInfoIdx = modState->stateInfoList.size();
    modState->stateInfoList.push_back(std::move(stateInfo));
}

void IOStates::outputStateHook(S2EExecutionState *outputState,
                               const SyscallCtx &syscall) {
    if (syscall.nr != 1 || syscall.arg1 != STDOUT_FILENO) {
        return;
    }

    m_ctx.setCurrentState(outputState);

    auto outputStateInfoList = detectLeak(outputState, syscall.arg2, syscall.arg3);
    auto modState = m_ctx.getPluginModuleState(outputState, this);

    OutputStateInfo stateInfo;
    stateInfo.valid = false;

    if (outputStateInfoList.size()) {
        stateInfo.valid = true;
        stateInfo.bufIndex = outputStateInfoList.front().bufIndex;
        stateInfo.baseOffset = outputStateInfoList.front().baseOffset;
        stateInfo.leakType = outputStateInfoList.front().leakType;

        log<WARN>()
            << "*** WARN *** detected leak: ("
            << IOStates::s_leakTypes[stateInfo.leakType] << ", "
            << klee::hexval(stateInfo.bufIndex) << ", "
            << klee::hexval(stateInfo.baseOffset) << ")\n";

        modState->currentLeakTargetIdx++;
    }

    modState->stateInfoList.push_back(std::move(stateInfo));
}


void IOStates::maybeInterceptStackCanary(S2EExecutionState *state,
                                         const Instruction &i) {
    static bool hasReachedMain = false;

    // If we've already intercepted the canary of the target ELF,
    // then we don't need to proceed anymore.
    if (m_canary) {
        return;
    }

    if (i.address == m_ctx.getExploit().getElf().getRuntimeAddress("main")) {
        hasReachedMain = true;
    }

    if (hasReachedMain &&
        i.mnemonic == "mov" && i.opStr == "rax, qword ptr fs:[0x28]") {
        uint64_t canary = m_ctx.reg().readConcrete(Register::X64::RAX);
        m_canary = canary;

        log<WARN>()
            << '[' << klee::hexval(i.address) << "] "
            << "Intercepted canary: " << klee::hexval(canary) << '\n';
    }
}

void IOStates::onStateForkModuleDecide(S2EExecutionState *state,
                                       bool *allowForking) {
    // If S2E native forking is enabled, then it will automatically fork
    // at canary check.
    if (!m_ctx.isNativeForkingDisabled()) {
        return;
    }

    // If the current branch instruction is the one before `call __stack_chk_fail@plt`,
    // then allow it to fork the current state.
    //
    // -> 401289:       74 05                   je     401290 <main+0xa2>
    //    40128b:       e8 20 fe ff ff          call   4010b0 <__stack_chk_fail@plt>
    //    401290:       c9                      leave
    uint64_t pc = state->regs()->getPc();
    std::optional<Instruction> i = m_ctx.getDisassembler().disasm(pc);  
    assert(i && "Disassemble failed?");

    // Look ahead the next instruction.
    if (!m_ctx.isCallSiteOf(pc + i->size, "__stack_chk_fail")) {
        *allowForking = false;
    } else {
        log<WARN>() << "Allowing fork before __stack_chk_fail@plt\n";
        *allowForking &= true;  // use &= so previous result is not overwritten.
    }
}

void IOStates::onStackChkFailed(S2EExecutionState *state,
                                const Instruction &i) {
    const uint64_t stackChkFailPlt
        = m_ctx.getExploit().getElf().getRuntimeAddress("__stack_chk_fail");

    if (i.address == stackChkFailPlt) {
        // The program has reached __stack_chk_fail and
        // there's no return, so kill it.
        g_s2e->getExecutor()->terminateState(*state, "reached __stack_chk_fail@plt");
    }
}


std::array<std::vector<uint64_t>, IOStates::LeakType::LAST>
IOStates::analyzeLeak(S2EExecutionState *inputState, uint64_t buf, uint64_t len) {
    auto mapInfo = m_ctx.mem().getMapInfo();
    uint64_t canary = m_canary;
    std::array<std::vector<uint64_t>, IOStates::LeakType::LAST> bufInfo;

    for (uint64_t i = 0; i < len; i += 8) {
        uint64_t value = u64(m_ctx.mem().readConcrete(buf + i, 8, /*concretize=*/false));
        //log<WARN>() << "addr = " << klee::hexval(buf + i) << " value = " << klee::hexval(value) << '\n';
        if (m_ctx.getExploit().getElf().getChecksec().hasCanary && value == canary) {
            bufInfo[LeakType::CANARY].push_back(i);
        } else {
            for (const auto &region : mapInfo) {
                if (value >= region.start && value <= region.end) {
                    bufInfo[getLeakType(region.image)].push_back(i);
                }
            }
        }
    }

    return bufInfo;
}

std::vector<IOStates::OutputStateInfo>
IOStates::detectLeak(S2EExecutionState *outputState, uint64_t buf, uint64_t len) {
    auto mapInfo = m_ctx.mem().getMapInfo();
    uint64_t canary = m_canary;
    std::vector<IOStates::OutputStateInfo> leakInfo;

    for (uint64_t i = 0; i < len; i += 8) {
        uint64_t value = u64(m_ctx.mem().readConcrete(buf + i, 8, /*concretize=*/false));
        //log<WARN>() << "addr = " << klee::hexval(buf + i) << " value = " << klee::hexval(value) << '\n';
        IOStates::OutputStateInfo info;
        info.valid = true;

        if (m_ctx.getExploit().getElf().getChecksec().hasCanary && (value & ~0xff) == canary) {
            info.bufIndex = i + 1;
            info.baseOffset = 0;
            info.leakType = LeakType::CANARY;
            leakInfo.push_back(info);
        } else {
            for (const auto &region : mapInfo) {
                if (value >= region.start && value <= region.end) {
                    info.bufIndex = i;
                    info.baseOffset = value - region.start;
                    info.leakType = getLeakType(region.image);
                    leakInfo.push_back(info);
                }
            }
        }
    }

    return leakInfo;
}

void IOStates::print() const {
    auto modState = m_ctx.getPluginModuleState(m_ctx.getCurrentState(), this);

    auto &os = log<WARN>();
    os << "Dumping IOStates: [";

    for (size_t i = 0; i < modState->stateInfoList.size(); i++) {
        if (const auto &inputStateInfo = std::get_if<InputStateInfo>(&modState->stateInfoList[i])) {
            os << "input";
        } else {
            os << "output";
        }
        if (i != modState->stateInfoList.size() - 1) {
            os << ", ";
        }
    }
    os << "]\n";
}


IOStates::LeakType IOStates::getLeakType(const std::string &image) const {
    if (image == m_ctx.getExploit().getElfFilename()) {
        return IOStates::LeakType::CODE;
    } else if (image == "[shared library]") {
        return IOStates::LeakType::LIBC;
    } else if (image == "[stack]") {
        return IOStates::LeakType::STACK;
    } else {
        return IOStates::LeakType::UNKNOWN;
    }
}

}  // namespace s2e::plugins::crax
