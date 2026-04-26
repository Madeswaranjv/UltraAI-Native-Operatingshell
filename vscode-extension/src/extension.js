"use strict";

const vscode = require("vscode");
const path = require("node:path");
const fs = require("node:fs/promises");
const crypto = require("node:crypto");

const { WorkspaceTaskQueue } = require("./taskQueue");
const { AnalyticsStore } = require("./analyticsStore");
const { RollbackStore } = require("./rollbackStore");
const { UltraBridge } = require("./ultraBridge");
const { ModelProviderClient } = require("./modelProvider");
const {
  applyPatchesToWorkspace,
  buildPatchReview,
  collectSnapshotEntries,
  parseUnifiedDiff
} = require("./diffUtils");
const { getWebviewHtml } = require("./webviewHtml");

const PENDING_PATCH_KEY = "ultra.pendingPatch";
const PATCH_MODES = new Set(["fixBug", "refactor", "addTests", "optimize", "architecture", "heavy"]);

const MODE_LABELS = {
  fixBug:       "Fix Bug",
  explain:      "Explain Code",
  refactor:     "Refactor",
  addTests:     "Add Tests",
  optimize:     "Optimize",
  architecture: "Architecture",
  heavy:        "Heavy Reasoning"
};

console.log("[ULTRA] module loaded");

function createRequestId() {
  return "req_" + Date.now().toString(36) + "_" + crypto.randomBytes(4).toString("hex");
}

function workspaceRootPath() {
  const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  return folder ? folder.uri.fsPath : null;
}

function hashWorkspaceKey(workspaceRoot) {
  return crypto.createHash("sha1").update(workspaceRoot || "__global__").digest("hex").slice(0, 12);
}

function relativeToWorkspace(workspaceRoot, absolutePath) {
  if (!workspaceRoot || !absolutePath) {
    return null;
  }
  const relative = path.relative(workspaceRoot, absolutePath);
  if (!relative || relative.startsWith("..")) {
    return null;
  }
  return relative.replace(/\\/g, "/");
}

async function currentEditorSymbol(editor) {
  if (!editor || editor.document.uri.scheme !== "file") {
    return "";
  }
  const symbols = await vscode.commands.executeCommand(
    "vscode.executeDocumentSymbolProvider",
    editor.document.uri
  );
  if (!Array.isArray(symbols) || symbols.length === 0) {
    return "";
  }

  const target = editor.selection.active;
  let best = null;
  const rangeSpan = (range) => {
    const lineDelta = range.end.line - range.start.line;
    const charDelta = range.end.character - range.start.character;
    return lineDelta * 100000 + charDelta;
  };
  const stack = [...symbols];
  while (stack.length > 0) {
    const item = stack.pop();
    if (!item || !item.range || !item.range.contains(target)) {
      continue;
    }
    if (!best || rangeSpan(item.range) <= rangeSpan(best.range)) {
      best = item;
    }
    if (Array.isArray(item.children)) {
      for (const child of item.children) {
        stack.push(child);
      }
    }
  }
  return best ? best.name : "";
}

async function collectEditorContext(workspaceRoot) {
  const editor = vscode.window.activeTextEditor;
  const selectedFiles = [];

  if (editor && editor.document.uri.scheme === "file") {
    const rel = relativeToWorkspace(workspaceRoot, editor.document.uri.fsPath);
    if (rel) {
      selectedFiles.push(rel);
    }
  }

  for (const visible of vscode.window.visibleTextEditors) {
    if (visible.document.uri.scheme !== "file") {
      continue;
    }
    const rel = relativeToWorkspace(workspaceRoot, visible.document.uri.fsPath);
    if (rel) {
      selectedFiles.push(rel);
    }
  }

  const uniqueFiles = [...new Set(selectedFiles)].slice(0, 12);
  const selection = editor
    ? editor.document.getText(editor.selection).trim()
    : "";
  const symbol = editor ? await currentEditorSymbol(editor) : "";
  const currentFile = editor && editor.document.uri.scheme === "file"
    ? relativeToWorkspace(workspaceRoot, editor.document.uri.fsPath)
    : "";

  return {
    selection,
    symbol,
    currentFile,
    selectedFiles: uniqueFiles
  };
}

