"use strict";

function nonce() {
  const chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  let value = "";
  for (let i = 0; i < 24; i += 1) {
    value += chars.charAt(Math.floor(Math.random() * chars.length));
  }
  return value;
}

function getWebviewHtml(webview) {
  const scriptNonce = nonce();

  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src ${webview.cspSource} 'unsafe-inline'; script-src 'nonce-${scriptNonce}';">
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>ULTRA</title>
  <style>
    :root {
      --bg: var(--vscode-sideBar-background);
      --fg: var(--vscode-sideBar-foreground);
      --muted: var(--vscode-descriptionForeground);
      --border: var(--vscode-panel-border);
      --btn: var(--vscode-button-background);
      --btn-fg: var(--vscode-button-foreground);
      --btn-secondary: var(--vscode-button-secondaryBackground);
      --btn-secondary-fg: var(--vscode-button-secondaryForeground);
      --input-bg: var(--vscode-input-background);
      --input-fg: var(--vscode-input-foreground);
      --input-border: var(--vscode-input-border);
      --add: #0f5132;
      --del: #58151c;
      --meta: #1f2937;
      --hunk: #0c4a6e;
      --ctx: #111827;
    }

    body {
      margin: 0;
      padding: 10px;
      font-family: var(--vscode-font-family);
      color: var(--fg);
      background: var(--bg);
    }

    .block {
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 10px;
      margin-bottom: 10px;
      background: color-mix(in srgb, var(--bg) 92%, #000 8%);
    }

    .title {
      font-size: 13px;
      font-weight: 600;
      margin-bottom: 8px;
      letter-spacing: 0.2px;
    }

    .row {
      display: flex;
      gap: 6px;
      flex-wrap: wrap;
      margin-bottom: 8px;
    }

    button {
      border: none;
      border-radius: 6px;
      padding: 6px 10px;
      cursor: pointer;
      background: var(--btn);
      color: var(--btn-fg);
      font-size: 12px;
    }

    button.secondary {
      background: var(--btn-secondary);
      color: var(--btn-secondary-fg);
    }

    button.mode.active {
      outline: 2px solid color-mix(in srgb, var(--btn) 55%, #fff 45%);
    }

    textarea {
      width: 100%;
      min-height: 86px;
      resize: vertical;
      background: var(--input-bg);
      color: var(--input-fg);
      border: 1px solid var(--input-border);
      border-radius: 6px;
      padding: 8px;
      font-family: var(--vscode-editor-font-family, Consolas, monospace);
      font-size: 12px;
      box-sizing: border-box;
    }

    .status-list {
      font-size: 12px;
      line-height: 1.5;
      max-height: 180px;
      overflow: auto;
      white-space: pre-wrap;
      color: var(--muted);
    }

    .result {
      font-size: 12px;
      line-height: 1.5;
      white-space: pre-wrap;
    }

    .files-used {
      font-size: 12px;
      color: var(--muted);
      margin-top: 8px;
    }

    .diff-file {
      border: 1px solid var(--border);
      border-radius: 6px;
      margin-bottom: 8px;
      overflow: hidden;
    }

    .diff-header {
      padding: 6px 8px;
      background: color-mix(in srgb, var(--bg) 84%, #000 16%);
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 6px;
      font-size: 12px;
    }

    .diff-header label {
      display: flex;
      align-items: center;
      gap: 6px;
      cursor: pointer;
      font-weight: 500;
    }

    .diff-block {
      margin: 0;
      padding: 8px;
      overflow: auto;
      font-size: 11px;
      line-height: 1.35;
      font-family: var(--vscode-editor-font-family, Consolas, monospace);
      border-top: 1px solid var(--border);
    }

    .line {
      display: block;
      white-space: pre;
      padding: 0 4px;
    }

    .line.meta { background: var(--meta); }
    .line.hunk { background: var(--hunk); }
    .line.add { background: var(--add); }
    .line.del { background: var(--del); }
    .line.ctx { background: var(--ctx); }

    .muted {
      color: var(--muted);
      font-size: 11px;
    }
  </style>
</head>
<body>
  <div class="block">
    <div class="title">Ask ULTRA</div>
    <div class="row">
      <button class="mode active" data-mode="fixBug">Fix Bug</button>
      <button class="mode secondary" data-mode="explain">Explain</button>
      <button class="mode secondary" data-mode="refactor">Refactor</button>
      <button class="mode secondary" data-mode="addTests">Add Tests</button>
      <button class="mode secondary" data-mode="optimize">Optimize</button>
      <button class="mode secondary" data-mode="architecture">Architecture</button>
      <button class="mode secondary" data-mode="heavy">Heavy Reasoning</button>
    </div>
    <textarea id="taskInput" placeholder="Describe what ULTRA should do..."></textarea>
    <div class="row" style="margin-top:8px;">
      <button id="runTask">Run Task</button>
      <button id="healthCheck" class="secondary">Health Check</button>
      <button id="rollback" class="secondary">Rollback</button>
      <button id="settings" class="secondary">Settings</button>
    </div>
    <div class="muted">Context: selected text, current file, current symbol, workspace.</div>
  </div>

  <div class="block" id="modelsControlBlock" style="display:none;">
    <div class="title" style="display:flex; justify-content:space-between; align-items:center;">
      <span>ULTRA Model Control</span>
      <span id="configDefaultProvider" class="muted" style="font-size:10px;"></span>
    </div>
    <div id="providersDetected" class="muted" style="font-size:11px; margin-bottom: 8px;"></div>
    <div id="modeDropdowns" style="font-size:11px; display:flex; flex-direction:column; gap:6px;"></div>
  </div>

  <div class="block">
    <div class="title">Progress</div>
    <div id="statusList" class="status-list"></div>
  </div>

  <div class="block">
    <div class="title">Result</div>
    <div id="resultText" class="result"></div>
    <div id="filesUsed" class="files-used"></div>
  </div>

  <div class="block" id="reviewBlock" style="display:none;">
    <div class="title">Patch Review</div>
    <div class="row">
      <button id="selectAll" class="secondary">Select All</button>
      <button id="selectNone" class="secondary">Select None</button>
      <button id="applySelected">Apply Selected</button>
    </div>
    <div id="diffList"></div>
  </div>

  <script nonce="${scriptNonce}">
    const vscode = acquireVsCodeApi();
    let currentMode = "fixBug";
    let currentRequestId = null;
    let currentReviewFiles = [];

    const statusList = document.getElementById("statusList");
    const resultText = document.getElementById("resultText");
    const filesUsed = document.getElementById("filesUsed");
    const reviewBlock = document.getElementById("reviewBlock");
    const diffList = document.getElementById("diffList");
    const taskInput = document.getElementById("taskInput");

    function escapeHtml(value) {
      return String(value || "")
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#39;");
    }

    function setMode(mode) {
      currentMode = mode;
      document.querySelectorAll("button.mode").forEach((button) => {
        const active = button.getAttribute("data-mode") === mode;
        button.classList.toggle("active", active);
        button.classList.toggle("secondary", !active);
      });
    }

    function appendStatus(line) {
      const entry = document.createElement("div");
      entry.textContent = line;
      statusList.appendChild(entry);
      statusList.scrollTop = statusList.scrollHeight;
    }

    function clearResult() {
      resultText.textContent = "";
      filesUsed.textContent = "";
      reviewBlock.style.display = "none";
      diffList.innerHTML = "";
      currentReviewFiles = [];
      currentRequestId = null;
    }

    function renderFilesUsed(list) {
      if (!Array.isArray(list) || list.length === 0) {
        filesUsed.textContent = "";
        return;
      }
      filesUsed.innerHTML = "Files used: " + list.map((item) => escapeHtml(item)).join(", ");
    }

    function renderReview(payload) {
      currentRequestId = payload.request_id;
      currentReviewFiles = payload.patches || [];
      resultText.textContent = [payload.summary || "", payload.why_changed || ""].filter(Boolean).join("\\n\\n");
      renderFilesUsed(payload.files_used || []);

      reviewBlock.style.display = "block";
      diffList.innerHTML = "";

      for (const item of currentReviewFiles) {
        const wrapper = document.createElement("div");
        wrapper.className = "diff-file";
        wrapper.setAttribute("data-path", item.path);

        const header = document.createElement("div");
        header.className = "diff-header";
        header.innerHTML =
          '<label><input type="checkbox" class="accept-box" data-path="' +
          escapeHtml(item.path) +
          '" checked /> ' +
          escapeHtml(item.path) +
          "</label>" +
          '<span class="muted">+' +
          Number(item.additions || 0) +
          " / -" +
          Number(item.deletions || 0) +
          "</span>";

        const body = document.createElement("div");
        body.innerHTML = item.html || "";

        wrapper.appendChild(header);
        wrapper.appendChild(body);
        diffList.appendChild(wrapper);
      }
    }

    function runTask() {
      const prompt = taskInput.value.trim();
      if (!prompt) {
        appendStatus("Provide a task prompt first.");
        return;
      }
      clearResult();
      appendStatus("Task queued...");
      vscode.postMessage({
        type: "runTask",
        mode: currentMode,
        prompt
      });
    }

    function applySelected() {
      if (!currentRequestId) {
        appendStatus("No pending patch review.");
        return;
      }
      const acceptedPaths = Array.from(document.querySelectorAll(".accept-box"))
        .filter((box) => box.checked)
        .map((box) => box.getAttribute("data-path"));
      vscode.postMessage({
        type: "applyPatch",
        request_id: currentRequestId,
        accepted_paths: acceptedPaths
      });
    }

    document.getElementById("runTask").addEventListener("click", runTask);
    document.getElementById("applySelected").addEventListener("click", applySelected);

    document.getElementById("healthCheck").addEventListener("click", () => {
      vscode.postMessage({ type: "healthCheck" });
    });
    document.getElementById("rollback").addEventListener("click", () => {
      vscode.postMessage({ type: "rollback" });
    });
    document.getElementById("settings").addEventListener("click", () => {
      vscode.postMessage({ type: "openSettings" });
    });
    document.getElementById("selectAll").addEventListener("click", () => {
      document.querySelectorAll(".accept-box").forEach((box) => {
        box.checked = true;
      });
    });
    document.getElementById("selectNone").addEventListener("click", () => {
      document.querySelectorAll(".accept-box").forEach((box) => {
        box.checked = false;
      });
    });

    document.querySelectorAll("button.mode").forEach((button) => {
      button.addEventListener("click", () => setMode(button.getAttribute("data-mode")));
    });
    
    // Request models data on load
    vscode.postMessage({ type: "getModels" });
    
    function renderModelsPanel(data) {
      const block = document.getElementById("modelsControlBlock");
      const detected = document.getElementById("providersDetected");
      const dropdowns = document.getElementById("modeDropdowns");
      
      block.style.display = "block";
      
      detected.innerHTML = "Providers: " + data.providers.map(p => escapeHtml(p.key) + " (" + escapeHtml(p.type) + ")").join(", ");
      
      dropdowns.innerHTML = "";
      
      const modes = ["fixBug", "explain", "refactor", "addTests", "optimize", "architecture", "heavy", "verify"];
      
      modes.forEach(mode => {
        const row = document.createElement("div");
        row.style.display = "flex";
        row.style.justifyContent = "space-between";
        row.style.alignItems = "center";
        
        const label = document.createElement("span");
        label.textContent = mode + ": ";
        
        const select = document.createElement("select");
        select.style.maxWidth = "160px";
        select.style.background = "var(--input-bg)";
        select.style.color = "var(--input-fg)";
        select.style.border = "1px solid var(--input-border)";
        select.style.borderRadius = "4px";
        select.style.padding = "2px";
        
        const currentRoute = data.routes[mode] || "auto";
        
        const autoOpt = document.createElement("option");
        autoOpt.value = "auto";
        autoOpt.textContent = "Auto";
        select.appendChild(autoOpt);
        
        for (const [prov, models] of Object.entries(data.modelsByProvider)) {
          const group = document.createElement("optgroup");
          group.label = prov;
          models.forEach(m => {
            const opt = document.createElement("option");
            opt.value = m.routeStr;
            opt.textContent = m.modelName;
            group.appendChild(opt);
          });
          select.appendChild(group);
        }
        
        select.value = currentRoute;
        
        select.addEventListener("change", (e) => {
          vscode.postMessage({ type: "updateRoute", mode, routeDest: e.target.value });
        });
        
        row.appendChild(label);
        row.appendChild(select);
        dropdowns.appendChild(row);
      });
    }

    window.addEventListener("message", (event) => {
      const msg = event.data || {};
      if (msg.type === "status") {
        appendStatus(msg.message || msg.stage || "...");
        return;
      }
      if (msg.type === "taskError") {
        appendStatus("Error: " + (msg.message || "Unknown error"));
        return;
      }
      if (msg.type === "explainResult") {
        currentRequestId = msg.request_id || null;
        resultText.textContent = msg.content || "";
        renderFilesUsed(msg.files_used || []);
        appendStatus("Done.");
        return;
      }
      if (msg.type === "reviewReady") {
        appendStatus("Patch ready for review.");
        renderReview(msg);
        return;
      }
      if (msg.type === "applyResult") {
        appendStatus(msg.message || "Apply completed.");
        if (msg.validation) {
          const validation = msg.validation;
          appendStatus(
            validation.ran
              ? "Validation: " + (validation.passed ? "PASS" : "FAIL") + " (" + (validation.command || "command") + ")"
              : "Validation skipped."
          );
        }
        if (msg.clear_review) {
          reviewBlock.style.display = "none";
        }
        return;
      }
      if (msg.type === "healthResult") {
        appendStatus("Health Check:");
        for (const check of msg.checks || []) {
          // check.name format: "Provider: openrouter" or "Provider: route:fixBug"
          if (String(check.name).includes("route:")) {
            // It's a route table entry — format it cleaner
            appendStatus("  ROUTE " + (check.details || ""));
          } else {
            appendStatus((check.ok ? "  OK  " : "  FAIL ") + check.name + " - " + (check.details || ""));
          }
        }
        return;
      }
      if (msg.type === "rollbackResult") {
        appendStatus(msg.message || "Rollback complete.");
        return;
      }
      if (msg.type === "restorePending") {
        if (msg.pending) {
          renderReview(msg.pending);
          appendStatus("Restored pending patch review.");
        }
        return;
      }
      if (msg.type === "configReloaded") {
        vscode.postMessage({ type: "getModels" });
        return;
      }
      if (msg.type === "modelsData") {
        renderModelsPanel(msg);
        return;
      }
      if (msg.type === "configError") {
        appendStatus("Config Error: " + (msg.message || "Invalid models.json"));
        return;
      }
    });
  </script>
</body>
</html>`;
}

module.exports = {
  getWebviewHtml
};
