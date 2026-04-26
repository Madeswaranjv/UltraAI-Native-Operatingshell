"use strict";

const { spawn } = require("node:child_process");
const fs = require("node:fs/promises");
const path = require("node:path");

function tail(text, maxChars) {
  const value = String(text || "");
  if (value.length <= maxChars) {
    return value;
  }
  return value.slice(value.length - maxChars);
}

function toForwardSlash(value) {
  return String(value || "").replace(/\\/g, "/");
}

function uniqueStrings(values) {
  const out = [];
  const seen = new Set();
  for (const value of values || []) {
    const normalized = String(value || "").trim();
    if (!normalized || seen.has(normalized)) {
      continue;
    }
    seen.add(normalized);
    out.push(normalized);
  }
  return out;
}

function parseIntFromStatus(output, key) {
  const regex = new RegExp(`${key}:\\s*([0-9]+)`, "i");
  const match = String(output || "").match(regex);
  return match ? Number(match[1]) : 0;
}

function parseAiStatus(output) {
  const text = String(output || "");
  return {
    active: /AI runtime:\s*active/i.test(text),
    filesIndexed: parseIntFromStatus(text, "Files indexed"),
    symbolsIndexed: parseIntFromStatus(text, "Symbols indexed"),
    pendingChanges: parseIntFromStatus(text, "Pending changes")
  };
}

class UltraBridge {
  constructor(workspaceRoot, outputChannel) {
    this.workspaceRoot = workspaceRoot;
    this.output = outputChannel;
    this._contextPrimed = false;
  }

  _log(message) {
    if (this.output) {
      this.output.appendLine(`[bridge] ${message}`);
    }
  }

  _runCommand(executable, args, options) {
    const opts = options || {};
    const timeoutMs = Number.isFinite(opts.timeoutMs) ? opts.timeoutMs : 120000;
    const cwd = opts.cwd || this.workspaceRoot || process.cwd();

    return new Promise((resolve, reject) => {
      const child = spawn(executable, args || [], {
        cwd,
        shell: Boolean(opts.shell),
        env: process.env,
        windowsHide: true
      });

      let stdout = "";
      let stderr = "";
      let stdoutLineBuffer = "";
      let stderrLineBuffer = "";
      let timedOut = false;

      const killTimer = setTimeout(() => {
        timedOut = true;
        child.kill();
      }, timeoutMs);

      const onStdout = (chunk) => {
        const text = chunk.toString();
        stdout += text;
        if (typeof opts.onStdoutLine === "function") {
          stdoutLineBuffer += text;
          const parts = stdoutLineBuffer.split(/\r?\n/);
          stdoutLineBuffer = parts.pop() || "";
          for (const line of parts) {
            opts.onStdoutLine(line);
          }
        }
      };

      const onStderr = (chunk) => {
        const text = chunk.toString();
        stderr += text;
        if (typeof opts.onStderrLine === "function") {
          stderrLineBuffer += text;
          const parts = stderrLineBuffer.split(/\r?\n/);
          stderrLineBuffer = parts.pop() || "";
          for (const line of parts) {
            opts.onStderrLine(line);
          }
        }
      };

      child.stdout.on("data", onStdout);
      child.stderr.on("data", onStderr);

      child.on("error", (error) => {
        clearTimeout(killTimer);
        reject(error);
      });

      child.on("close", (code) => {
        clearTimeout(killTimer);
        if (stdoutLineBuffer && typeof opts.onStdoutLine === "function") {
          opts.onStdoutLine(stdoutLineBuffer);
        }
        if (stderrLineBuffer && typeof opts.onStderrLine === "function") {
          opts.onStderrLine(stderrLineBuffer);
        }
        resolve({
          code: Number.isFinite(code) ? code : -1,
          stdout,
          stderr,
          timedOut
        });
      });
    });
  }

  async runUltra(args, options) {
    try {
      return await this._runCommand("ultra", args, options);
    } catch (error) {
      if (error && error.code === "ENOENT") {
        throw new Error("`ultra` binary not found in PATH.");
      }
      throw error;
    }
  }

  async runShell(command, options) {
    return this._runCommand(command, [], Object.assign({}, options, { shell: true }));
  }

  async ensureDaemon(autoWake, progress) {
    if (typeof progress === "function") {
      progress("daemon_check", "Checking ULTRA daemon status...");
    }
    const statusResult = await this.runUltra(["ai_status"], { timeoutMs: 30000 });
    const status = parseAiStatus(statusResult.stdout);
    if (statusResult.code === 0 && status.active) {
      return status;
    }

    if (!autoWake) {
      throw new Error("ULTRA daemon is not active. Enable `ultra.autoWakeDaemon` or run `ultra wake_ai`.");
    }

    if (typeof progress === "function") {
      progress("daemon_check", "Starting ULTRA daemon...");
    }
    const wake = await this.runUltra(["wake_ai"], { timeoutMs: 60000 });
    if (wake.code !== 0) {
      throw new Error(`Failed to start ULTRA daemon.\n${wake.stderr || wake.stdout}`);
    }

    const secondStatus = await this.runUltra(["ai_status"], { timeoutMs: 30000 });
    const parsed = parseAiStatus(secondStatus.stdout);
    if (!parsed.active) {
      throw new Error("ULTRA daemon did not become active after wake_ai.");
    }
    return parsed;
  }