/**
 * Returns only the VSCode settings that are still relevant.
 * Provider/model/apiKey configuration all comes from .ultra/models.json.
 */
function getUltraSettings() {
  const config = vscode.workspace.getConfiguration("ultra");
  return {
    validationCommand: config.get(
      "validationCommand",
      "ctest --test-dir build -C Release --output-on-failure"
    ),
    maxContextFileChars:    config.get("maxContextFileChars", 12000),
    autoWakeDaemon:         config.get("autoWakeDaemon", true),
    enableAnalytics:        config.get("enableAnalytics", true),
    additionalContextFiles: config.get("additionalContextFiles", [])
  };
}

class UltraSidebarProvider {
  constructor(context, outputChannel, queue) {
    this.context = context;
    this.output = outputChannel;
    this.queue = queue;
    this.view = null;
    this.modelProvider = new ModelProviderClient(outputChannel);
    this._configUnwatch = null; // hot-reload watcher cleanup
  }

  _workspaceRoot() {
    return workspaceRootPath();
  }

  async _workspaceStorage() {
    const workspaceRoot = this._workspaceRoot();
    const base = this.context.globalStorageUri.fsPath;
    const key = hashWorkspaceKey(workspaceRoot || "__global__");
    const dir = path.join(base, "workspaces", key);
    await fs.mkdir(dir, { recursive: true });
    return {
      workspaceRoot,
      dir
    };
  }

  _post(message) {
    if (!this.view) {
      return false;
    }
    this.view.webview.postMessage(message);
    return true;
  }

  _emitStatus(stage, message) {
    this._post({
      type: "status",
      stage,
      message
    });
    this.output.appendLine(`[status] ${stage}: ${message}`);
  }

  async resolveWebviewView(webviewView) {
    this.view = webviewView;
    webviewView.webview.options = {
      enableScripts: true
    };
    webviewView.webview.html = getWebviewHtml(webviewView.webview);

    webviewView.webview.onDidReceiveMessage(async (message) => {
      if (!message || typeof message.type !== "string") {
        return;
      }
      if (message.type === "runTask") {
        await this.runTask(message.mode, message.prompt);
        return;
      }
      if (message.type === "applyPatch") {
        await this.applyPatch(message.request_id, message.accepted_paths || []);
        return;
      }
      if (message.type === "healthCheck") {
        await this.runHealthCheck();
        return;
      }
      if (message.type === "rollback") {
        await this.rollbackLast();
        return;
      }
      if (message.type === "openSettings") {
        await vscode.commands.executeCommand("workbench.action.openSettings", "@ext:ultra-infinity.ultra-vscode");
        return;
      }
      if (message.type === "getModels") {
        await this.sendModelsToWebview();
        return;
      }
      if (message.type === "updateRoute") {
        await this.updateModelRoute(message.mode, message.routeDest);
        return;
      }
    });

    // Start hot-reload watcher on models.json
    this._startConfigWatcher();

    // Stop watcher when panel is disposed
    webviewView.onDidDispose(() => {
      if (this._configUnwatch) {
        this._configUnwatch();
        this._configUnwatch = null;
      }
    });

    await this.restorePendingPatch();
  }

