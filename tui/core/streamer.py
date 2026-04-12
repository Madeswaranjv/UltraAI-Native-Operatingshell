"""
EngineStreamer — drives live TUI output from the real Ultra cognitive loop.

The C++ backend keeps stdout reserved for the final JSON payload and emits
structured loop diagnostics on stderr. This module preserves that contract
while surfacing both channels in real time inside the TUI.
"""
from __future__ import annotations

import asyncio
import contextlib
import json
import os
import queue
import random
import re
import shutil
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

from utils.anime_words import anime_words_for_phase
from utils.thinking_states import LOOP_PHASE_LABELS, LOOP_STATE_FRAMES, THINKING_FRAMES
from core.state_manager import AppState
from core.intent_parser import IntentPayload


TRANSITION_RE = re.compile(
    r"^\[UltraLoop\]\[Transition\] from=(?P<from>[A-Z_]+) "
    r"to=(?P<to>[A-Z_]+) reason=(?P<reason>.*)$"
)
ARBITRATION_RE = re.compile(
    r"^\[UltraLoop\]\[Arbitration\] iteration=(?P<iteration>\d+) "
    r"candidates=(?P<candidates>\d+) conflicts=(?P<conflicts>\d+) "
    r"selected=(?P<selected>\d+) tasks=(?P<tasks>.*)$"
)
REPAIR_RE = re.compile(
    r"^\[UltraLoop\]\[Repair\] iteration=(?P<iteration>\d+) "
    r"attempt=(?P<attempt>\d+) site=(?P<site>\S+) "
    r"decision=(?P<decision>\S+) tasks=(?P<tasks>\S*) "
    r"reason=(?P<reason>.*)$"
)
TASK_OUTPUT_RE = re.compile(
    r"^\[UltraLoop\] Task (?P<task>\S+) produced text_output bytes="
    r"(?P<output_len>\d+)$"
)
PROVIDER_RE = re.compile(
    r"^\[ULTRA-RUNTIME\] provider_used=(?P<provider>\S+)"
    r"(?: endpoint=(?P<endpoint>.*))?$"
)
MODEL_ENTER_RE = re.compile(
    r"^\[ULTRA-DEBUG\] ModelGenerate entered\..*provider=(?P<provider>\S+)"
)
MODEL_RESPONSE_RE = re.compile(
    r"^\[ULTRA-DEBUG\] ModelGenerate response\. ok=(?P<ok>true|false) "
    r"error=(?P<error>.*?) output_len=(?P<output_len>\d+)$"
)
TOOL_CALL_DETECTED_RE = re.compile(
    r"^\[TOOL_CALL_DETECTED\] count=(?P<count>\d+) "
    r"source=(?P<source>\S+) tools=(?P<tools>.*)$"
)
TOOL_ROUTER_EXECUTED_RE = re.compile(
    r"^\[TOOL_ROUTER_EXECUTED\] tool=(?P<tool>\S+) "
    r"transport=(?P<transport>\S+) ok=(?P<ok>true|false)$"
)
EXECUTION_KERNEL_APPLIED_RE = re.compile(
    r"^\[EXECUTION_KERNEL_APPLIED\] tool=(?P<tool>\S+) "
    r"ok=(?P<ok>true|false) applied=(?P<applied>true|false)"
    r"(?: file_verified=(?P<file_verified>true|false))?$"
)
FAILURE_TRACE_RE = re.compile(
    r"^\[FAILURE TRACE\] phase=(?P<phase>\S+) "
    r"module=(?P<module>\S+) error_type=(?P<error_type>\S+) "
    r"root_cause=(?P<root_cause>.*)$"
)
LOG_PREFIX_RE = re.compile(r"^\[(?:INFO|WARNING|ERROR)\](?: \[[A-Z]+\])? (?P<message>.*)$")
GOVERNANCE_RE = re.compile(
    r"^Governance decision tool='(?P<tool>[^']+)' risk=(?P<risk>\S+) "
    r"confidence=(?P<confidence>[0-9.]+) allowed=(?P<allowed>true|false) "
    r"reason='(?P<reason>.*)'\.?$"
)
EXECUTING_RE = re.compile(r"^Executing: (?P<command>.*)$")
TOOL_ROUTER_COMMAND_RE = re.compile(
    r"^ToolRouter executing Ultra command: (?P<command>.*)$"
)
TOOL_ROUTER_RETRY_RE = re.compile(
    r"^ToolRouter retrying Ultra command after failure for tool '(?P<tool>[^']+)'\.$"
)
MICROPLANNER_FALLBACK_RE = re.compile(
    r"^\[MicroPlanner\] Invalid payload -> fallback to ContextExtraction\."
    r" task=(?P<task>\S+) reason=(?P<reason>.*)$"
)
FAILURE_RECOVERY_RE = re.compile(
    r"^\[FailureRecovery\] task=(?P<task>\S+) "
    r"(?:class=(?P<failure_class>\S+) )?"
    r"(?:memory_action=(?P<memory_action>\S+) )?"
    r"retry=(?P<retry>\d+)/(?P<limit>\d+) "
    r"(?:action=(?P<action>\S+) )?"
    r"(?:(?:message|pattern)=(?P<message>.*))?$"
)
LOOP_PHASE_TO_APP_STATE = {
    "INIT": AppState.INITIALIZING,
    "PLAN": AppState.PLANNING,
    "ARBITRATION": AppState.ARBITRATING,
    "MICRO_PLAN": AppState.MICRO_PLANNING,
    "EXECUTE": AppState.EXECUTING,
    "PARTIAL_REPAIR": AppState.REPAIRING,
    "VERIFY": AppState.VERIFYING,
    "REFLECT": AppState.REFLECTING,
    "RE_ANCHOR": AppState.REANCHORING,
    "REPLAN": AppState.REPLANNING,
    "TERMINATE": AppState.COMPLETE,
}


