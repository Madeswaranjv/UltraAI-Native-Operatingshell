"use strict";

const fs = require("node:fs/promises");
const path = require("node:path");
const { extractUnifiedDiffFromText } = require("./diffUtils");
const { ModelConfigLoader } = require("./config/ModelConfigLoader");
const { safeLog } = require("./logging");
const { RouterProvider } = require("./providers/RouterProvider");

function parseJsonSafe(text) {
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

function extractJsonObject(text) {
  const input = String(text || "");
  const direct = parseJsonSafe(input);
  if (direct && typeof direct === "object") {
    return direct;
  }

  let depth = 0;
  let start = -1;
  for (let i = 0; i < input.length; i += 1) {
    const ch = input[i];
    if (ch === "{") {
      if (depth === 0) {
        start = i;
      }
      depth += 1;
      continue;
    }
    if (ch === "}") {
      depth -= 1;
      if (depth === 0 && start >= 0) {
        const candidate = input.slice(start, i + 1);
        const parsed = parseJsonSafe(candidate);
        if (parsed && typeof parsed === "object") {
          return parsed;
        }
        start = -1;
      }
    }
  }
  return null;
}

function toArray(value) {
  if (!value) {
    return [];
  }
  if (Array.isArray(value)) {
    return value.filter((item) => typeof item === "string");
  }
  return [];
}

function isPatchMode(mode) {
  return mode !== "explain" && mode !== "health";
}

function buildContextBlock(taskContext) {
  const lines = [];
  lines.push(`Workspace: ${taskContext.workspaceRoot || "<none>"}`);
  lines.push(`Mode: ${taskContext.mode}`);
  lines.push(`Prompt: ${taskContext.prompt}`);
  if (taskContext.selection) {
    lines.push("\nSelected Text:\n" + taskContext.selection);
  }
  if (taskContext.currentFile) {
    lines.push(`\nActive File: ${taskContext.currentFile}`);
  }
  if (taskContext.symbol) {
    lines.push(`Current Symbol: ${taskContext.symbol}`);
  }
  if (taskContext.symbolInfo) {
    lines.push(`\nSymbol Query:\n${taskContext.symbolInfo}`);
  }
  for (const file of taskContext.files || []) {
    lines.push(`\n[FILE] ${file.path}\n${file.content}`);
  }
  return lines.join("\n");
}

class ModelProviderClient {
  constructor(outputChannel) {
    this._output = outputChannel;
    this.configLoader = null;
    this.router = null;
  }

  _log(message) {
    safeLog(this._output, `[provider-client] ${message}`, "[provider-client]");
  }

  _ensureRouter(workspaceRoot) {
    if (!this.configLoader || this.configLoader.workspaceRoot !== workspaceRoot) {
      this.configLoader = new ModelConfigLoader(workspaceRoot);
      this.router = new RouterProvider(this.configLoader, this._output);
    }
  }

  _systemPrompt(mode) {
    if (mode === "explain") {
      return [
        "You are ULTRA's code explainer.",
        "Be precise, concise, and factual.",
        "Use concrete references to files and symbols when available.",
        "Do not invent files or behavior."
      ].join(" ");
    }
    return [
      "You are ULTRA's patch planner.",
      "Return a JSON object only.",
      "No markdown fences, no prose outside JSON.",
      "Schema:",
      '{"summary":"...", "why_changed":"...", "files_used":["..."], "diff":"<unified diff>", "validation":"..."}',
      "The diff must be a valid unified diff that can be applied."
    ].join(" ");
  }

  _buildMessages(mode, taskContext) {
    const system = this._systemPrompt(mode);
    if (mode === "explain") {
      return [
        { role: "system", content: system },
        {
          role: "user",
          content: [
            "Explain this request with context-aware reasoning.",
            buildContextBlock(taskContext),
            "Return plain markdown text."
          ].join("\n\n")
        }
      ];
    }

    return [
      { role: "system", content: system },
      {
        role: "user",
        content: [
          "Generate a safe patch plan for this request.",
          buildContextBlock(taskContext),
          "Return JSON only with keys: summary, why_changed, files_used, diff, validation.",
          "Use only files that appear in context unless absolutely necessary."
        ].join("\n\n")
      }
    ];
  }

  _parseOutput(mode, raw, fallbackFiles) {
    if (mode === "explain") {
      const json = extractJsonObject(raw);
      if (json && typeof json.explanation === "string") {
        return {
          type: "explain",
          explanation: json.explanation,
          summary: typeof json.summary === "string" ? json.summary : "Explanation generated.",
          whyChanged: typeof json.why_changed === "string" ? json.why_changed : "",
          filesUsed: toArray(json.files_used).length ? toArray(json.files_used) : fallbackFiles
        };
      }
      return {
        type: "explain",
        explanation: String(raw || ""),
        summary: "Explanation generated.",
        whyChanged: "",
        filesUsed: fallbackFiles
      };
    }

    const json = extractJsonObject(raw);
    const diffFromJson = json && typeof json.diff === "string" ? json.diff.trim() : "";
    const diff = diffFromJson || extractUnifiedDiffFromText(raw);
    if (!diff) {
      return null;
    }
    return {
      type: "patch",
      summary: json && typeof json.summary === "string" ? json.summary : "Patch generated.",
      whyChanged: json && typeof json.why_changed === "string" ? json.why_changed : "",
      filesUsed: json && toArray(json.files_used).length ? toArray(json.files_used) : fallbackFiles,
      validation: json && typeof json.validation === "string" ? json.validation : "",
      diff
    };
  }

  async _writeDebugLog(workspaceRoot, isInvalid, details) {
    if (!workspaceRoot) return;
    try {
      const debugDir = path.join(workspaceRoot, ".ultra", "debug");
      await fs.mkdir(debugDir, { recursive: true });
      const filename = isInvalid ? "last_invalid_patch.md" : "last_valid_patch.md";
      const filePath = path.join(debugDir, filename);

      let content = "";
      if (isInvalid) {
        content = `# ULTRA Invalid Patch Report
**Timestamp:** ${new Date().toISOString()}
**Workspace:** ${workspaceRoot}
**Mode:** ${details.mode}
**Prompt:** ${details.prompt || "N/A"}
**Provider:** ${details.provider}
**Model:** ${details.model}

## Expected Unified Diff Format
\`\`\`diff
--- a/file.cpp
+++ b/file.cpp
@@ -10,3 +10,3 @@
-old line
+new line
 unchanged context
\`\`\`

## Raw Model Response
\`\`\`raw
${details.raw}
\`\`\`

## Validation Failure Reason
${details.reason}

## Suggested Retry Prompt
Return ONLY raw unified diff.
No markdown.
No explanation.
Start with --- a/<file>
`;
      } else {
        content = `# ULTRA Valid Patch Report
**Timestamp:** ${new Date().toISOString()}
**Workspace:** ${workspaceRoot}
**Mode:** ${details.mode}
**Provider:** ${details.provider}
**Model:** ${details.model}
**Latency:** ${details.latencyMs}ms

## Parsed Diff Summary
**Files Touched:** ${(details.filesTouched || []).join(", ")}

## Raw Model Response
\`\`\`raw
${details.raw}
\`\`\`
`;
      }
      await fs.writeFile(filePath, content, "utf8");
      this._log(`[patch-debug] wrote ${isInvalid ? "invalid" : "valid"} patch report: ${filePath}`);
      if (isInvalid) {
        this._log(`[patch-debug] parser reason: ${details.reason}`);
      }
    } catch (err) {
      this._log(`[patch-debug] failed to write debug report: ${err.message}`);
    }
  }

  async generateRaw(mode, messages, workspaceRoot, onEvent) {
    this._ensureRouter(workspaceRoot);
    const result = await this.router.generate(mode, messages, onEvent);
    return result;
  }

  async generate(mode, taskContext, settingsOrOnEvent, maybeOnEvent) {
    this._ensureRouter(taskContext.workspaceRoot);
    const onEvent = typeof maybeOnEvent === "function"
      ? maybeOnEvent
      : typeof settingsOrOnEvent === "function"
        ? settingsOrOnEvent
        : undefined;
    
    if (mode === "health") {
      this._log(`mode=${mode} - Executing health check via router.`);
      const checks = await this.router.checkHealth();
      return { type: "health", checks };
    }

    const filesFallback = (taskContext.files || []).map((file) => file.path);
    const messages = this._buildMessages(mode, taskContext);

    const firstResult = await this.router.generate(mode, messages, onEvent);
    let parsed = this._parseOutput(mode, firstResult.content, filesFallback);
    
    if (parsed) {
      if (isPatchMode(mode)) {
        await this._writeDebugLog(taskContext.workspaceRoot, false, {
          mode,
          provider: firstResult.provider,
          model: firstResult.model,
          latencyMs: firstResult.latencyMs,
          filesTouched: parsed.filesUsed,
          raw: firstResult.content
        });
      }
      parsed.retries = 0;
      parsed.provider = firstResult.provider;
      parsed.model = firstResult.model;
      parsed.latencyMs = firstResult.latencyMs;
      parsed.fallbackUsed = firstResult.fallbackUsed;
      return parsed;
    }

    if (isPatchMode(mode)) {
      if (typeof onEvent === "function") {
        onEvent("retrying", "Retrying with stricter patch format...");
      }
      const retryMessages = messages.concat([
        {
          role: "user",
          content: [
            "Previous response was invalid.",
            "Return JSON only with valid unified diff in `diff` field.",
            "Do not include markdown fences."
          ].join("\n")
        }
      ]);
      
      const retryResult = await this.router.generate(mode, retryMessages, onEvent);
      parsed = this._parseOutput(mode, retryResult.content, filesFallback);
      
      if (parsed) {
        await this._writeDebugLog(taskContext.workspaceRoot, false, {
          mode,
          provider: retryResult.provider,
          model: retryResult.model,
          latencyMs: retryResult.latencyMs,
          filesTouched: parsed.filesUsed,
          raw: retryResult.content
        });
        parsed.retries = 1;
        parsed.provider = retryResult.provider;
        parsed.model = retryResult.model;
        parsed.latencyMs = retryResult.latencyMs;
        parsed.fallbackUsed = retryResult.fallbackUsed;
        return parsed;
      }

      await this._writeDebugLog(taskContext.workspaceRoot, true, {
        mode,
        prompt: JSON.stringify(retryMessages[retryMessages.length - 1]),
        provider: retryResult.provider,
        model: retryResult.model,
        raw: retryResult.content,
        reason: "Missing --- / +++ headers or @@ hunk markers"
      });
      
      const err = new Error("Model response did not include a valid unified diff.");
      err.isInvalidPatch = true;
      throw err;
    }

    throw new Error("Model response did not include a valid output format.");
  }
}

module.exports = {
  ModelProviderClient,
  extractJsonObject // Exported for testing if needed
};