  _startConfigWatcher() {
    const workspaceRoot = this._workspaceRoot();
    if (!workspaceRoot) return;

    // Stop any prior watcher
    if (this._configUnwatch) {
      this._configUnwatch();
      this._configUnwatch = null;
    }

    const { ModelConfigLoader } = require("./config/ModelConfigLoader");
    const loader = new ModelConfigLoader(workspaceRoot);

    this._configUnwatch = loader.watch((err, config) => {
      if (err) {
        this.output.appendLine(`[config-watcher] Reload error: ${err.message}`);
        this._post({ type: "configError", message: err.message });
        return;
      }
      this.output.appendLine(`[config-watcher] models.json changed — hot reload applied`);

      // Build a simple route summary for the UI
      const defaultKey = config && config.default_provider;
      const defaultProv = config && config.providers && config.providers[defaultKey];
      const routes = (defaultProv && (defaultProv.type === "router" || defaultProv.type === "hybrid"))
        ? defaultProv.routes || {}
        : {};

      const routeSummary = Object.entries(routes).map(([mode, dest]) => ({ mode, dest }));
      this._post({ type: "configReloaded", defaultProvider: defaultKey, routes: routeSummary });
      this.sendModelsToWebview(); // refresh models panel
    });
  }

  async sendModelsToWebview() {
    try {
      const storage = await this._workspaceStorage();
      this.modelProvider._ensureRouter(storage.workspaceRoot);
      const fullConfig = await this.modelProvider.configLoader.getConfig();
      if (!fullConfig || !fullConfig.providers) return;

      const providersDetected = [];
      const modelsByProvider = {};
      const routes = {};

      const defaultProv = fullConfig.providers[fullConfig.default_provider];
      if (defaultProv && (defaultProv.type === "router" || defaultProv.type === "hybrid")) {
        Object.assign(routes, defaultProv.routes || {});
      }

      for (const [provKey, provConfig] of Object.entries(fullConfig.providers)) {
        if (provConfig.type === "router" || provConfig.type === "hybrid") continue;
        providersDetected.push({ key: provKey, type: provConfig.type });
        modelsByProvider[provKey] = [];
        if (provConfig.models) {
          for (const [role, modelName] of Object.entries(provConfig.models)) {
            modelsByProvider[provKey].push({ role, modelName, routeStr: `${provKey}.${role}` });
          }
        }
      }

      this._post({
        type: "modelsData",
        providers: providersDetected,
        modelsByProvider,
        routes
      });
    } catch (e) {
      this.output.appendLine(`[extension] sendModelsToWebview error: ${e.message}`);
    }
  }

  async updateModelRoute(mode, routeDest) {
    try {
      const storage = await this._workspaceStorage();
      this.modelProvider._ensureRouter(storage.workspaceRoot);
      await this.modelProvider.configLoader.updateRoute(mode, routeDest);
      this.output.appendLine(`[extension] Updated route for ${mode} to ${routeDest}`);
    } catch (e) {
      this._post({ type: "taskError", message: `Failed to save route: ${e.message}` });
    }
  }

  async _analyticsStore(storageDir, enabled) {
    const analytics = new AnalyticsStore(path.join(storageDir, "analytics"), enabled);
    await analytics.init();
    return analytics;
  }

  async _bridge(workspaceRoot) {
    if (!workspaceRoot) {
      throw new Error("Open a workspace folder to use ULTRA.");
    }
    return new UltraBridge(workspaceRoot, this.output);
  }

  async restorePendingPatch() {
    const pending = this.context.workspaceState.get(PENDING_PATCH_KEY);
    if (!pending || !pending.request_id) {
      return;
    }
    this._post({
      type: "restorePending",
      pending
    });
  }

