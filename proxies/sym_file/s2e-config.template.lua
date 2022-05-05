--[[
This is the main S2E configuration file
=======================================

This file was automatically generated by s2e-env at 2021-10-06 14:21:59.710454

Changes can be made by the user where appropriate.
]]--

-------------------------------------------------------------------------------
-- This section configures the S2E engine.
s2e = {
    logging = {
        -- Possible values include "all", "debug", "info", "warn" and "none".
        -- See Logging.h in libs2ecore.
        console = "info",
        logLevel = "info",
    },

    -- All the cl::opt options defined in the engine can be tweaked here.
    -- This can be left empty most of the time.
    -- Most of the options can be found in S2EExecutor.cpp and Executor.cpp.
    kleeArgs = {},
}

-- Declare empty plugin settings. They will be populated in the rest of
-- the configuration file.
plugins = {}
pluginsConfig = {}

-- Include various convenient functions
dofile('library.lua')

-------------------------------------------------------------------------------
-- This plugin contains the core custom instructions.
-- Some of these include s2e_make_symbolic, s2e_kill_state, etc.
-- You always want to have this plugin included.

add_plugin("BaseInstructions")
pluginsConfig.BaseInstructions = {
    logLevel="info",
}

-------------------------------------------------------------------------------
-- This plugin implements "shared folders" between the host and the guest.
-- Use it in conjunction with s2eget and s2eput guest tools in order to
-- transfer files between the guest and the host.

add_plugin("HostFiles")
pluginsConfig.HostFiles = {
    baseDirs = {
        "/home/aesophor/s2e/projects/sym_file",
    },
    allowWrite = true,
}

-------------------------------------------------------------------------------
-- This plugin provides support for virtual machine introspection and binary
-- formats parsing. S2E plugins can use it when they need to extract
-- information from binary files that are either loaded in virtual memory
-- or stored on the host's file system.

add_plugin("Vmi")
pluginsConfig.Vmi = {
    baseDirs = {
        "/home/aesophor/s2e/projects/sym_file",
        "/home/aesophor/s2e/images/debian-9.2.1-x86_64/guestfs",
    },
}

-------------------------------------------------------------------------------
-- This plugin provides various utilities to read from process memory.
-- In case it is not possible to read from guest memory, the plugin tries
-- to read static data from binary files stored in guestfs.
add_plugin("MemUtils")

-------------------------------------------------------------------------------
-- This plugin collects various execution statistics and sends them to a QMP
-- server that listens on an address:port configured by the S2E_QMP_SERVER
-- environment variable.
--
-- The "s2e run sym_file" command sets up such a server in order to display
-- stats on the dashboard.
--
-- You may also want to use this plugin to integrate S2E into a larger
-- system. The server could collect information about execution from different
-- S2E instances, filter them, and store them in a database.

add_plugin("WebServiceInterface")
pluginsConfig.WebServiceInterface = {
    statsUpdateInterval = 2
}

-------------------------------------------------------------------------------
-- This is the main execution tracing plugin.
-- It generates the ExecutionTracer.dat file in the s2e-last folder.
-- That files contains trace information in a binary format. Other plugins can
-- hook into ExecutionTracer in order to insert custom tracing data.
--
-- This is a core plugin, you most likely always want to have it.

add_plugin("ExecutionTracer")

-------------------------------------------------------------------------------
-- This plugin records events about module loads/unloads and stores them
-- in ExecutionTracer.dat.
-- This is useful in order to map raw program counters and pids to actual
-- module names.

add_plugin("ModuleTracer")

-------------------------------------------------------------------------------
-- This is a generic plugin that let other plugins communicate with each other.
-- It is a simple key-value store.
--
-- The plugin has several modes of operation:
--
-- 1. local: runs an internal store private to each instance (default)
-- 2. distributed: the plugin interfaces with an actual key-value store server.
-- This allows different instances of S2E to communicate with each other.

add_plugin("KeyValueStore")

-------------------------------------------------------------------------------
-- Records the program counter of executed translation blocks.
-- Generates a json coverage file. This file can be later processed by other
-- tools to generate line coverage information. Please refer to the S2E
-- documentation for more details.

add_plugin("TranslationBlockCoverage")
pluginsConfig.TranslationBlockCoverage = {
    writeCoverageOnStateKill = true,
    writeCoverageOnStateSwitch = true,
}

