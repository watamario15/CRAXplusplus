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

#ifndef S2E_PLUGINS_CRAX_H
#define S2E_PLUGINS_CRAX_H

#include <s2e/Plugin.h>
#include <s2e/Plugins/Core/BaseInstructions.h>
#include <s2e/Plugins/OSMonitors/ModuleDescriptor.h>
#include <s2e/Plugins/OSMonitors/Linux/LinuxMonitor.h>
#include <s2e/Plugins/CRAX/API/Register.h>
#include <s2e/Plugins/CRAX/API/Memory.h>
#include <s2e/Plugins/CRAX/API/Disassembler.h>
#include <s2e/Plugins/CRAX/API/Logging.h>
#include <s2e/Plugins/CRAX/Modules/Module.h>
#include <s2e/Plugins/CRAX/Techniques/Technique.h>
#include <s2e/Plugins/CRAX/Exploit.h>
#include <s2e/Plugins/CRAX/ExploitGenerator.h>

#include <pybind11/embed.h>

#include <cassert>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace s2e::plugins::crax {

// A plugin state contains per-state information of a plugin,
// so CRAXState holds information specific to a particular S2EExecutionState.
//
// In addition, CRAX supports "modules" (or you can think of them as plugin),
// so a CRAXState further splits the per-state information at module level.
class CRAXState : public PluginState {
    using ModuleStateMap = std::map<const Module *, std::unique_ptr<ModuleState>>;

public:
    CRAXState() : m_moduleState() {}
    virtual ~CRAXState() = default;

    static PluginState *factory(Plugin *, S2EExecutionState *) {
        return new CRAXState();
    }

    virtual CRAXState *clone() const override {
        CRAXState *newState = new CRAXState();
        for (const auto &entry : m_moduleState) {
            const Module *module = entry.first;
            std::unique_ptr<ModuleState> newModuleState(entry.second->clone());
            newState->m_moduleState.insert(std::make_pair(module, std::move(newModuleState)));
        }
        return newState;
    }


    ModuleState *getModuleState(Module *module, ModuleStateFactory factory) {
        auto it = m_moduleState.find(module);
        if (it == m_moduleState.end()) {
            std::unique_ptr<ModuleState> newModuleState(factory(module, this));
            assert(newModuleState);
            ModuleState *ret = newModuleState.get();
            m_moduleState.insert(std::make_pair(module, std::move(newModuleState)));
            return ret;
        }
        return it->second.get();
    }

private:
    ModuleStateMap m_moduleState;
};



class CRAX : public Plugin, IPluginInvoker {
    S2E_PLUGIN

public:
    CRAX(S2E *s2e);
    void initialize();


    [[nodiscard, gnu::always_inline]]
    inline S2EExecutionState *fork(S2EExecutionState &state) {
        if (m_disableNativeForking) {
            m_allowedForkingStates.insert(&state);
        }

        if (state.needToJumpToSymbolic()) {
            state.jumpToSymbolic();
        }

        S2EExecutor::StatePair sp = s2e()->getExecutor()->fork(state);
        assert(sp.second && "CRAX: failed to fork state!");
        return static_cast<S2EExecutionState *>(sp.second);
    }

    // Here we define it again in the derived class
    // to unhide the overloaded version from Plugin::getPluginState(). 
    PluginState *getPluginState(S2EExecutionState *state, PluginStateFactory f) const {
        return Plugin::getPluginState(state, f);
    }

    // This version is intended to provide a user-friendly interface.
    [[nodiscard, gnu::always_inline]]
    inline CRAXState *getPluginState(S2EExecutionState *state) const {
        // See: libs2ecore/include/s2e/Plugin.h
        return static_cast<CRAXState *>(Plugin::getPluginState(state, &CRAXState::factory));
    }

    // This is a shortcut to perform `getPluginState()` + `getModuleState()`.
    template <typename T>
    [[nodiscard]]
    typename T::State *getPluginModuleState(S2EExecutionState *state,
                                            const T *mod) const {
        CRAXState *craxState = getPluginState(state);
        assert(craxState && "Unable to get plugin state for CRAX!?");

        ModuleState *modState = mod->getModuleState(craxState, &T::State::factory);
        assert(modState && "Unable to get plugin module state!");

        return static_cast<typename T::State *>(modState);
    }


    [[nodiscard]]
    S2EExecutionState *getCurrentState() { return m_currentState; }

    void setCurrentState(S2EExecutionState *state) { m_currentState = state; }

    void setShowInstructions(bool showInstructions) { m_showInstructions = showInstructions; }

    void setShowSyscalls(bool showSyscalls) { m_showSyscalls = showSyscalls; }

    [[nodiscard]]
    bool isNativeForkingDisabled() const { return m_disableNativeForking; }

    [[nodiscard]]
    uint64_t getUserSpecifiedCanary() const { return m_userSpecifiedCanary; }

    [[nodiscard]]
    uint64_t getUserSpecifiedElfBase() const { return m_userSpecifiedElfBase; }