  async runTask(mode, prompt) {
    const normalizedMode = MODE_LABELS[mode] ? mode : "fixBug";
    const trimmedPrompt = String(prompt || "").trim();
    if (!trimmedPrompt) {
      this._post({ type: "taskError", message: "Task prompt is required." });
      return;
    }

    const settings = getUltraSettings();
    const storage = await this._workspaceStorage();
    const workspaceRoot = storage.workspaceRoot;
    const analytics = await this._analyticsStore(storage.dir, settings.enableAnalytics);
    const bridge = await this._bridge(workspaceRoot);
    const requestId = createRequestId();
    const start = Date.now();

    const editorContext = await collectEditorContext(workspaceRoot);
    const payload = {
      task: trimmedPrompt,
      mode: normalizedMode,
      workspace: workspaceRoot,
      selected_files: editorContext.selectedFiles,
      selection: editorContext.selection,
      symbol: editorContext.symbol,
      current_file: editorContext.currentFile
    };
    const envelope = {
      request_id: requestId,
      type: "apply_task",
      payload
    };

    await analytics.recordTaskStart(normalizedMode);

    try {
      await this.queue.run(workspaceRoot, { patchTask: PATCH_MODES.has(normalizedMode) }, async () => {
        this._emitStatus("queueing", `Task queued: ${MODE_LABELS[normalizedMode]}`);
        await bridge.ensureDaemon(settings.autoWakeDaemon, (stage, message) => this._emitStatus(stage, message));
        await bridge.ensureContextAst((stage, message) => this._emitStatus(stage, message));

        const context = await bridge.collectTaskContext(payload, settings, (stage, message) =>
          this._emitStatus(stage, message)
        );

        this._emitStatus(
          "generating",
          normalizedMode === "explain" ? "Generating explanation..." : "Generating patch..."
        );
        const modelResult = await this.modelProvider.generate(
          normalizedMode,
          context,
          settings,
          (stage, message) => this._emitStatus(stage, message)
        );

        if (modelResult.type === "explain" || normalizedMode === "explain") {
          this._post({
            type: "explainResult",
            request_id: requestId,
            summary: modelResult.summary,
            files_used: modelResult.filesUsed || [],
            content: modelResult.explanation || ""
          });
          this._emitStatus("completed", "Done.");
          return;
        }

        // Determine the best fallback filename for the case where the model
        // returns a bare hunk-only diff with no --- / +++ headers.
        // This is the primary cause of "No files were applied".
        const filesUsed = modelResult.filesUsed || context.files.map((file) => file.path);
        const fallbackFilename = filesUsed[0] || editorContext.currentFile || null;

        const patches = parseUnifiedDiff(modelResult.diff, fallbackFilename);
        if (!patches.length) {
          throw new Error("Model did not return an actionable unified diff.");
        }
        const patchReview = buildPatchReview(modelResult.diff, fallbackFilename);

        const pending = {
          request_id: envelope.request_id,
          type: "review",
          summary: modelResult.summary || "Patch ready",
          why_changed: modelResult.whyChanged || "",
          files_used: modelResult.filesUsed || context.files.map((file) => file.path),
          diff: modelResult.diff,
          patches: patchReview,
          created_at: new Date().toISOString()
        };
        await this.context.workspaceState.update(PENDING_PATCH_KEY, pending);

        this._post({
          type: "reviewReady",
          request_id: envelope.request_id,
          summary: pending.summary,
          why_changed: pending.why_changed,
          files_used: pending.files_used,
          patches: patchReview
        });
        this._emitStatus("review_ready", "Patch generated. Review and apply selected files.");
      });

      await analytics.recordTaskComplete({
        success: true,
        latencyMs: Date.now() - start,
        retries: 0
      });
    } catch (error) {
      if (error.isInvalidPatch) {
        vscode.window.showErrorMessage(`ULTRA patch invalid. Debug report saved to .ultra/debug/last_invalid_patch.md`);
      }
      this._post({ type: "taskError", message: error.message || String(error) });
      this._emitStatus("failed", error.message || "Task failed.");
      await analytics.recordTaskComplete({
        success: false,
        latencyMs: Date.now() - start,
        retries: 0
      });
    }
  }

