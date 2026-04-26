# ULTRA VS Code Extension (Phase 2 MVP)

This extension turns ULTRA into an installable weekly-use workflow inside VS Code:

1. Ask ULTRA in the sidebar (`Fix Bug`, `Explain Code`, `Refactor`, `Add Tests`, `Optimize`)
2. Review generated per-file diff
3. Accept/reject files
4. Apply selected changes
5. Run validation command
6. Roll back in one click if needed

## Features in this MVP

- Workspace auto-detection
- Context awareness:
  - active file
  - selected text
  - current symbol
  - visible files
  - workspace root
- ULTRA daemon bridge (`ai_status`, `wake_ai`, `context --ast`)
- Provider layer:
  - Ollama
  - OpenAI
  - Anthropic
- Streaming progress updates in panel
- Trust workflow:
  - diff preview
  - per-file accept/reject
  - manual apply
  - rollback snapshot
- Validation integration (build/test command)
- Privacy-safe local analytics (no code content)
- Health check command

## Development

Open this folder in VS Code and run `F5` to launch Extension Development Host.

Settings are under `ULTRA` in VS Code settings:

- `ultra.provider`
- `ultra.model`
- `ultra.apiKey`
- `ultra.temperature`
- `ultra.maxTokens`
- `ultra.ollamaEndpoint`
- `ultra.validationCommand`

## Protocol

See [docs/protocol.md](docs/protocol.md) for extension runtime request/response envelopes.