class StreamEventType(str, Enum):
    PLAN_START = "PLAN_START"
    PLAN_STEP = "PLAN_STEP"
    EXECUTION_START = "EXECUTION_START"
    EXECUTION_STEP = "EXECUTION_STEP"
    TOOL_CALL = "TOOL_CALL"
    TOOL_RESULT = "TOOL_RESULT"
    GOVERNANCE_BLOCK = "GOVERNANCE_BLOCK"
    RETRY_EVENT = "RETRY_EVENT"
    SUCCESS = "SUCCESS"
    FAILURE = "FAILURE"


@dataclass
class StreamEvent:
    event_type: StreamEventType
    title: str
    detail: str = ""
    phase: str = ""
    raw_line: str = ""
    indent: int = 1
    boundary: bool = False
    metadata: dict[str, object] = field(default_factory=dict)


@dataclass
class LiveRenderState:
    phase: str = "INIT"
    status_reason: str = "Launching cognitive loop..."
    event_detail: str = ""
    anime_word: str = "Analyzing..."
    frame_index: int = 0
    anime_index: int = 0
    provider_used: str = ""
    provider_endpoint: str = ""
    final_output_ready: bool = False
    output_revealed: bool = False
    stdout_lines: list[str] = field(default_factory=list)
    stderr_lines: list[str] = field(default_factory=list)
    events: list[StreamEvent] = field(default_factory=list)
    transcript_lines: list[str] = field(default_factory=list)
    raw_log_lines: list[str] = field(default_factory=list)
    current_step: str = ""
    current_tool: str = ""
    retry_count: int = 0
    last_event_key: str = ""