  async applyPatch(requestId, acceptedPaths) {
    const settings = getUltraSettings();
    const storage = await this._workspaceStorage();
    const workspaceRoot = storage.workspaceRoot;
    const analytics = await this._analyticsStore(storage.dir, settings.enableAnalytics);
    const bridge = await this._bridge(workspaceRoot);
    const rollbackStore = new RollbackStore(workspaceRoot, path.join(storage.dir, "rollback"));
    await rollbackStore.init();

    const pending = this.context.workspaceState.get(PENDING_PATCH_KEY);
    if (!pending || !pending.request_id) {
      this._post({ type: "taskError", message: "No patch review is pending." });
      return;
    }
    if (requestId && pending.request_id !== requestId) {
      this._post({ type: "taskError", message: "Pending patch request id does not match." });
      return;
    }

    const selected = Array.isArray(acceptedPaths)
      ? [...new Set(acceptedPaths.map((item) => String(item).trim()).filter(Boolean))]
      : [];
    if (selected.length === 0) {
      this._post({ type: "taskError", message: "Select at least one file to apply." });
      return;
    }

    // Re-derive the fallback filename from the persisted review data so that
    // bare hunk-only diffs (missing --- / +++ headers) are still resolvable
    // to their target file at apply-time, not just at review-time.
    const pendingFallback = (pending.files_used && pending.files_used[0]) || null;
    const patches = parseUnifiedDiff(pending.diff, pendingFallback);

    try {
      await this.queue.run(workspaceRoot, { patchTask: true }, async () => {
        this._emitStatus("applying", "Applying selected patch files...");
        const snapshotPrep = await collectSnapshotEntries(workspaceRoot, patches, selected);
        if (snapshotPrep.selectedCount === 0) {
          throw new Error("No selected files matched generated diff paths.");
        }
        const snapshotId = await rollbackStore.saveSnapshot(snapshotPrep.entries, {
          request_id: pending.request_id,
          files: selected
        });

        let applyResult;
        try {
          applyResult = await applyPatchesToWorkspace(workspaceRoot, patches, selected);
        } catch (error) {
          if (snapshotId) {
            await rollbackStore.restoreById(snapshotId);
          }
          throw error;
        }

        if (applyResult.appliedPaths.length === 0) {
          throw new Error("No files were applied.");
        }

        await vscode.workspace.saveAll(false);
        const validation = await bridge.runValidation(settings.validationCommand, (stage, message) =>
          this._emitStatus(stage, message)
        );

        await this.context.workspaceState.update(PENDING_PATCH_KEY, undefined);
        await analytics.recordPatchAccepted();

        const summary = validation.ran
          ? validation.passed
            ? "Patch applied and validation passed."
            : "Patch applied but validation failed."
          : "Patch applied.";

        this._post({
          type: "applyResult",
          message: summary + (snapshotId ? ` Rollback snapshot: ${snapshotId}.` : ""),
          validation,
          clear_review: true
        });
        this._emitStatus("completed", summary);
      });
    } catch (error) {
      this._post({ type: "taskError", message: error.message || String(error) });
      this._emitStatus("failed", error.message || "Patch apply failed.");
    }
  }

  async runHealthCheck() {
    try {
      const settings = getUltraSettings();
      const storage = await this._workspaceStorage();
      const bridge = await this._bridge(storage.workspaceRoot);
      const bridgeChecks = await bridge.healthCheck(settings);
      
      const modelChecksResp = await this.modelProvider.generate("health", { workspaceRoot: storage.workspaceRoot });
      const modelChecks = (modelChecksResp.checks || []).map(c => ({
         name: `Provider: ${c.provider}`,
         ok: c.ok,
         details: c.message
      }));
      
      const checks = [...bridgeChecks, ...modelChecks];
      
      const posted = this._post({ type: "healthResult", checks });
      if (!posted) {
        const failed = checks.filter((check) => !check.ok).length;
        const summary = failed === 0 ? "ULTRA health check passed." : `ULTRA health check found ${failed} issue(s).`;
        vscode.window.showInformationMessage(summary);
      }
    } catch (error) {
      this._post({ type: "taskError", message: error.message || String(error) });
    }
  }

