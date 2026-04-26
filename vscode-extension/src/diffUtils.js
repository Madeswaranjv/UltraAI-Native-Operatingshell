"use strict";

const fs = require("node:fs/promises");
const path = require("node:path");

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function normalizePathToken(rawPath) {
  if (!rawPath) {
    return null;
  }
  const token = rawPath.trim().split(/\s+/)[0];
  if (!token || token === "/dev/null") {
    return null;
  }
  return token.replace(/^a\//, "").replace(/^b\//, "").replace(/\\/g, "/");
}

function extractUnifiedDiffFromText(text) {
  if (!text) {
    return "";
  }

  const raw = String(text).replace(/\r\n/g, "\n");
  const fenced = raw.match(/```diff\s*([\s\S]*?)```/i);
  if (fenced && fenced[1]) {
    return fenced[1].trim();
  }

  const genericFence = raw.match(/```[\s\S]*?```/g);
  if (genericFence) {
    for (const block of genericFence) {
      const inner = block.replace(/^```[a-zA-Z0-9_-]*\s*/, "").replace(/```$/, "");
      if (inner.includes("diff --git") || inner.includes("\n@@")) {
        return inner.trim();
      }
    }
  }

  const diffStart = raw.indexOf("diff --git ");
  if (diffStart >= 0) {
    return raw.slice(diffStart).trim();
  }

  if (raw.includes("\n@@ ") && raw.includes("\n--- ") && raw.includes("\n+++ ")) {
    return raw.trim();
  }

  return "";
}

function parseUnifiedDiff(diffText, fallbackFilename) {
  // Normalise all line endings (including bare \r from Windows-trained models).
  const normalized = String(diffText || "").replace(/\r\n/g, "\n").replace(/\r/g, "\n").trim();
  if (!normalized) {
    return [];
  }

  const lines = normalized.split("\n");
  const patches = [];
  let patch = null;
  let currentHunk = null;

  function ensurePatch() {
    if (!patch) {
      patch = {
        oldPath: null,
        newPath: null,
        displayPath: "",
        hunks: [],
        rawLines: [],
        additions: 0,
        deletions: 0
      };
    }
  }

  function finalizePatch() {
    if (!patch) {
      return;
    }
    // When the model returns a bare hunk-only diff (no --- / +++ / diff --git
    // headers), patch.oldPath and patch.newPath are both null.  Without a real
    // path the apply engine silently skips the patch (targetRel = null).
    // Use the caller-supplied fallback (active file / files_used[0]) so that
    // the patch can actually be written to disk.
    const fallback = fallbackFilename
      ? String(fallbackFilename).replace(/\\/g, "/").trim()
      : null;
    const resolvedPath = patch.newPath || patch.oldPath || fallback || "unknown";
    patch.displayPath = resolvedPath;
    // Synthesise oldPath / newPath from the fallback so the apply engine's
    // `targetRel = patch.newPath || patch.oldPath` always yields a real value.
    if (!patch.newPath && !patch.oldPath && resolvedPath !== "unknown") {
      patch.oldPath = resolvedPath;
      patch.newPath = resolvedPath;
    }
    patch.raw = patch.rawLines.join("\n");
    delete patch.rawLines;
    patches.push(patch);
    patch = null;
    currentHunk = null;
  }

  for (const line of lines) {
    if (line.startsWith("diff --git ")) {
      finalizePatch();
      ensurePatch();
      patch.rawLines.push(line);
      const match = line.match(/^diff --git a\/(.+?) b\/(.+)$/);
      if (match) {
        patch.oldPath = normalizePathToken(`a/${match[1]}`);
        patch.newPath = normalizePathToken(`b/${match[2]}`);
      }
      continue;
    }

    if (line.startsWith("--- ")) {
      ensurePatch();
      patch.rawLines.push(line);
      patch.oldPath = normalizePathToken(line.slice(4));
      continue;
    }

    if (line.startsWith("+++ ")) {
      ensurePatch();
      patch.rawLines.push(line);
      patch.newPath = normalizePathToken(line.slice(4));
      continue;
    }

    if (line.startsWith("@@ ")) {
      ensurePatch();
      patch.rawLines.push(line);
      const match = line.match(/^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@/);
      if (!match) {
        continue;
      }
      currentHunk = {
        oldStart: Number(match[1]),
        oldCount: Number(match[2] || "1"),
        newStart: Number(match[3]),
        newCount: Number(match[4] || "1"),
        lines: []
      };
      patch.hunks.push(currentHunk);
      continue;
    }

    if (!patch) {
      continue;
    }

    patch.rawLines.push(line);

    if (currentHunk && (line.startsWith(" ") || line.startsWith("+") || line.startsWith("-") || line.startsWith("\\"))) {
      // Strip any stray \r a Windows-trained model may embed in hunk lines.
      // Without this, applyPatchToText's strict equality check fails on CRLF files.
      const cleanLine = line.replace(/\r$/, "");
      currentHunk.lines.push(cleanLine);
      if (cleanLine.startsWith("+") && !cleanLine.startsWith("+++")) {
        patch.additions += 1;
      } else if (cleanLine.startsWith("-") && !cleanLine.startsWith("---")) {
        patch.deletions += 1;
      }
    }
  }

  finalizePatch();
  return patches;
}

function renderPatchHtml(patch) {
  const source = patch && patch.raw ? patch.raw : "";
  const lines = source.split("\n");
  const html = lines
    .map((line) => {
      let cls = "ctx";
      if (line.startsWith("+") && !line.startsWith("+++")) {
        cls = "add";
      } else if (line.startsWith("-") && !line.startsWith("---")) {
        cls = "del";
      } else if (line.startsWith("@@")) {
        cls = "hunk";
      } else if (
        line.startsWith("diff --git") ||
        line.startsWith("--- ") ||
        line.startsWith("+++ ") ||
        line.startsWith("index ")
      ) {
        cls = "meta";
      }
      return `<span class="line ${cls}">${escapeHtml(line)}</span>`;
    })
    .join("\n");
  return `<pre class="diff-block">${html}</pre>`;
}

function splitTextLines(value) {
  const normalized = String(value || "").replace(/\r\n/g, "\n");
  const hadTrailingNewline = normalized.endsWith("\n");
  const lines = normalized.split("\n");
  if (hadTrailingNewline) {
    lines.pop();
  }
  return { lines, hadTrailingNewline };
}

function applyPatchToText(originalText, patch) {
  const { lines: originalLines, hadTrailingNewline } = splitTextLines(originalText);
  let cursor = 0;
  const output = [];

  function normalizeWhitespace(str) {
    return str.trim().replace(/\s+/g, " ");
  }

  function linesMatch(line1, line2) {
    if (line1 === line2) return true;
    return normalizeWhitespace(line1) === normalizeWhitespace(line2);
  }

  for (const hunk of patch.hunks) {
    let targetIndex = Math.max(0, hunk.oldStart - 1);
    
    let bestOffset = null;
    const maxOffset = 100;
    
    const expectedOldLines = hunk.lines.filter(l => l[0] === " " || l[0] === "-").map(l => l.slice(1));
    
    for (let offset = 0; offset <= maxOffset; offset++) {
      for (const sign of [1, -1]) {
        const testOffset = offset * sign;
        const testIndex = targetIndex + testOffset;
        
        if (testIndex < cursor || testIndex + expectedOldLines.length > originalLines.length) {
          continue;
        }
        
        let match = true;
        for (let i = 0; i < expectedOldLines.length; i++) {
          if (!linesMatch(originalLines[testIndex + i], expectedOldLines[i])) {
            match = false;
            break;
          }
        }
        
        if (match) {
          bestOffset = testOffset;
          break;
        }
      }
      if (bestOffset !== null) break;
    }
    
    if (bestOffset === null) {
      throw new Error(
        `Patch context mismatch in ${patch.displayPath} at source line ${targetIndex + 1}.`
      );
    }
    
    targetIndex = targetIndex + bestOffset;

    while (cursor < targetIndex && cursor < originalLines.length) {
      output.push(originalLines[cursor]);
      cursor += 1;
    }

    for (const hunkLine of hunk.lines) {
      const kind = hunkLine[0];
      const text = hunkLine.slice(1);

      if (kind === " ") {
        output.push(originalLines[cursor]);
        cursor += 1;
        continue;
      }

      if (kind === "-") {
        cursor += 1;
        continue;
      }

      if (kind === "+") {
        output.push(text);
        continue;
      }
    }
  }

  while (cursor < originalLines.length) {
    output.push(originalLines[cursor]);
    cursor += 1;
  }

  let result = output.join("\n");
  if (hadTrailingNewline || result.length > 0) {
    result += "\n";
  }
  return result;
}

function isInsideWorkspace(workspaceRoot, candidatePath) {
  const root = path.resolve(workspaceRoot);
  const target = path.resolve(candidatePath);
  if (process.platform === "win32") {
    const rootLower = root.toLowerCase();
    const targetLower = target.toLowerCase();
    return targetLower === rootLower || targetLower.startsWith(rootLower + "\\");
  }
  return target === root || target.startsWith(root + path.sep);
}

function resolveWorkspacePath(workspaceRoot, relativePath) {
  const absolute = path.resolve(workspaceRoot, relativePath);
  if (!isInsideWorkspace(workspaceRoot, absolute)) {
    throw new Error(`Refusing to access path outside workspace: ${relativePath}`);
  }
  return absolute;
}

async function readUtf8IfExists(filePath) {
  try {
    return await fs.readFile(filePath, "utf8");
  } catch (error) {
    if (error && error.code === "ENOENT") {
      return null;
    }
    throw error;
  }
}

async function applyPatchesToWorkspace(workspaceRoot, patches, acceptedPaths) {
  const accepted = new Set((acceptedPaths || []).map((v) => String(v)));
  const selected = patches.filter((patch) => accepted.has(patch.displayPath));
  const appliedPaths = [];

  for (const patch of selected) {
    const targetRel = patch.newPath || patch.oldPath;
    if (!targetRel) {
      // Do NOT silently skip — surface a clear error so the user knows why
      // apply failed rather than seeing the opaque "No files were applied".
      throw new Error(
        `Cannot apply patch "${patch.displayPath}": target filename is missing. ` +
        `The diff lacks --- a/file and +++ b/file headers. ` +
        `Ensure the model returns a full unified diff with file paths.`
      );
    }
    const targetAbs = resolveWorkspacePath(workspaceRoot, targetRel);
    const sourceRel = patch.oldPath || targetRel;
    const sourceAbs = resolveWorkspacePath(workspaceRoot, sourceRel);
    const originalText = await readUtf8IfExists(sourceAbs);

    if (patch.newPath === null) {
      await fs.rm(targetAbs, { force: true });
      appliedPaths.push(targetRel);
      continue;
    }

    const baseText = patch.oldPath === null ? "" : originalText || "";
    const nextText = applyPatchToText(baseText, patch);
    await fs.mkdir(path.dirname(targetAbs), { recursive: true });
    await fs.writeFile(targetAbs, nextText, "utf8");
    if (sourceRel !== targetRel) {
      await fs.rm(sourceAbs, { force: true });
    }
    appliedPaths.push(targetRel);
  }

  return {
    selectedCount: selected.length,
    appliedPaths
  };
}

async function collectSnapshotEntries(workspaceRoot, patches, acceptedPaths) {
  const accepted = new Set((acceptedPaths || []).map((v) => String(v)));
  const selected = patches.filter((patch) => accepted.has(patch.displayPath));
  const entries = [];
  const seen = new Set();

  async function addEntry(relativePath) {
    if (!relativePath || seen.has(relativePath)) {
      return;
    }
    const absolute = resolveWorkspacePath(workspaceRoot, relativePath);
    const originalText = await readUtf8IfExists(absolute);
    entries.push({
      path: relativePath,
      content: originalText
    });
    seen.add(relativePath);
  }

  for (const patch of selected) {
    await addEntry(patch.oldPath || patch.newPath);
    const targetRel = patch.newPath || patch.oldPath;
    await addEntry(targetRel);
  }

  return {
    selectedCount: selected.length,
    entries
  };
}

function buildPatchReview(diffText, fallbackFilename) {
  const patches = parseUnifiedDiff(diffText, fallbackFilename);
  return patches.map((patch) => ({
    path: patch.displayPath,
    additions: patch.additions,
    deletions: patch.deletions,
    html: renderPatchHtml(patch)
  }));
}

module.exports = {
  applyPatchesToWorkspace,
  buildPatchReview,
  collectSnapshotEntries,
  escapeHtml,
  extractUnifiedDiffFromText,
  parseUnifiedDiff
};
