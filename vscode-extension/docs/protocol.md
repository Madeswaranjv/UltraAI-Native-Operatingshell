# Extension Runtime Protocol (MVP)

The VS Code extension uses a stable envelope between UI and task engine.

## Request Envelope

```json
{
  "request_id": "req_01H...",
  "type": "apply_task",
  "payload": {
    "task": "fix failing test",
    "mode": "fixBug",
    "workspace": "E:/Projects/MyRepo",
    "selected_files": ["src/a.cpp", "tests/a_test.cpp"],
    "selection": "selected editor text",
    "symbol": "Foo::bar",
    "model": "qwen2.5-coder:7b"
  }
}
```

## Status Events

```json
{
  "request_id": "req_01H...",
  "status": "running",
  "stage": "analyzing_repo",
  "message": "Analyzing repo..."
}
```

Stages currently emitted:

- `queueing`
- `daemon_check`
- `index_context`
- `collecting_context`
- `generating`
- `review_ready`
- `applying`
- `validating`
- `completed`
- `failed`

## Completion (Patch)

```json
{
  "request_id": "req_01H...",
  "status": "completed",
  "result_type": "patch_review",
  "files_used": ["src/a.cpp", "tests/a_test.cpp"],
  "why_changed": "Fix null dereference in parser init path.",
  "diff": "diff --git ...",
  "validation": {
    "ran": true,
    "passed": true,
    "command": "ctest --output-on-failure"
  }
}
```

## Completion (Explain)

```json
{
  "request_id": "req_01H...",
  "status": "completed",
  "result_type": "explain",
  "files_used": ["src/a.cpp"],
  "explanation_markdown": "..."
}
```