  async rollbackLast() {
    try {
      const settings = getUltraSettings();
      const storage = await this._workspaceStorage();
      const rollbackStore = new RollbackStore(storage.workspaceRoot, path.join(storage.dir, "rollback"));
      await rollbackStore.init();
      const result = await rollbackStore.restoreLatest();
      if (result.restored) {
        await vscode.workspace.saveAll(false);
        const analytics = await this._analyticsStore(storage.dir, settings.enableAnalytics);
        await analytics.recordRollback();
      }
      const posted = this._post({
        type: "rollbackResult",
        message: result.message
      });
      if (!posted) {
        vscode.window.showInformationMessage(result.message);
      }
    } catch (error) {
      this._post({ type: "taskError", message: error.message || String(error) });
    }
  }

  async promptAndRun(mode) {
    const value = await vscode.window.showInputBox({
      title: `ULTRA: ${MODE_LABELS[mode] || "Task"}`,
      placeHolder: "Describe the task...",
      ignoreFocusOut: true
    });
    if (!value) {
      return;
    }
    await this.runTask(mode, value);
  }
}

let providerInstance;

function activate(context) {
  console.log("[ULTRA] activate entered");
  const output = vscode.window.createOutputChannel("ULTRA");
  output.appendLine("[lifecycle] activate entered");

  try {
    const queue = new WorkspaceTaskQueue();
    providerInstance = new UltraSidebarProvider(context, output, queue);

    context.subscriptions.push(output);

    output.appendLine("[lifecycle] registering webview provider ultra.sidebar");
    context.subscriptions.push(
      vscode.window.registerWebviewViewProvider("ultra.sidebar", providerInstance)
    );

    output.appendLine("[lifecycle] commands registering");
    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.openPanel", async () => {
        await vscode.commands.executeCommand("workbench.view.extension.ultra");
      })
    );

    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.runHealthCheck", async () => {
        await providerInstance.runHealthCheck();
      })
    );

    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.rollbackLast", async () => {
        await providerInstance.rollbackLast();
      })
    );

    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.openSettings", async () => {
        await vscode.commands.executeCommand("workbench.action.openSettings", "@ext:ultra-infinity.ultra-vscode");
      })
    );

    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.openLastPatchDebug", async () => {
        try {
          const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
          if (!workspaceRoot) return;
          const reportPath = path.join(workspaceRoot, ".ultra", "debug", "last_invalid_patch.md");
          const uri = vscode.Uri.file(reportPath);
          await vscode.workspace.fs.stat(uri);
          const doc = await vscode.workspace.openTextDocument(uri);
          await vscode.window.showTextDocument(doc);
        } catch (e) {
          vscode.window.showErrorMessage("No invalid patch debug report found in .ultra/debug/last_invalid_patch.md.");
        }
      })
    );

    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.task.fixBug", async () => providerInstance.promptAndRun("fixBug"))
    );
    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.task.explain", async () => providerInstance.promptAndRun("explain"))
    );
    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.task.refactor", async () => providerInstance.promptAndRun("refactor"))
    );
    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.task.addTests", async () => providerInstance.promptAndRun("addTests"))
    );
    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.task.optimize", async () => providerInstance.promptAndRun("optimize"))
    );
    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.task.architecture", async () => providerInstance.promptAndRun("architecture"))
    );
    context.subscriptions.push(
      vscode.commands.registerCommand("ultra.task.heavy", async () => providerInstance.promptAndRun("heavy"))
    );
    output.appendLine("[lifecycle] activate completed");
    console.log("[ULTRA] activate completed");
  } catch (error) {
    const details = error && error.stack ? error.stack : String(error);
    output.appendLine(`[lifecycle] activate failed: ${details}`);
    console.error("[ULTRA] activate failed", error);
    throw error;
  }
}

function deactivate() {}

module.exports = {
  activate,
  deactivate
};