  async ensureContextAst(progress) {
    if (this._contextPrimed || !this.workspaceRoot) {
      return;
    }
    if (typeof progress === "function") {
      progress("index_context", "Analyzing repo...");
    }
    const result = await this.runUltra(["context", "--ast", "."], {
      cwd: this.workspaceRoot,
      timeoutMs: 180000
    });
    if (result.code === 0) {
      this._contextPrimed = true;
      return;
    }
    this._log("context --ast failed: " + tail(result.stderr || result.stdout, 800));
  }

  _resolveRelativePath(filePath) {
    if (!filePath || !this.workspaceRoot) {
      return null;
    }
    const absolute = path.isAbsolute(filePath) ? filePath : path.resolve(this.workspaceRoot, filePath);
    const relative = path.relative(this.workspaceRoot, absolute);
    if (!relative || relative.startsWith("..")) {
      return null;
    }
    return toForwardSlash(relative);
  }

  async _readFileFallback(relativePath, maxChars) {
    const absolute = path.resolve(this.workspaceRoot, relativePath);
    const text = await fs.readFile(absolute, "utf8");
    return text.length > maxChars
      ? text.slice(0, maxChars) + `\n\n[TRUNCATED to ${maxChars} chars]`
      : text;
  }

  async readFileContext(relativePath, maxChars) {
    const queryPath = toForwardSlash(relativePath);
    const aiSource = await this.runUltra(["ai_source", queryPath], {
      cwd: this.workspaceRoot,
      timeoutMs: 30000
    });
    const content = String(aiSource.stdout || "").trim();
    if (aiSource.code === 0 && content && content !== "{}") {
      return content.length > maxChars
        ? content.slice(0, maxChars) + `\n\n[TRUNCATED to ${maxChars} chars]`
        : content;
    }
    return this._readFileFallback(relativePath, maxChars);
  }

  async querySymbol(symbol) {
    if (!symbol) {
      return "";
    }
    const result = await this.runUltra(["ai_query", symbol], {
      cwd: this.workspaceRoot,
      timeoutMs: 30000
    });
    if (result.code !== 0) {
      return "";
    }
    return tail(result.stdout.trim(), 20000);
  }

  async collectTaskContext(request, settings, progress) {
    const maxChars = Number(settings.maxContextFileChars || 12000);
    const additional = Array.isArray(settings.additionalContextFiles)
      ? settings.additionalContextFiles
      : [];
    const candidateFiles = uniqueStrings(
      []
        .concat(request.current_file ? [request.current_file] : [])
        .concat(request.selected_files || [])
        .concat(additional)
    )
      .map((filePath) => this._resolveRelativePath(filePath))
      .filter(Boolean)
      .slice(0, 8);

    if (typeof progress === "function") {
      progress("collecting_context", "Selecting files...");
    }

    const files = [];
    for (const relativePath of candidateFiles) {
      try {
        const content = await this.readFileContext(relativePath, maxChars);
        files.push({ path: relativePath, content });
      } catch (error) {
        this._log(`context read failed for ${relativePath}: ${error.message}`);
      }
    }

    const symbolInfo = await this.querySymbol(request.symbol);
    return {
      workspaceRoot: this.workspaceRoot,
      mode: request.mode,
      prompt: request.task,
      selection: request.selection || "",
      currentFile: request.current_file || "",
      symbol: request.symbol || "",
      symbolInfo,
      files
    };
  }

  async runValidation(validationCommand, progress) {
    const command = String(validationCommand || "").trim();
    if (!command) {
      return {
        ran: false,
        passed: true,
        command: ""
      };
    }

    if (typeof progress === "function") {
      progress("validating", "Preparing validation environment...");
    }
    await this.runUltra(["sleep_ai"], {
      cwd: this.workspaceRoot,
      timeoutMs: 30000
    }).catch(() => undefined);

    if (typeof progress === "function") {
      progress("validating", `Running validation: ${command}`);
    }
    const result = await this.runShell(command, {
      cwd: this.workspaceRoot,
      timeoutMs: 20 * 60 * 1000
    });

    await this.runUltra(["wake_ai"], {
      cwd: this.workspaceRoot,
      timeoutMs: 60000
    }).catch(() => undefined);

    return {
      ran: true,
      passed: result.code === 0,
      command,
      exitCode: result.code,
      stdoutTail: tail(result.stdout, 12000),
      stderrTail: tail(result.stderr, 12000)
    };
  }

  async healthCheck(settings) {
    const checks = [];

    const ultraVersion = await this.runUltra(["version"], { timeoutMs: 10000 }).catch((error) => ({
      code: 1,
      stdout: "",
      stderr: error.message
    }));
    checks.push({
      name: "ULTRA binary",
      ok: ultraVersion.code === 0,
      details: ultraVersion.code === 0 ? (ultraVersion.stdout.trim() || "Detected") : ultraVersion.stderr
    });

    const statusResult = await this.runUltra(["ai_status"], { timeoutMs: 15000 }).catch((error) => ({
      code: 1,
      stdout: "",
      stderr: error.message
    }));
    const parsedStatus = parseAiStatus(statusResult.stdout);
    checks.push({
      name: "ULTRA daemon",
      ok: statusResult.code === 0 && parsedStatus.active,
      details:
        statusResult.code === 0
          ? parsedStatus.active
            ? `Active, ${parsedStatus.filesIndexed} files indexed`
            : "Inactive"
          : statusResult.stderr || "Status command failed"
    });

    checks.push({
      name: "Workspace indexed",
      ok: parsedStatus.filesIndexed > 0,
      details: parsedStatus.filesIndexed > 0 ? `${parsedStatus.filesIndexed} files indexed` : "No indexed files detected"
    });

    return checks;
  }
}

module.exports = {
  UltraBridge
};