class EngineStreamer:
    PROCESS_TIMEOUT_SECONDS = 300.0

    def __init__(self, output_component, state_manager):
        self.output = output_component
        self.sm = state_manager
        self.session = None

    def set_session(self, session) -> None:
        self.session = session

    def _runtime_intent_payload(self, intent_payload: dict) -> dict:
        runtime_payload = dict(intent_payload)
        requested_role = runtime_payload.get("requested_role")
        if not isinstance(requested_role, str) or not requested_role:
            model_role = runtime_payload.get("model_role")
            if isinstance(model_role, str) and model_role:
                requested_role = model_role
            else:
                requested_role = "auto"
        runtime_payload["model_role"] = "auto"
        runtime_payload["requested_role"] = requested_role
        return runtime_payload

    async def run_user_driven_pipeline(
        self, prompt: str, intent: IntentPayload, session
    ) -> None:
        session_dict = self._session_payload(session)
        intent_dict = self._runtime_intent_payload(intent.to_dict())
        intent_dict.pop("context_hint", None)

        payload = {
            "intent": intent_dict,
            "session": session_dict,
        }
        launch_detail = f"{intent.action}: {intent.goal_summary}"
        await self._run_live_cognitive_pipeline(payload, launch_detail)

    async def run_architectural_pipeline(self, prompt: str, session) -> None:
        payload = {
            "intent": self._runtime_intent_payload({
                "raw_prompt": prompt,
                "action": "add",
                "targets": [],
                "goal_summary": prompt[:80],
                "constraints": [],
                "risk_level": session.policy.risk_tolerance,
                "requires_planning": True,
                "model_role": "planner",
            }),
            "session": self._session_payload(session),
        }
        await self._run_live_cognitive_pipeline(
            payload,
            "architectural pass: launching cognitive_run",
        )

    async def _run_live_cognitive_pipeline(
        self,
        payload: dict,
        launch_detail: str,
    ) -> None:
        self.output.mount_new_response(verbose=getattr(self.sm, "verbose_stream", False))
        live = LiveRenderState(
            status_reason=launch_detail,
            anime_word=anime_words_for_phase("INIT")[0],
        )

        self.sm.set_state(AppState.INITIALIZING)
        self.output.update_active_stream("")
        self.output.update_active("")
        self._render_live_status(live)
        self._emit_event(
            live,
            StreamEvent(
                StreamEventType.PLAN_STEP,
                "Request accepted",
                launch_detail,
                phase="INIT",
                indent=0,
            ),
        )

        anime_task = asyncio.create_task(self._anime_loop(live))
        try:
            backend_result = await self._invoke_ultra("cognitive_run", payload, live)
            await self._finalize_live_response(live, backend_result)
        finally:
            await self._stop_task(anime_task)

    async def _anime_loop(self, live: LiveRenderState) -> None:
        while not live.final_output_ready:
            words = anime_words_for_phase(live.phase)
            if words:
                live.anime_word = words[live.anime_index % len(words)]
                live.anime_index += 1
            live.frame_index += 1
            self._render_live_status(live)
            await asyncio.sleep(random.uniform(3, 5))

    async def _stop_task(self, task: Optional[asyncio.Task]) -> None:
        if task is None:
            return
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    def _render_live_status(self, live: LiveRenderState) -> None:
        frame_source = THINKING_FRAMES if live.phase == "INIT" else LOOP_STATE_FRAMES
        frame = frame_source[live.frame_index % len(frame_source)]
        phase_label = LOOP_PHASE_LABELS.get(
            live.phase,
            live.phase.lower().replace("_", " "),
        )
        banner = f"{frame}  {phase_label.upper()}  {live.anime_word}"

        detail_lines: list[str] = []
        if live.status_reason:
            detail_lines.append(live.status_reason)
        if live.event_detail and live.event_detail not in detail_lines:
            detail_lines.append(live.event_detail)
        if live.current_tool:
            tool_line = f"tool: {live.current_tool}"
            if tool_line not in detail_lines:
                detail_lines.append(tool_line)
        if live.retry_count > 0:
            retry_line = f"retry count: {live.retry_count}"
            if retry_line not in detail_lines:
                detail_lines.append(retry_line)
        if live.provider_used:
            provider_line = f"provider: {live.provider_used}"
            if live.provider_endpoint:
                provider_line += f" @ {live.provider_endpoint}"
            if provider_line not in detail_lines:
                detail_lines.append(provider_line)

        self.output.update_active_status(banner, "\n".join(detail_lines[:3]))

    def _set_loop_phase(
        self,
        live: LiveRenderState,
        phase: str,
        reason: str = "",
    ) -> None:
        live.phase = phase
        if reason:
            live.status_reason = reason
        live.event_detail = ""

        app_state = LOOP_PHASE_TO_APP_STATE.get(phase)
        if app_state is not None:
            self.sm.set_state(app_state)
        self._render_live_status(live)

    def _strip_log_prefix(self, line: str) -> str:
        log_prefix_match = LOG_PREFIX_RE.match(line)
        if log_prefix_match:
            return log_prefix_match.group("message").strip()
        return line.strip()

    def _append_raw_line(self, live: LiveRenderState, channel: str, line: str) -> None:
        if not line:
            return
        raw_line = f"{channel:>6} | {line}"
        live.raw_log_lines.append(raw_line)
        self.output.append_active_raw(raw_line + "\n")

    def _is_streamable_log_line(self, channel: str, line: str) -> bool:
        stripped = line.strip()
        if not stripped:
            return False
        if channel == "stdout" and stripped.startswith("{"):
            return False
        return True

    def _phase_heading_for(self, phase: str) -> str:
        label = LOOP_PHASE_LABELS.get(phase, phase.lower().replace("_", " "))
        return label.replace("_", " ").title()

    def _event_key(self, event: StreamEvent) -> str:
        return "|".join(
            [
                event.event_type.value,
                event.title,
                event.detail,
                event.phase,
                str(event.metadata.get("tool", "")),
            ]
        )

    def _format_event(self, live: LiveRenderState, event: StreamEvent) -> str:
        group_icons = {
            StreamEventType.EXECUTION_START: "⚙️",
            StreamEventType.SUCCESS: "✓",
            StreamEventType.FAILURE: "✗",
            StreamEventType.GOVERNANCE_BLOCK: "✗",
        }
        line_icons = {
            StreamEventType.PLAN_STEP: "→",
            StreamEventType.EXECUTION_STEP: "⚙️",
            StreamEventType.TOOL_CALL: "→",
            StreamEventType.TOOL_RESULT: "✓",
            StreamEventType.RETRY_EVENT: "↻",
        }
        prefix = "  " * max(0, event.indent)
        icon = group_icons.get(event.event_type, line_icons.get(event.event_type, "•"))

        if event.event_type == StreamEventType.PLAN_START:
            lines = [event.title]
        else:
            lines = [f"{prefix}{icon} {event.title}".rstrip()]
        if event.detail:
            detail_prefix = "  " * max(1, event.indent + 1)
            lines.append(f"{detail_prefix}{event.detail}")

        rendered = "\n".join(lines)
        if event.boundary and live.transcript_lines:
            rendered = "\n" + rendered
        return rendered + "\n"

    def _emit_event(self, live: LiveRenderState, event: StreamEvent) -> None:
        event.phase = event.phase or live.phase
        key = self._event_key(event)
        if key == live.last_event_key:
            return

        live.last_event_key = key
        live.events.append(event)
        live.current_step = event.title
        live.event_detail = event.detail or event.title

        tool_name = str(event.metadata.get("tool", "") or "")
        if tool_name:
            live.current_tool = tool_name

        retry_raw = event.metadata.get("retry_count")
        if isinstance(retry_raw, int):
            live.retry_count = max(live.retry_count, retry_raw)

        rendered = self._format_event(live, event)
        live.transcript_lines.append(rendered)
        self.output.append_active_stream(rendered)
        self._render_live_status(live)

    def _make_transition_event(
        self,
        phase: str,
        reason: str,
    ) -> Optional[StreamEvent]:
        normalized_reason = reason.strip()
        if phase == "PLAN":
            return StreamEvent(
                StreamEventType.PLAN_START,
                "Planning",
                normalized_reason or "Building strategy from the resolved intent.",
                phase=phase,
                indent=0,
                boundary=True,
            )
        if phase == "ARBITRATION":
            return StreamEvent(
                StreamEventType.PLAN_STEP,
                "Arbitrating candidate actions",
                normalized_reason or "Selecting a deterministic path through the plan.",
                phase=phase,
            )
        if phase == "MICRO_PLAN":
            return StreamEvent(
                StreamEventType.PLAN_STEP,
                "Building micro-plan",
                normalized_reason or "Expanding the selected strategy into task graph steps.",
                phase=phase,
            )
        if phase == "EXECUTE":
            return StreamEvent(
                StreamEventType.EXECUTION_START,
                "Executing",
                normalized_reason or "Running the task graph.",
                phase=phase,
                indent=0,
                boundary=True,
            )
        if phase == "PARTIAL_REPAIR":
            return StreamEvent(
                StreamEventType.RETRY_EVENT,
                "Repairing execution path",
                normalized_reason or "Recovery requested a partial repair.",
                phase=phase,
                metadata={"retry_count": 1},
            )
        if phase in {"VERIFY", "REFLECT", "RE_ANCHOR", "REPLAN"}:
            return StreamEvent(
                StreamEventType.EXECUTION_STEP,
                self._phase_heading_for(phase),
                normalized_reason,
                phase=phase,
            )
        return None

    def _tool_result_detail(
        self,
        tool: str,
        ok_raw: str,
        applied_raw: str,
        verified_raw: Optional[str],
    ) -> tuple[str, str]:
        ok = ok_raw == "true"
        applied = applied_raw == "true"
        verified = verified_raw == "true" if verified_raw is not None else None

        if ok and applied and verified is True:
            return f"{tool} completed", "Patch applied and file verification passed."
        if ok and applied:
            return f"{tool} completed", "Patch applied successfully."
        if ok:
            return f"{tool} completed", "Tool returned success."
        if verified is False:
            return f"{tool} failed", "Patch output did not verify against the file state."
        return f"{tool} failed", "Tool execution returned an error state."

    def _parse_runtime_line(
        self,
        raw_line: str,
        live: LiveRenderState,
    ) -> Optional[StreamEvent]:
        normalized = raw_line.strip()
        if not normalized:
            return None

        transition_match = TRANSITION_RE.match(normalized)
        if transition_match:
            next_phase = transition_match.group("to")
            reason = transition_match.group("reason").strip()
            self._set_loop_phase(live, next_phase, reason)
            return self._make_transition_event(next_phase, reason)

        arbitration_match = ARBITRATION_RE.match(normalized)
        if arbitration_match:
            return StreamEvent(
                StreamEventType.PLAN_STEP,
                "Micro-plan candidates ranked",
                (
                    f"Iteration {arbitration_match.group('iteration')} selected "
                    f"{arbitration_match.group('selected')} of "
                    f"{arbitration_match.group('candidates')} candidates with "
                    f"{arbitration_match.group('conflicts')} conflicts."
                ),
                phase=live.phase,
            )

        repair_match = REPAIR_RE.match(normalized)
        if repair_match:
            attempt = int(repair_match.group("attempt"))
            return StreamEvent(
                StreamEventType.RETRY_EVENT,
                "Partial repair requested",
                (
                    f"{repair_match.group('decision')} at {repair_match.group('site')} "
                    f"(attempt {attempt}). {repair_match.group('reason').strip()}"
                ),
                phase=live.phase,
                metadata={"retry_count": attempt},
            )

        provider_match = PROVIDER_RE.match(normalized)
        if provider_match:
            live.provider_used = provider_match.group("provider")
            live.provider_endpoint = (provider_match.group("endpoint") or "").strip()
            detail = live.provider_used
            if live.provider_endpoint:
                detail += f" @ {live.provider_endpoint}"
            return StreamEvent(
                StreamEventType.EXECUTION_STEP,
                "Model provider selected",
                detail,
                phase=live.phase,
            )

        model_enter_match = MODEL_ENTER_RE.match(normalized)
        if model_enter_match:
            provider = model_enter_match.group("provider")
            detail = (
                f"Provider hint: {provider}" if provider and provider != "auto" else
                "Generating the next executable action."
            )
            return StreamEvent(
                StreamEventType.EXECUTION_STEP,
                "Generating tool-ready action",
                detail,
                phase=live.phase,
            )

        model_response_match = MODEL_RESPONSE_RE.match(normalized)
        if model_response_match:
            ok = model_response_match.group("ok") == "true"
            detail = f"{model_response_match.group('output_len')} bytes returned."
            error = model_response_match.group("error").strip()
            if error and error != "null":
                detail = f"{detail} Error: {error}"
            return StreamEvent(
                StreamEventType.EXECUTION_STEP if ok else StreamEventType.FAILURE,
                "Model response ready" if ok else "Model response failed",
                detail,
                phase=live.phase,
                boundary=not ok,
            )

        tool_call_match = TOOL_CALL_DETECTED_RE.match(normalized)
        if tool_call_match:
            tools = tool_call_match.group("tools") or tool_call_match.group("count")
            tool_list = [token.strip() for token in tools.split(",") if token.strip()]
            tool_name = tool_list[0] if len(tool_list) == 1 else ""
            return StreamEvent(
                StreamEventType.TOOL_CALL,
                f"Tool call detected: {tools}",
                f"Source: {tool_call_match.group('source')}",
                phase=live.phase,
                metadata={"tool": tool_name},
            )

        tool_router_match = TOOL_ROUTER_EXECUTED_RE.match(normalized)
        if tool_router_match:
            tool_name = tool_router_match.group("tool")
            return StreamEvent(
                StreamEventType.TOOL_CALL,
                f"Running tool: {tool_name}",
                f"Transport: {tool_router_match.group('transport')}",
                phase=live.phase,
                metadata={"tool": tool_name},
            )

        execution_applied_match = EXECUTION_KERNEL_APPLIED_RE.match(normalized)
        if execution_applied_match:
            tool_name = execution_applied_match.group("tool")
            title, detail = self._tool_result_detail(
                tool_name,
                execution_applied_match.group("ok"),
                execution_applied_match.group("applied"),
                execution_applied_match.group("file_verified"),
            )
            event_type = (
                StreamEventType.TOOL_RESULT
                if execution_applied_match.group("ok") == "true"
                else StreamEventType.FAILURE
            )
            return StreamEvent(
                event_type,
                title,
                detail,
                phase=live.phase,
                metadata={"tool": tool_name},
            )

        failure_trace_match = FAILURE_TRACE_RE.match(normalized)
        if failure_trace_match:
            failure_phase = failure_trace_match.group("phase")
            if failure_phase in LOOP_PHASE_TO_APP_STATE:
                live.phase = failure_phase
                self.sm.set_state(LOOP_PHASE_TO_APP_STATE[failure_phase])
            return StreamEvent(
                StreamEventType.FAILURE,
                f"{failure_trace_match.group('module')} failed",
                failure_trace_match.group("root_cause"),
                phase=failure_phase,
                boundary=True,
            )

        task_output_match = TASK_OUTPUT_RE.match(normalized)
        if task_output_match:
            return StreamEvent(
                StreamEventType.EXECUTION_STEP,
                f"Task output captured: {task_output_match.group('task')}",
                f"{task_output_match.group('output_len')} bytes of text output.",
                phase=live.phase,
            )

        microplanner_fallback = MICROPLANNER_FALLBACK_RE.match(normalized)
        if microplanner_fallback:
            return StreamEvent(
                StreamEventType.PLAN_STEP,
                "Micro-plan normalized invalid payload",
                (
                    f"Task {microplanner_fallback.group('task')} fell back to "
                    f"ContextExtraction because {microplanner_fallback.group('reason')}."
                ),
                phase=live.phase,
            )

        governance_match = GOVERNANCE_RE.match(normalized)
        if governance_match:
            tool_name = governance_match.group("tool")
            allowed = governance_match.group("allowed") == "true"
            detail = (
                f"Reason: {governance_match.group('reason')} "
                f"(risk={governance_match.group('risk')}, "
                f"confidence={governance_match.group('confidence')})"
            )
            return StreamEvent(
                StreamEventType.EXECUTION_STEP if allowed else StreamEventType.GOVERNANCE_BLOCK,
                f"Governance cleared {tool_name}" if allowed else "Governance blocked",
                detail,
                phase=live.phase,
                boundary=not allowed,
                metadata={"tool": tool_name},
            )

        executing_match = EXECUTING_RE.match(normalized)
        if executing_match:
            return StreamEvent(
                StreamEventType.EXECUTION_STEP,
                "Executing command",
                executing_match.group("command"),
                phase=live.phase,
            )

        tool_router_command_match = TOOL_ROUTER_COMMAND_RE.match(normalized)
        if tool_router_command_match:
            return StreamEvent(
                StreamEventType.TOOL_CALL,
                "Dispatching tool command",
                tool_router_command_match.group("command"),
                phase=live.phase,
            )

        tool_router_retry_match = TOOL_ROUTER_RETRY_RE.match(normalized)
        if tool_router_retry_match:
            return StreamEvent(
                StreamEventType.RETRY_EVENT,
                "Retrying tool dispatch",
                f"Previous {tool_router_retry_match.group('tool')} command failed.",
                phase=live.phase,
                metadata={"tool": tool_router_retry_match.group("tool")},
            )

        failure_recovery_match = FAILURE_RECOVERY_RE.match(normalized)
        if failure_recovery_match:
            retry_count = int(failure_recovery_match.group("retry"))
            action = (
                failure_recovery_match.group("action")
                or failure_recovery_match.group("memory_action")
                or "REPLAN_REQUIRED"
            )
            message = failure_recovery_match.group("message") or "Recovery path updated."
            return StreamEvent(
                StreamEventType.RETRY_EVENT,
                f"Recovery decision: {action}",
                f"Task {failure_recovery_match.group('task')} - {message}",
                phase=live.phase,
                metadata={"retry_count": retry_count},
            )

        if "Repeated failures detected" in normalized:
            return StreamEvent(
                StreamEventType.RETRY_EVENT,
                "Retrying action",
                "Previous attempt failed repeatedly; the runtime is escalating recovery.",
                phase=live.phase,
                metadata={"retry_count": live.retry_count + 1},
            )

        if normalized.startswith("[UltraLoop] "):
            event_type = (
                StreamEventType.PLAN_STEP
                if live.phase in {"PLAN", "ARBITRATION", "MICRO_PLAN", "REPLAN"}
                else StreamEventType.EXECUTION_STEP
            )
            return StreamEvent(
                event_type,
                self._phase_heading_for(live.phase),
                normalized.replace("[UltraLoop] ", "", 1),
                phase=live.phase,
            )

        if normalized.startswith("[FailureRecovery] "):
            return StreamEvent(
                StreamEventType.RETRY_EVENT,
                "Recovery update",
                normalized.replace("[FailureRecovery] ", "", 1),
                phase=live.phase,
                metadata={"retry_count": live.retry_count + 1},
            )

        if "apply_patch" in normalized:
            return StreamEvent(
                StreamEventType.TOOL_CALL,
                "apply_patch activity",
                normalized,
                phase=live.phase,
                metadata={"tool": "apply_patch"},
            )

        return None

    def _handle_runtime_line(
        self,
        line: str,
        live: LiveRenderState,
        *,
        channel: str,
    ) -> None:
        stripped = line.strip()
        if not stripped:
            return

        if self._is_streamable_log_line(channel, stripped):
            self._append_raw_line(live, channel, stripped)

        event = self._parse_runtime_line(self._strip_log_prefix(stripped), live)
        if event is not None:
            self._emit_event(live, event)

    async def _progressively_reveal_output(
        self,
        live: LiveRenderState,
        text: str,
    ) -> None:
        if not text:
            live.output_revealed = True
            return

        self.output.update_active("")
        for chunk in self._logical_output_chunks(text):
            self.output.append_active(chunk)
            await asyncio.sleep(0.025)

        self.output.update_active(text)
        live.output_revealed = True

    def _logical_output_chunks(self, text: str) -> list[str]:
        chunks: list[str] = []
        paragraphs = re.split(r"(\n\s*\n)", text)
        for token in paragraphs:
            if not token:
                continue
            if re.fullmatch(r"\n\s*\n", token):
                if chunks:
                    chunks[-1] += token
                continue

            lines = token.splitlines(keepends=True)
            if len(lines) <= 2:
                chunks.append(token)
                continue

            current = ""
            for line in lines:
                current += line
                if current.count("\n") >= 2 or len(current) >= 240:
                    chunks.append(current)
                    current = ""
            if current:
                chunks.append(current)
        return chunks or [text]

    def _emit_backend_summary_events(
        self,
        live: LiveRenderState,
        backend_result: dict,
    ) -> None:
        retry_count = backend_result.get("apply_patch_retry_count")
        if isinstance(retry_count, int) and retry_count > live.retry_count:
            self._emit_event(
                live,
                StreamEvent(
                    StreamEventType.RETRY_EVENT,
                    "Retry summary",
                    f"apply_patch required {retry_count} retry attempt(s).",
                    phase=live.phase,
                    metadata={"retry_count": retry_count, "tool": "apply_patch"},
                ),
            )

        timer_text = self._execution_timer_text(backend_result)
        tool_summary = str(backend_result.get("tool_execution_summary", "") or "").strip()
        success_detail_parts = [part for part in [tool_summary, timer_text] if part]

        if backend_result.get("status") == "ok":
            self._emit_event(
                live,
                StreamEvent(
                    StreamEventType.SUCCESS,
                    "Success",
                    " | ".join(success_detail_parts),
                    phase=live.phase,
                    indent=0,
                    boundary=True,
                ),
            )
            return

        failure_detail = self._extract_error_text(backend_result).strip()
        if not failure_detail:
            failure_lines = self._failure_trace_lines(backend_result)
            failure_detail = failure_lines[0] if failure_lines else "Execution ended with an error."
        self._emit_event(
            live,
            StreamEvent(
                StreamEventType.FAILURE,
                "Failure",
                failure_detail,
                phase=live.phase,
                indent=0,
                boundary=True,
            ),
        )

    async def _finalize_live_response(
        self,
        live: LiveRenderState,
        backend_result: dict,
    ) -> None:
        self._emit_backend_summary_events(live, backend_result)
        final_text = self._final_output_text(backend_result)
        if final_text and not live.output_revealed:
            live.final_output_ready = True
            await self._progressively_reveal_output(live, final_text)

        self.output.update_active(final_text)
        self.sm.set_state(
            AppState.COMPLETE
            if backend_result.get("status") == "ok"
            else AppState.ERROR
        )
        self.output.finalize_active(clear_status=True)

    def _error_backend_response(self, message: str, output: str = "") -> dict:
        return {
            "status": "error",
            "verify_status": "FAIL",
            "confidence": "low",
            "llm_output": "",
            "output": output,
            "error": message,
        }

    def _combine_process_output(self, stdout_text: str, stderr_text: str) -> str:
        stdout_text = stdout_text.strip()
        stderr_text = stderr_text.strip()
        if stdout_text and stderr_text:
            return f"{stdout_text}\n\n[stderr]\n{stderr_text}"
        return stdout_text or stderr_text

    def _nested_string(self, data: dict, path: tuple[str, ...]) -> str:
        current = data
        for key in path:
            if not isinstance(current, dict):
                return ""
            current = current.get(key)
        return current if isinstance(current, str) else ""

    def _first_non_empty_string(
        self,
        data: dict,
        paths: tuple[tuple[str, ...], ...],
    ) -> str:
        for path in paths:
            value = self._nested_string(data, path)
            if value and value.strip():
                return value
        return ""

    def _extract_output_text(self, data: dict) -> str:
        return self._first_non_empty_string(
            data,
            (
                ("llm_output",),
                ("llmOutput",),
                ("text_output",),
                ("textOutput",),
                ("output",),
                ("response", "text_output"),
                ("response", "textOutput"),
                ("payload", "text_output"),
                ("payload", "textOutput"),
                ("payload", "output"),
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

    def _execution_timer_text(self, backend_result: dict) -> str:
        timer = backend_result.get("execution_timer")
        if isinstance(timer, dict):
            duration = timer.get("duration_seconds")
            try:
                return f"Total execution time: {float(duration):.2f} seconds"
            except (TypeError, ValueError):
                return ""
        return ""

    def _tool_execution_lines(self, backend_result: dict) -> list[str]:
        tool_execution = backend_result.get("tool_execution")
        if not isinstance(tool_execution, dict):
            return []

        tool_name = str(tool_execution.get("tool", "") or "tool")
        ok = str(bool(tool_execution.get("ok", False))).lower()
        lines = [f"[tool] {tool_name} ok={ok}"]

        if "applied" in tool_execution:
            lines.append(f"applied={str(bool(tool_execution.get('applied'))).lower()}")
        if "file_verified" in tool_execution:
            lines.append(
                f"file_verified={str(bool(tool_execution.get('file_verified'))).lower()}"
            )
        error_text = str(tool_execution.get("error", "") or "")
        if error_text.strip():
            lines.append(f"error={error_text}")
        stderr_text = str(backend_result.get("_stderr", "") or "").strip()
        if ok == "false" and stderr_text and stderr_text != error_text:
            lines.append(f"stderr={stderr_text}")
        return lines

    def _failure_trace_lines(self, backend_result: dict) -> list[str]:
        traces = backend_result.get("failure_traces")
        if not isinstance(traces, list):
            return []

        lines: list[str] = []
        for trace in traces:
            if not isinstance(trace, dict):
                continue
            phase = str(trace.get("phase", "unknown") or "unknown")
            module = str(trace.get("module", "unknown") or "unknown")
            error_type = str(trace.get("error_type", "unknown") or "unknown")
            root_cause = str(trace.get("root_cause", "unknown") or "unknown")
            lines.append(
                f"[FAILURE TRACE] phase={phase} module={module} "
                f"error_type={error_type} root_cause={root_cause}"
            )
        return lines

    def _final_output_text(self, backend_result: dict) -> str:
        llm_output = str(
            backend_result.get("llm_output", backend_result.get("llmOutput", "")) or ""
        )
        if llm_output.strip():
            return llm_output.strip()

        output = str(backend_result.get("output", "") or "")
        if output.strip():
            return output.strip()

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
        has_llm_output_field = "llm_output" in normalized or "llmOutput" in normalized
        llm_output = str(normalized.get("llm_output", normalized.get("llmOutput", "")) or "")
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
        if has_llm_output_field:
            normalized["llm_output"] = llm_output
            if llm_output.strip():
                output = llm_output
        normalized["output"] = output
        if error:
            normalized["error"] = error
        elif status == "error":
            normalized["error"] = "backend_error"
        return normalized

    async def _invoke_ultra_via_ipc(
        self,
        cmd: str,
        payload: dict,
        project_root: str,
    ) -> Optional[dict]:
        state_dirs = [
            os.path.join(project_root, ".ultra_daemon"),
            os.path.join(project_root, ".ultra"),
        ]

        selected_state_dir = None
        for state_dir in state_dirs:
            daemon_pid = os.path.join(state_dir, "daemon.pid")
            pipe_name = os.path.join(state_dir, "pipe.name")
            daemon_sock = os.path.join(state_dir, "daemon.sock")
            if os.path.isfile(daemon_pid) and (
                os.path.isfile(pipe_name) or os.path.exists(daemon_sock)
            ):
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
            for key in (
                "status",
                "verify_status",
                "verifyStatus",
                "llm_output",
                "llmOutput",
                "output",
                "error",
            )
        ):
            return self._normalize_backend_result(ipc_response)
        return None

    def _pipe_reader(
        self,
        pipe,
        channel: str,
        event_queue: queue.Queue,
    ) -> None:
        try:
            for line in iter(pipe.readline, ""):
                if line == "":
                    break
                event_queue.put((channel, line))
        finally:
            with contextlib.suppress(Exception):
                pipe.close()
            event_queue.put((f"{channel}_closed", ""))

    def _parse_backend_json(self, stdout_text: str) -> Optional[dict]:
        candidate = stdout_text.strip()
        if not candidate:
            return None

        try:
            parsed = json.loads(candidate)
            return parsed if isinstance(parsed, dict) else None
        except (json.JSONDecodeError, ValueError):
            pass

        object_start = candidate.find("{")
        object_end = candidate.rfind("}")
        if object_start != -1 and object_end > object_start:
            try:
                parsed = json.loads(candidate[object_start : object_end + 1])
                return parsed if isinstance(parsed, dict) else None
            except json.JSONDecodeError:
                pass

        for line in reversed(candidate.splitlines()):
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                parsed = json.loads(line)
                return parsed if isinstance(parsed, dict) else None
            except json.JSONDecodeError:
                continue

        return None

    def _terminate_process(self, proc: subprocess.Popen) -> None:
        if proc.poll() is None:
            with contextlib.suppress(Exception):
                proc.kill()

    async def _collect_process_output(
        self,
        proc: subprocess.Popen,
        cmd: str,
        event_queue: queue.Queue,
        reader_threads: list[threading.Thread],
        live: Optional[LiveRenderState],
    ) -> dict:
        stdout_lines: list[str] = []
        stderr_lines: list[str] = []
        parsed_result: Optional[dict] = None
        reveal_task: Optional[asyncio.Task] = None
        stdout_closed = False
        stderr_closed = False
        timed_out = False
        deadline = time.monotonic() + self.PROCESS_TIMEOUT_SECONDS

        def drain_events() -> bool:
            nonlocal parsed_result
            nonlocal reveal_task
            nonlocal stdout_closed
            nonlocal stderr_closed

            drained_any = False
            while True:
                try:
                    channel, payload = event_queue.get_nowait()
                except queue.Empty:
                    break

                drained_any = True
                if channel == "stdout":
                    stdout_lines.append(payload)
                    if live is not None:
                        live.stdout_lines.append(payload)
                        self._handle_runtime_line(
                            payload.rstrip("\r\n"),
                            live,
                            channel="stdout",
                        )
                    if parsed_result is None:
                        candidate = self._parse_backend_json("".join(stdout_lines))
                        if candidate is not None:
                            parsed_result = self._normalize_backend_result(candidate)
                            if live is not None:
                                live.final_output_ready = True
                                final_text = self._final_output_text(parsed_result)
                                if final_text:
                                    reveal_task = asyncio.create_task(
                                        self._progressively_reveal_output(live, final_text)
                                    )
                elif channel == "stderr":
                    stderr_lines.append(payload)
                    if live is not None:
                        live.stderr_lines.append(payload)
                        self._handle_runtime_line(
                            payload.rstrip("\r\n"),
                            live,
                            channel="stderr",
                        )
                elif channel == "stdout_closed":
                    stdout_closed = True
                elif channel == "stderr_closed":
                    stderr_closed = True

            return drained_any

        try:
            while True:
                drained = drain_events()
                if proc.poll() is not None and stdout_closed and stderr_closed:
                    break
                if time.monotonic() >= deadline:
                    timed_out = True
                    self._terminate_process(proc)
                    break
                if not drained:
                    await asyncio.sleep(0.03)

            await asyncio.to_thread(proc.wait)
            await asyncio.sleep(0)
            drain_events()
        except asyncio.CancelledError:
            self._terminate_process(proc)
            with contextlib.suppress(Exception):
                await asyncio.to_thread(proc.wait)
            raise
        finally:
            for thread in reader_threads:
                thread.join(timeout=0.2)

        if reveal_task is not None:
            await reveal_task

        stdout_text = "".join(stdout_lines).strip()
        stderr_text = "".join(stderr_lines).strip()

        if parsed_result is None:
            parsed = self._parse_backend_json(stdout_text)
            if parsed is not None:
                parsed_result = self._normalize_backend_result(parsed)

        if parsed_result is not None:
            if stderr_text:
                parsed_result["_stderr"] = stderr_text
            return parsed_result

        if timed_out:
            return self._error_backend_response(
                f"ultra {cmd} timed out after {int(self.PROCESS_TIMEOUT_SECONDS)}s",
                output=self._combine_process_output(stdout_text, stderr_text),
            )

        if proc.returncode != 0 and not stdout_text:
            return self._error_backend_response(
                stderr_text or f"ultra {cmd} exited {proc.returncode}"
            )

        return self._error_backend_response(
            "ultra output was not valid JSON",
            output=self._combine_process_output(stdout_text, stderr_text),
        )

    async def _invoke_ultra(
        self,
        cmd: str,
        payload: dict,
        live: Optional[LiveRenderState] = None,
    ) -> dict:
        """
        Invoke the Ultra backend.

        `cognitive_run` is streamed through a non-blocking subprocess so stdout
        and stderr can be consumed independently without breaking the JSON
        contract.
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

        ipc_only_cmds = {"ai_status", "ai_query", "ai_source", "ai_context", "ai_impact"}
        if cmd in ipc_only_cmds:
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
                "ultra binary not found in PATH or build/. Run ultra build --release first."
            )

        payload_file: Optional[str] = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                delete=False,
                suffix=".json",
            ) as temp_payload:
                temp_payload.write(json.dumps(payload, ensure_ascii=False))
                payload_file = temp_payload.name

            proc = subprocess.Popen(
                [ultra_bin, cmd, "--file", payload_file],
                cwd=project_root,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )

            if proc.stdout is None or proc.stderr is None:
                return self._error_backend_response(
                    "ultra subprocess did not expose stdout/stderr pipes."
                )

            event_queue: queue.Queue = queue.Queue()
            reader_threads = [
                threading.Thread(
                    target=self._pipe_reader,
                    args=(proc.stdout, "stdout", event_queue),
                    daemon=True,
                ),
                threading.Thread(
                    target=self._pipe_reader,
                    args=(proc.stderr, "stderr", event_queue),
                    daemon=True,
                ),
            ]
            for thread in reader_threads:
                thread.start()

            return await self._collect_process_output(
                proc,
                cmd,
                event_queue,
                reader_threads,
                live,
            )
        except FileNotFoundError:
            return self._error_backend_response(f"ultra binary not executable: {ultra_bin}")
        except Exception as exc:  # noqa: BLE001
            return self._error_backend_response(str(exc))
        finally:
            if payload_file:
                with contextlib.suppress(OSError):
                    os.remove(payload_file)

    def _session_payload(self, session) -> dict:
        session_dict = session.to_dict()
        session_dict.pop("mode", None)
        return session_dict