    [[nodiscard]]
    Register &reg() { return m_register; }

    [[nodiscard]]
    Memory &mem() { return m_memory; }

    [[nodiscard]]
    Disassembler &getDisassembler() { return m_disassembler; }

    [[nodiscard]]
    Exploit &getExploit() { return m_exploit; }

    [[nodiscard]]
    std::vector<Technique *> getTechniques() {
        std::vector<Technique *> ret(m_techniques.size());
        std::transform(m_techniques.begin(),
                       m_techniques.end(),
                       ret.begin(),
                       [](const auto &p) { return p.get(); });
        return ret;
    }

    [[nodiscard]]
    static Module *getModule(const std::string &name) {
        auto it = Module::s_mapper.find(name);
        return (it != Module::s_mapper.end()) ? it->second : nullptr;
    }

    [[nodiscard]]
    static Technique *getTechnique(const std::string &name) {
        auto it = Technique::s_mapper.find(name);
        return (it != Technique::s_mapper.end()) ? it->second : nullptr;
    }


    [[nodiscard]]
    uint64_t getTargetProcessPid() const { return m_targetProcessPid; }

    [[nodiscard]]
    bool isCallSiteOf(uint64_t instructionAddr,
                      const std::string &symbol) const;

    [[nodiscard]]
    std::string getBelongingSymbol(uint64_t instructionAddr) const;


    // clang-format off
    sigc::signal<void,
                 S2EExecutionState*,
                 const Instruction&>
        beforeInstruction;

    sigc::signal<void,
                 S2EExecutionState*,
                 const Instruction&>
        afterInstruction;

    sigc::signal<void,
                 S2EExecutionState*,
                 SyscallCtx&>
        beforeSyscall;

    sigc::signal<void,
                 S2EExecutionState*,
                 const SyscallCtx&>
        afterSyscall;

    sigc::signal<void,
                 S2EExecutionState*,
                 const klee::ref<klee::Expr>&,
                 bool&>
        onStateForkModuleDecide;

    sigc::signal<void> beforeExploitGeneration;
    // clang-format on


    // Embedded Python interpreter from pybind11 library.
    static pybind11::scoped_interpreter s_pybind11;
    static pybind11::module s_pwnlib;

private:
    // Allow the guest to communicate with this plugin using s2e_invoke_plugin
    virtual void handleOpcodeInvocation(S2EExecutionState *state,
                                        uint64_t guestDataPtr,
                                        uint64_t guestDataSize) {}

    void onSymbolicRip(S2EExecutionState *state,
                       klee::ref<klee::Expr> symbolicRip,
                       uint64_t concreteRip,
                       bool &concretize,
                       CorePlugin::symbolicAddressReason reason);

    void onProcessLoad(S2EExecutionState *state,
                       uint64_t cr3,
                       uint64_t pid,
                       const std::string &imageFileName);

    void onModuleLoad(S2EExecutionState *state,
                      const ModuleDescriptor &md);

    void onTranslateInstructionStart(ExecutionSignal *onInstructionExecute,
                                     S2EExecutionState *state,
                                     TranslationBlock *tb,
                                     uint64_t pc);

    void onTranslateInstructionEnd(ExecutionSignal *onInstructionExecute,
                                   S2EExecutionState *state,
                                   TranslationBlock *tb,
                                   uint64_t pc);

    void onExecuteInstructionStart(S2EExecutionState *state,
                                   uint64_t pc);

    void onExecuteInstructionEnd(S2EExecutionState *state,
                                 uint64_t pc);

    void onExecuteSyscallStart(S2EExecutionState *state,
                               uint64_t pc);

    void onExecuteSyscallEnd(S2EExecutionState *state,
                             uint64_t pc,
                             SyscallCtx &syscall);

    void onStateForkDecide(S2EExecutionState *state,
                           const klee::ref<klee::Expr> &condition,
                           bool &allowForking);


    // S2E
    S2EExecutionState *m_currentState;
    LinuxMonitor *m_linuxMonitor;

    // CRAX's config options.
    bool m_showInstructions;
    bool m_showSyscalls;
    bool m_disableNativeForking;
    uint64_t m_userSpecifiedCanary;
    uint64_t m_userSpecifiedElfBase;

    // CRAX's attributes.
    Register m_register;
    Memory m_memory;
    Disassembler m_disassembler;
    Exploit m_exploit;
    ExploitGenerator m_exploitGenerator;
    std::vector<std::unique_ptr<Module>> m_modules;
    std::vector<std::unique_ptr<Technique>> m_techniques;

    uint64_t m_targetProcessPid;
    std::map<uint64_t, SyscallCtx> m_scheduledAfterSyscallHooks;  // <pc, SyscallCtx>
    std::unordered_set<S2EExecutionState *> m_allowedForkingStates;
};

}  // namespace s2e::plugins::crax

#endif  // S2E_PLUGINS_CRAX_H
