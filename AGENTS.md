# Repository Guidelines

---

# 🧠 ULTRA AGENT OPERATING PROTOCOL (CRITICAL)

This project uses **Ultra AI Runtime (UAIR)** as the primary cognition and querying system.

## ⚡ Command Priority Rules

1. **ALWAYS prefer Ultra commands FIRST**
2. Only fallback to traditional tools (grep, file reads, etc.) if Ultra fails
3. Never bypass Ultra if a command exists for the task

---

## 🚀 Daemon Lifecycle Rules (MANDATORY)

Before ANY query or analysis:

1. Check daemon status:

   ```
   ultra ai_status
   ```

2. If not running:

   ```
   ultra wake_ai
   ```

👉 Agents MUST ensure daemon is active before proceeding

---

## 🧠 Context Acquisition Rules

At project root:

```
ultra context --ast .
```

Additionally:

* Read `.ultra.context.json` file at root
* This provides **full project structural + semantic understanding**

---

## 🔍 Symbol Awareness Rule (VERY IMPORTANT)

Before querying any symbol:

Read:

```
E:\Projects\UltraInfinity\.ultra\ai\symbols.tbl
```

👉 This ensures:

* Only real symbols are queried
* Prevents hallucinated references

---

## 🧰 Stable Ultra Commands (PRIMARY TOOLSET)

These commands are **trusted and must be preferred**:

```
ultra wake_ai                      Start UAIR daemon (fast persisted graph load)
ultra ai_status [--verbose]        Query daemon snapshot (no recompute)
ultra ai_query <target>            Query indexed file/symbol from daemon memory
ultra ai_source <file>             Fetch raw source for indexed file
ultra ai_impact <target>           Analyze transitive impact for file/symbol
ultra rebuild_ai                   Trigger daemon full rebuild
ultra sleep_ai                     Stop daemon
ultra ai_verify                    Verify incremental vs rebuild index hash
ultra context --query <Symbol>     Query symbol context (Eg: AiRuntimeManager)
```

---

## ⚠️ Build Safety Rule (CRITICAL)

Before running ANY build:

```
ultra sleep_ai
```

👉 Reason:

* Prevent `.exe` file lock conflicts
* Avoid daemon interference during compilation

---

## 🔁 Recommended Execution Flow

```
1. ultra ai_status
2. ultra wake_ai (if needed)
3. ultra context --ast .
4. Read symbols.tbl
5. ultra ai_query / ai_impact / ai_source
6. Perform task
7. ultra sleep_ai (before build)
```

---

# 🏗️ Project Structure & Module Organization

* `src/` contains the C++ core, grouped by domain (`ai/`, `cli/`, `runtime/`, `memory/`, `metrics/`, `platform/`)
* `include/` holds public headers (`ast/`, `context/`, `ultra/`, `utils/`)
* `tests/` is the main GoogleTest suite
* `tui/` and `newui/ultra_infinity/` are Python/Textual terminal UIs
* `third_party/` vendors Tree-sitter and grammar sources

---

# ⚙️ Build, Test, and Development Commands

* `cmake -S . -B build`
* `cmake --build build --config Release`
* `ctest --test-dir build -C Release --output-on-failure`
* `ctest --test-dir build -C Release -R "ExecutionKernel|UltraLoop"`
* `cd tui; python app.py`
* `cd newui/ultra_infinity; python app.py`

---

# 🧑‍💻 Coding Style & Naming Conventions

* Warnings as errors enforced
* 2-space indentation (C++)
* 4-space indentation (Python)
* `PascalCase` → C++ types/files
* `snake_case` → Python modules
* Explicit namespaces (`ultra::cli`, etc.)

---

# 🧪 Testing Guidelines

* Use GoogleTest in `tests/`
* Keep tests deterministic
* Avoid `.ultra/` or `.ultra_daemon/` dependencies
* Manual verification required for UI changes

---

# 🔁 Commit & Pull Request Guidelines

* Use concise, imperative commit messages
* Include validation steps in PRs
* Add screenshots for UI changes
* NEVER commit:

  * `build/`
  * `.ultra/`
  * `.ultra_daemon/`
  * `__pycache__/`
  * `.db`, `.log`
