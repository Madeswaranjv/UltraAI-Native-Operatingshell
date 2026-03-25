"""
EngineStreamer — drives all visual output and cognitive loop simulation.

Each public method maps to one Ultra execution path:
  run_user_driven_pipeline()    → USER-DRIVEN mode
  run_architectural_pipeline()  → ARCHITECTURAL mode

Replace the _call_backend_* stubs with real ultra subprocess / IPC calls
once the C++ bridge is ready. All animations and effects are preserved.
"""
from __future__ import annotations
import asyncio
import json
import os
import random
import shutil
import tempfile
from typing import Optional

from utils.anime_words import ANIME_WORDS
from utils.thinking_states import THINKING_FRAMES, LOOP_STATE_FRAMES
from core.state_manager import AppState
from core.intent_parser import IntentPayload


class EngineStreamer:

    def __init__(self, output_component, state_manager):
        self.output = output_component
        self.sm = state_manager
        self.session = None
        self._used_phrases: set = set()

    def set_session(self, session) -> None:
        self.session = session

    # ── Anime phrase pool ────────────────────────────────────────────────────

    def _phrase(self) -> str:
        available = list(set(ANIME_WORDS) - self._used_phrases)
        if not available:
            self._used_phrases.clear()
            available = list(ANIME_WORDS)
        phrase = random.choice(available)
        self._used_phrases.add(phrase)
        return phrase

    # ── Shared animations ────────────────────────────────────────────────────

    async def _thinking_animation(self, cycles: int = 8) -> None:
        for i in range(cycles):
            frame = THINKING_FRAMES[i % len(THINKING_FRAMES)]
            self.output.update_active(frame)
            await asyncio.sleep(0.18)

    async def _loop_state_animation(self, state_name: str, cycles: int = 4) -> None:
        for i in range(cycles):
            frame = LOOP_STATE_FRAMES[i % len(LOOP_STATE_FRAMES)]
            self.output.update_active(f"{frame}  {state_name.upper()}")
            await asyncio.sleep(0.15)

    async def _stream_text(self, text: str, prefix: str = "", delay: float = 0.03) -> str:
        """Stream text word-by-word, returning the full accumulated string."""
        words = text.split(" ")
        accumulated = prefix
        for word in words:
            accumulated += word + " "
            self.output.update_active(accumulated)
            await asyncio.sleep(delay)
        return accumulated

    async def _phase_line(self, accumulated: str, label: str, detail: str = "") -> str:
        phrase = self._phrase()
        line = f"  [ {phrase} ]  {label}"
        if detail:
            line += f"  →  {detail}"
        line += "\n"
        accumulated += line
        self.output.update_active(accumulated)
        await asyncio.sleep(0.45)
        return accumulated

    # ── USER-DRIVEN pipeline ─────────────────────────────────────────────────

    async def run_user_driven_pipeline(
        self, prompt: str, intent: IntentPayload, session
    ) -> None:
        self.output.mount_new_response()
        acc = ""

        # ── INTENT PARSE ──────────────────────────────────────────────────
        self.sm.set_state(AppState.PARSING_INTENT)
        await self._loop_state_animation("parsing intent", 5)
        acc += f"INTENT PARSED\n"
        acc += f"  action       →  {intent.action}\n"
        acc += f"  targets      →  {', '.join(intent.targets) if intent.targets else 'unspecified'}\n"
        acc += f"  goal         →  {intent.goal_summary}\n"
        acc += f"  risk level   →  {intent.risk_level}\n"
        if intent.constraints:
            acc += f"  constraints  →  {', '.join(intent.constraints)}\n"
        acc += "\n"
        self.output.update_active(acc)
        await asyncio.sleep(0.4)

        # ── STRATEGY PLAN ─────────────────────────────────────────────────
        self.sm.set_state(AppState.PLANNING)
        await self._loop_state_animation("planning", 4)
        acc += "STRATEGY PLAN\n"
        acc = await self._phase_line(acc, "L11 Intent Runtime", "goal structured")
        acc = await self._phase_line(acc, "L12 Strategy Planner", f"style: {session.policy.strategy_style}")
        acc += "\n"

        # ── MICRO PLAN ────────────────────────────────────────────────────
        self.sm.set_state(AppState.MICRO_PLANNING)
        await self._loop_state_animation("micro-planning", 4)
        tasks = self._generate_micro_tasks(intent)
        acc += "MICRO TASK GRAPH\n"
        for i, task in enumerate(tasks, 1):
            acc += f"  [{i}] {task}\n"
            self.output.update_active(acc)
            await asyncio.sleep(0.2)
        acc += "\n"

        # ── GOVERNANCE CHECK ──────────────────────────────────────────────
        self.sm.set_state(AppState.GOVERNANCE_CHECK)
        await self._loop_state_animation("governance check", 3)
        gov_result, gov_detail = self._simulate_governance(intent, session)
        phrase = self._phrase()
        acc += f"GOVERNANCE  [ {phrase} ]\n"
        acc += f"  status       →  {gov_result}\n"
        acc += f"  detail       →  {gov_detail}\n"

        if gov_result == "BLOCKED":
            acc += "\n  ⚠ Execution halted by governance policy.\n"
            self.output.update_active(acc)
            self.sm.set_state(AppState.ERROR)
            self.output.finalize_active()
            return

        if session.governance.require_plan_approval:
            acc += "  approval     →  auto-approved (require_plan_approval=true stub)\n"
        acc += "\n"
        self.output.update_active(acc)
        await asyncio.sleep(0.3)

        # ── EXECUTE ───────────────────────────────────────────────────────
        self.sm.set_state(AppState.EXECUTING)
        await self._loop_state_animation("executing", 5)
        acc += "EXECUTION\n"
        for i, task in enumerate(tasks, 1):
            acc = await self._phase_line(acc, f"task {i}/{len(tasks)}", task)

        # ── STUB: call backend here ────────────────────────────────────────
        backend_result = await self._call_backend_user_driven(intent, session)
        acc += "\n"

        # ── VERIFY ────────────────────────────────────────────────────────
        self.sm.set_state(AppState.VERIFYING)
        await self._loop_state_animation("verifying", 3)
        acc += f"VERIFICATION  →  {backend_result['verify_status']}\n"
        acc += f"  confidence   →  {backend_result['confidence']}\n\n"
        if backend_result["status"] == "error":
            acc += f"BACKEND ERROR →  {backend_result.get('error', 'backend_error')}\n\n"
        self.output.update_active(acc)
        await asyncio.sleep(0.3)

        # ── REFLECT ───────────────────────────────────────────────────────
        self.sm.set_state(AppState.REFLECTING)
        await self._loop_state_animation("reflecting", 3)
        if backend_result["status"] == "ok":
            acc += "REFLECT  →  memory updated\n\n"
        else:
            acc += "REFLECT  →  skipped due to backend error\n\n"
        self.output.update_active(acc)
        await asyncio.sleep(0.2)

        # ── FINAL OUTPUT ──────────────────────────────────────────────────
        self.sm.set_state(AppState.COMPLETE if backend_result["status"] == "ok" else AppState.ERROR)
        acc += "─" * 48 + "\n"
        acc = await self._stream_text(
            self._final_output_text(backend_result),
            prefix=acc,
            delay=0.03,
        )
        self.output.finalize_active()

    # ── ARCHITECTURAL pipeline ───────────────────────────────────────────────

    async def run_architectural_pipeline(self, prompt: str, session) -> None:
        self.output.mount_new_response()
        acc = ""

        # ── THINKING ─────────────────────────────────────────────────────
        self.sm.set_state(AppState.THINKING)
        await self._thinking_animation(10)

        acc += "ARCHITECTURAL MODE  —  Project Structure Received\n\n"
        self.output.update_active(acc)
        await asyncio.sleep(0.3)

        # ── PLAN ──────────────────────────────────────────────────────────
        self.sm.set_state(AppState.PLANNING)
        await self._loop_state_animation("planning", 6)
        acc += "AUTONOMOUS PLAN\n"
        acc = await self._phase_line(acc, "L11 Intent Runtime", "structure analysed")
        acc = await self._phase_line(acc, "L12 Strategy Planner", f"risk: {session.policy.risk_tolerance}")
        acc = await self._phase_line(acc, "L13 Micro Planner", "task DAG generated")
        acc = await self._phase_line(acc, "L18 Governance", "policies applied")
        acc += "\n"

        # ── GOVERNANCE ────────────────────────────────────────────────────
        self.sm.set_state(AppState.GOVERNANCE_CHECK)
        acc += f"GOVERNANCE POLICIES ACTIVE\n"
        if session.governance.protected_paths:
            acc += f"  protected    →  {', '.join(session.governance.protected_paths)}\n"
        if session.governance.forbidden_actions:
            acc += f"  forbidden    →  {', '.join(session.governance.forbidden_actions)}\n"
        acc += f"  max iters    →  {session.governance.max_iterations}\n\n"
        self.output.update_active(acc)
        await asyncio.sleep(0.4)

        # ── EXECUTE ───────────────────────────────────────────────────────
        self.sm.set_state(AppState.EXECUTING)
        await self._loop_state_animation("executing", 6)
        acc += "SCAFFOLD GENERATION\n"
        modules = self._extract_modules_from_prompt(prompt)
        for mod in modules:
            acc = await self._phase_line(acc, f"scaffold", mod)

        backend_result = await self._call_backend_architectural(prompt, session)
        acc += "\n"

        # ── VERIFY + REFLECT ──────────────────────────────────────────────
        self.sm.set_state(AppState.VERIFYING)
        acc += f"VERIFICATION  →  {backend_result['verify_status']}\n"
        if backend_result["status"] == "error":
            acc += f"BACKEND ERROR →  {backend_result.get('error', 'backend_error')}\n"
        self.sm.set_state(AppState.REFLECTING)
        if backend_result["status"] == "ok":
            acc += "REFLECT  →  architecture locked to memory\n\n"
        else:
            acc += "REFLECT  →  skipped due to backend error\n\n"
        self.output.update_active(acc)
        await asyncio.sleep(0.3)

        self.sm.set_state(AppState.COMPLETE if backend_result["status"] == "ok" else AppState.ERROR)
        acc += "─" * 48 + "\n"
        acc = await self._stream_text(
            self._final_output_text(backend_result),
            prefix=acc,
            delay=0.025,
        )
        self.output.finalize_active()

    # ── Backend bridge ────────────────────────────────────────────────────────

    def _error_backend_response(self, message: str, output: str = "") -> dict:
        return {
            "status": "error",
            "verify_status": "FAIL",
            "confidence": "low",
            "output": output,
            "error": message,
        }

    def _nested_string(self, data: dict, path: tuple[str, ...]) -> str:
        current = data
        for key in path:
            if not isinstance(current, dict):
                return ""
            current = current.get(key)
        return current if isinstance(current, str) else ""

    def _first_non_empty_string(self, data: dict, paths: tuple[tuple[str, ...], ...]) -> str:
        for path in paths:
            value = self._nested_string(data, path)
            if value and value.strip():
                return value
        return ""

    def _extract_output_text(self, data: dict) -> str:
        return self._first_non_empty_string(
            data,
            (
                ("output",),
                ("text_output",),
                ("textOutput",),
                ("response", "text_output"),
                ("response", "textOutput"),
                ("payload", "output"),
                ("payload", "text_output"),
                ("payload", "textOutput"),
                ("payload", "response", "text_output"),
                ("payload", "response", "textOutput"),
                ("result", "output"),
                ("result", "payload", "response", "text_output"),
                ("result", "payload", "response", "textOutput"),
            ),
        )

    def _extract_error_text(self, data: dict) -> str:
        return self._first_non_empty_string(
            data,
            (
                ("error",),
                ("error_message",),
                ("errorMessage",),
                ("payload", "error"),
                ("payload", "error_message"),
                ("payload", "errorMessage"),
                ("result", "error"),
                ("result", "payload", "error"),
            ),
        )

    def _final_output_text(self, backend_result: dict) -> str:
        output = str(backend_result.get("output", "") or "")
        if output.strip():
            return output
        error = str(backend_result.get("error", "") or "")
        if error.strip():
            return f"Backend error: {error}"
        return "(No output returned by cognitive_run.)"

    def _normalize_backend_result(self, result: dict) -> dict:
        normalized = dict(result) if isinstance(result, dict) else {}

        status = str(normalized.get("status", "")).lower()
        if not status and isinstance(normalized.get("ok"), bool):
            status = "ok" if normalized["ok"] else "error"

        verify_raw = normalized.get("verify_status", normalized.get("verifyStatus", ""))
        verify = str(verify_raw).upper()
        if not status:
            if verify == "PASS":
                status = "ok"
            elif verify == "FAIL":
                status = "error"
            else:
                status = "ok"

        output = self._extract_output_text(normalized)
        error = self._extract_error_text(normalized)
        if status != "error" and error and not output:
            status = "error"

        confidence_value = normalized.get(
            "confidence",
            normalized.get("confidence_level", normalized.get("confidenceLevel", "")),
        )
        confidence = str(confidence_value).strip().lower()
        if not confidence:
            confidence = "medium" if status == "ok" else "low"

        normalized["status"] = status
        normalized["verify_status"] = verify if verify else ("PASS" if status == "ok" else "FAIL")
        normalized["confidence"] = confidence
        normalized["output"] = output
        if error:
            normalized["error"] = error
        elif status == "error":
            normalized["error"] = "backend_error"
        return normalized

    async def _invoke_ultra_via_ipc(self, cmd: str, payload: dict, project_root: str) -> Optional[dict]:
        state_dirs = [
            os.path.join(project_root, ".ultra_daemon"),
            os.path.join(project_root, ".ultra"),
        ]

        selected_state_dir = None
        for state_dir in state_dirs:
            daemon_pid = os.path.join(state_dir, "daemon.pid")
            pipe_name = os.path.join(state_dir, "pipe.name")
            daemon_sock = os.path.join(state_dir, "daemon.sock")
            if os.path.isfile(daemon_pid) and (os.path.isfile(pipe_name) or os.path.exists(daemon_sock)):
                selected_state_dir = state_dir
                break
        if selected_state_dir is None:
            return None

        request_obj = {
            "type": cmd,
            "payload": payload,
        }
        request_line = json.dumps(request_obj, separators=(",", ":")) + "\n"

        async def _send_unix_socket(socket_path: str) -> Optional[dict]:
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_unix_connection(socket_path),
                    timeout=2.0,
                )
                writer.write(request_line.encode("utf-8"))
                await writer.drain()
                raw = await asyncio.wait_for(reader.readline(), timeout=3.0)
                writer.close()
                await writer.wait_closed()
                if not raw:
                    return None
                response = json.loads(raw.decode("utf-8", errors="replace"))
                return response if isinstance(response, dict) else None
            except Exception:
                return None

        async def _send_named_pipe(pipe_path: str) -> Optional[dict]:
            def _io() -> Optional[dict]:
                try:
                    with open(pipe_path, "r+b", buffering=0) as pipe_handle:
                        pipe_handle.write(request_line.encode("utf-8"))
                        buffer = bytearray()
                        while len(buffer) < 1024 * 1024:
                            chunk = pipe_handle.read(1)
                            if not chunk:
                                break
                            if chunk == b"\n":
                                break
                            buffer.extend(chunk)
                    if not buffer:
                        return None
                    decoded = buffer.decode("utf-8", errors="replace")
                    parsed = json.loads(decoded)
                    return parsed if isinstance(parsed, dict) else None
                except Exception:
                    return None

            try:
                return await asyncio.wait_for(asyncio.to_thread(_io), timeout=3.0)
            except Exception:
                return None

        ipc_response = None
        if os.name == "nt":
            pipe_name_path = os.path.join(selected_state_dir, "pipe.name")
            if os.path.isfile(pipe_name_path):
                try:
                    with open(pipe_name_path, "r", encoding="utf-8") as pipe_file:
                        pipe_name = pipe_file.readline().strip()
                    if pipe_name:
                        ipc_response = await _send_named_pipe(pipe_name)
                except Exception:
                    ipc_response = None
        else:
            socket_path = os.path.join(selected_state_dir, "daemon.sock")
            if os.path.exists(socket_path):
                ipc_response = await _send_unix_socket(socket_path)

        if not isinstance(ipc_response, dict):
            return None
        if isinstance(ipc_response.get("payload"), dict):
            payload_result = dict(ipc_response["payload"])
            if "status" not in payload_result and "status" in ipc_response:
                payload_result["status"] = ipc_response["status"]
            return self._normalize_backend_result(payload_result)
        if any(
            key in ipc_response
            for key in ("status", "verify_status", "verifyStatus", "output", "error")
        ):
            return self._normalize_backend_result(ipc_response)
        return None

    async def _invoke_ultra(self, cmd: str, payload: dict) -> dict:
        """
        Invoke the Ultra C++ backend.
        Primary fallback: subprocess call to `ultra <cmd> --file <payload_file>`.
        """
        session_payload = payload.get("session", {}) if isinstance(payload, dict) else {}
        project_root = str(
            session_payload.get(
                "project_root",
                getattr(self.session, "project_root", os.getcwd()),
            )
        )
        if not project_root:
            project_root = os.getcwd()

        # cognitive_run is a CLI command — never route through daemon IPC.
        # IPC is only for daemon-native commands (ai_status, ai_query, etc.)
        _IPC_ONLY_CMDS = {"ai_status", "ai_query", "ai_source", "ai_context", "ai_impact"}
        if cmd in _IPC_ONLY_CMDS:
            ipc_result = await self._invoke_ultra_via_ipc(cmd, payload, project_root)
            if ipc_result is not None:
                return ipc_result

        ultra_bin = shutil.which("ultra")
        if ultra_bin is None:
            candidates = [
                os.path.join(project_root, "build", "Release", "ultra.exe"),
                os.path.join(project_root, "build", "ultra.exe"),
                os.path.join(project_root, "build", "ultra"),
            ]
            for candidate in candidates:
                if os.path.isfile(candidate):
                    ultra_bin = candidate
                    break

        if ultra_bin is None:
            return self._error_backend_response(
                "ultra binary not found in PATH or build/. Run cmake --build build first."
            )

        json_payload = json.dumps(payload)
        payload_file: Optional[str] = None

        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                delete=False,
                suffix=".json",
            ) as temp_payload:
                temp_payload.write(json_payload)
                payload_file = temp_payload.name

            proc = await asyncio.create_subprocess_exec(
                ultra_bin,
                cmd,
                "--file",
                payload_file,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                cwd=project_root,
            )

            try:
                out, err = await asyncio.wait_for(proc.communicate(), timeout=300.0)
            except asyncio.TimeoutError:
                proc.kill()
                return self._error_backend_response(f"ultra {cmd} timed out after 300s")

            stdout_text = out.decode("utf-8", errors="replace").strip()
            stderr_text = err.decode("utf-8", errors="replace").strip()

            if proc.returncode != 0 and not stdout_text:
                return self._error_backend_response(
                    stderr_text or f"ultra {cmd} exited {proc.returncode}"
                )

            try:
                parsed = json.loads(stdout_text)
                if not isinstance(parsed, dict):
                    raise ValueError("Expected JSON object")
                return self._normalize_backend_result(parsed)
            except (json.JSONDecodeError, ValueError):
                object_start = stdout_text.find("{")
                object_end = stdout_text.rfind("}")
                if object_start != -1 and object_end > object_start:
                    try:
                        parsed = json.loads(stdout_text[object_start : object_end + 1])
                        if isinstance(parsed, dict):
                            return self._normalize_backend_result(parsed)
                    except json.JSONDecodeError:
                        pass
                lines = stdout_text.splitlines()
                for line in reversed(lines):
                    line = line.strip()
                    if not line.startswith("{"):
                        continue
                    try:
                        parsed = json.loads(line)
                        if isinstance(parsed, dict):
                            return self._normalize_backend_result(parsed)
                    except json.JSONDecodeError:
                        continue
                return self._error_backend_response(
                    "ultra output was not valid JSON",
                    output=stdout_text,
                )
        except FileNotFoundError:
            return self._error_backend_response(
                f"ultra binary not executable: {ultra_bin}"
            )
        except Exception as exc:  # noqa: BLE001
            return self._error_backend_response(str(exc))
        finally:
            if payload_file:
                try:
                    os.remove(payload_file)
                except OSError:
                    pass

    async def _call_backend_user_driven(self, intent: IntentPayload, session) -> dict:
        session_dict = session.to_dict()
        # CLIEngine cognitive_run only accepts project_root, governance, policy
        # Strip 'mode' and any other TUI-only fields
        session_dict.pop("mode", None)
        intent_dict = intent.to_dict()
        # Strip context_hint — not part of CLIEngine's intent schema
        intent_dict.pop("context_hint", None)
        payload = {
            "intent": intent_dict,
            "session": session_dict,
        }
        return await self._invoke_ultra("cognitive_run", payload)

    async def _call_backend_architectural(self, prompt: str, session) -> dict:
        payload = {
            "intent": {
                "raw_prompt": prompt,
                "action": "add",
                "targets": [],
                "goal_summary": prompt[:80],
                "constraints": [],
                "risk_level": session.policy.risk_tolerance,
                "requires_planning": True,
            },
            "session": session.to_dict(),
        }
        return await self._invoke_ultra("cognitive_run", payload)

    # ── Helpers ──────────────────────────────────────────────────────────────

    def _generate_micro_tasks(self, intent: IntentPayload) -> list:
        base = intent.goal_summary
        action = intent.action
        tasks = {
            "add":      [f"Create {base}", "Compile check", f"Integrate {base}", "Run tests"],
            "fix":      [f"Locate defect in {base}", "Analyse root cause", "Apply patch", "Verify fix"],
            "refactor": [f"Analyse {base}", "Extract components", "Rewrite structure", "Validate API"],
            "remove":   [f"Check usages of {base}", "Remove symbol", "Update references", "Compile"],
            "test":     [f"Analyse {base} interface", "Write unit tests", "Write edge cases", "Run suite"],
            "analyse":  [f"Query graph for {base}", "Compress context", "Generate report"],
            "optimise": [f"Profile {base}", "Identify bottleneck", "Apply optimisation", "Benchmark"],
        }
        return tasks.get(action, [f"Process {base}", "Verify", "Complete"])

    def _simulate_governance(self, intent: IntentPayload, session) -> tuple:
        if session.governance.forbidden_actions:
            if intent.action in session.governance.forbidden_actions:
                return "BLOCKED", f"action '{intent.action}' is forbidden by policy"
        if session.governance.protected_paths and intent.targets:
            for target in intent.targets:
                for path in session.governance.protected_paths:
                    if path.lower() in target.lower():
                        return "BLOCKED", f"target '{target}' is in protected path '{path}'"
        if intent.risk_level == "high" and session.policy.risk_tolerance == "low":
            return "FLAGGED", "high-risk action flagged — proceeding with caution"
        return "APPROVED", f"risk: {intent.risk_level} | tolerance: {session.policy.risk_tolerance}"

    def _extract_modules_from_prompt(self, prompt: str) -> list:
        import re
        words = re.findall(r"\b[A-Z][a-zA-Z0-9]+\b", prompt)
        return words[:6] if words else ["Core", "API", "Storage", "UI"]#MultiModelOrchestrator.cppMultiModelOrchestrator.cpp