-------------------------------------------------------------------------------
-- Tracks execution of specific modules.
-- Analysis plugins are often interested only in small portions of the system,
-- typically the modules under analysis. This plugin filters out all core
-- events that do not concern the modules under analysis. This simplifies
-- code instrumentation.
-- Instead of listing individual modules, you can also track all modules by
-- setting configureAllModules = true

add_plugin("ModuleExecutionDetector")
pluginsConfig.ModuleExecutionDetector = {
    mod_0 = {
        moduleName = "target",
    },
    logLevel="info"
}

-------------------------------------------------------------------------------
-- This plugin controls the forking behavior of S2E.

add_plugin("ForkLimiter")
pluginsConfig.ForkLimiter = {
    -- How many times each program counter is allowed to fork.
    -- -1 for unlimited.
    maxForkCount = -1,

    -- How many seconds to wait before allowing an S2E process
    -- to spawn a child. When there are many states, S2E may
    -- spawn itself into multiple processes in order to leverage
    -- multiple cores on the host machine. When an S2E process A spawns
    -- a process B, A and B each get half of the states.
    --
    -- In some cases, when states fork and terminate very rapidly,
    -- one can see flash crowds of S2E instances. This decreases
    -- execution efficiency. This parameter forces S2E to wait a few
    -- seconds so that more states can accumulate in an instance
    -- before spawning a process.
    processForkDelay = 5,
}

-------------------------------------------------------------------------------
-- This plugin tracks execution of processes.
-- This is the preferred way of tracking execution and will eventually replace
-- ModuleExecutionDetector.

add_plugin("ProcessExecutionDetector")
pluginsConfig.ProcessExecutionDetector = {
    moduleNames = {
        "target",
    },
}

-------------------------------------------------------------------------------
-- Keeps for each state/process an updated map of all the loaded modules.
add_plugin("ModuleMap")
pluginsConfig.ModuleMap = {
  logLevel = "info"
}


-------------------------------------------------------------------------------
-- Keeps for each process in ProcessExecutionDetector an updated map
-- of memory regions.
add_plugin("MemoryMap")
pluginsConfig.MemoryMap = {
  logLevel = "info"
}


-------------------------------------------------------------------------------
-- MultiSearcher is a top-level searcher that allows switching between
-- different sub-searchers.
add_plugin("MultiSearcher")

-- CUPA stands for Class-Uniform Path Analysis. It is a searcher that groups
-- states into classes. Each time the searcher needs to pick a state, it first
-- chooses a class, then picks a state in that class. Classes can further be
-- subdivided into subclasses.
--
-- The advantage of CUPA over other searchers is that it gives similar weights
-- to different parts of the program. If one part forks a lot, a random searcher
-- would most likely pick a state from that hotspot, decreasing the probability
-- of choosing another state that may have better chance of covering new code.
-- CUPA avoids this problem by grouping similar states together.

add_plugin("CUPASearcher")
pluginsConfig.CUPASearcher = {
    -- The order of classes is important, please refer to the plugin
    -- source code and documentation for details on how CUPA works.
    classes = {
        -- This ensures that states run for a certain amount of time.
        -- Otherwise too frequent state switching may decrease performance.
        "batch",

        -- A program under test may be composed of several binaries.
        -- We want to give equal chance to all binaries, even if some of them
        -- fork a lot more than others.
        "pagedir",

        -- Finally, group states by program counter at fork.
        "pc",
    },

    logLevel="info",
    enabled = true,

    -- Delay (in seconds) before switching states (when used with the "batch" class).
    -- A very large delay becomes similar to DFS (current state keeps running
    -- until it is terminated).
    batchTime = 5
}


-------------------------------------------------------------------------------
-- Function models help drastically reduce path explosion. A model is an
-- expression that efficiently encodes the behavior of a function. In imperative
-- languages, functions often have if-then-else branches and loops, which
-- may cause path explosion. A model compresses this into a single large
-- expression. Models are most suitable for side-effect-free functions that
-- fork a lot. Please refer to models.lua and the documentation for more details.

--add_plugin("StaticFunctionModels")

--pluginsConfig.StaticFunctionModels = {
--  modules = {}
--}

--g_function_models = {}
--safe_load('models.lua')
--pluginsConfig.StaticFunctionModels.modules = g_function_models


add_plugin("FunctionModels")

pluginsConfig.FunctionModels = {

}


-------------------------------------------------------------------------------
-- This generates test cases when a state crashes or terminates.
-- If symbolic inputs consist of symbolic files, the test case generator writes
-- concrete files in the S2E output folder. These files can be used to
-- demonstrate the crash in a program, added to a test suite, etc.

--add_plugin("TestCaseGenerator")
--pluginsConfig.TestCaseGenerator = {
--    generateOnStateKill = true,
--    generateOnSegfault = true
--}


-------------------------------------------------------------------------------
-- The screenshot plugin records a screenshot of the guest into screenshotX.png,
-- where XX is the path number. You can configure the interval here:
--add_plugin("Screenshot")
--pluginsConfig.Screenshot = {
--    period = 5
--}


-- ========================================================================= --
-- ============== Target-specific configuration begins here. =============== --
-- ========================================================================= --

-------------------------------------------------------------------------------
-- LinuxMonitor is a plugin that monitors Linux events and exposes them
-- to other plugins in a generic way. Events include process load/termination,
-- thread events, signals, etc.
--
-- LinuxMonitor requires a custom Linux kernel with S2E extensions. This kernel
-- (and corresponding VM image) can be built with S2E tools. Please refer to
-- the documentation for more details.

add_plugin("LinuxMonitor")
pluginsConfig.LinuxMonitor = {
    -- Kill the execution state when it encounters a segfault
    terminateOnSegfault = true,

    -- Kill the execution state when it encounters a trap
    terminateOnTrap = true,
}


-- ========================================================================= --
-- ============== User-specific scripts begin here ========================= --
-- ========================================================================= --


-------------------------------------------------------------------------------
-- This plugin exposes core S2E engine functionality to LUA scripts.
-- In particular, it provides the g_s2e global variable, which works similarly
-- to C++ plugins.
-------------------------------------------------------------------------------
add_plugin("LuaBindings")

-------------------------------------------------------------------------------
-- Exposes S2E engine's core event.
-- These are similar to events in CorePlugin.h. Please refer to
-- the LuaCoreEvents.cpp source file for a list of availble events.
-------------------------------------------------------------------------------
add_plugin("LuaCoreEvents")

-- This configuration shows an example that kills states if they fork in
-- a specific module.
--[[
pluginsConfig.LuaCoreEvents = {
    -- This annotation is called in case of a fork. It should return true
    -- to allow the fork and false to prevent it.
    onStateForkDecide = "onStateForkDecide"
}

function onStateForkDecide(state)
   mmap = g_s2e:getPlugin("ModuleMap")
   mod = mmap:getModule(state)
   if mod ~= nil then
      name = mod:getName()
      if name == "mymodule" then
          state:kill(0, "forked in mymodule")
      end

      if name == "myothermodule" then
          return false
      end
   end
   return true
end
--]]

-------------------------------------------------------------------------------
-- CRAX exploit generation engine.
-- See source/s2e/libs2eplugins/src/s2e/Plugins/CRAX/CRAX.cpp
-------------------------------------------------------------------------------
add_plugin("CRAX")

pluginsConfig.CRAX = {
    -- Core Settings
    showInstructions = false,
    showSyscalls = true,
    concolicMode = true,

    -- Filenames
    elfFilename = "./target",
    libcFilename = "./libc-2.24.so",
    ldFilename = "./ld-2.24.so",

    -- Modules of CRAX++ that you wish to load
    modules = {
        "GuestOutput",
        --"IOStates",
        --"DynamicRop",
        --"SymbolicAddressMap",
    },

    -- Module config
    modulesConfig = {
        -- Used for stage2 concolic execution
        IOStates = {
            canary = __CANARY__,
            elfBase = __ELF_BASE__,
            stateInfoList = __STATE_INFO_LIST__,
        },
    },

    -- The exploitaion techniques that your exploit will use
    techniques = {
        "Ret2stack",
        --"Ret2csu",
        --"BasicStackPivoting",
        --"AdvancedStackPivoting",
        --"Ret2syscall",
        --"GotLeakLibc",
        --"OneGadget"
        --"Ret2syscall"
    },
}
