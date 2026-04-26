"use strict";

const fs = require("node:fs/promises");
const path = require("node:path");
const crypto = require("node:crypto");

function makeSnapshotId() {
  return "snap_" + Date.now().toString(36) + "_" + crypto.randomBytes(4).toString("hex");
}

function isInside(rootPath, candidate) {
  const rootResolved = path.resolve(rootPath);
  const candidateResolved = path.resolve(candidate);
  const rootWithSep = rootResolved.endsWith(path.sep) ? rootResolved : rootResolved + path.sep;
  return candidateResolved === rootResolved || candidateResolved.startsWith(rootWithSep);
}

class RollbackStore {
  constructor(workspaceRoot, storageDir) {
    this._workspaceRoot = workspaceRoot;
    this._storageDir = storageDir;
    this._snapshotsDir = path.join(storageDir, "snapshots");
    this._latestFile = path.join(storageDir, "latest_snapshot.json");
  }

  async init() {
    await fs.mkdir(this._snapshotsDir, { recursive: true });
  }

  async saveSnapshot(entries, metadata) {
    if (!Array.isArray(entries) || entries.length === 0) {
      return null;
    }

    const filtered = entries.filter((entry) => {
      return entry && typeof entry.path === "string" && Object.prototype.hasOwnProperty.call(entry, "content");
    });
    if (filtered.length === 0) {
      return null;
    }

    const snapshot = {
      id: makeSnapshotId(),
      created_at: new Date().toISOString(),
      workspace: this._workspaceRoot,
      metadata: metadata || {},
      files: filtered
    };

    const snapshotPath = path.join(this._snapshotsDir, `${snapshot.id}.json`);
    await fs.writeFile(snapshotPath, JSON.stringify(snapshot, null, 2), "utf8");
    await fs.writeFile(this._latestFile, JSON.stringify({ id: snapshot.id }, null, 2), "utf8");
    return snapshot.id;
  }

  async _loadLatestSnapshotId() {
    try {
      const raw = await fs.readFile(this._latestFile, "utf8");
      const parsed = JSON.parse(raw);
      return parsed && typeof parsed.id === "string" ? parsed.id : null;
    } catch {
      return null;
    }
  }

  async _loadSnapshotById(snapshotId) {
    if (!snapshotId) {
      return null;
    }
    const snapshotPath = path.join(this._snapshotsDir, `${snapshotId}.json`);
    try {
      const raw = await fs.readFile(snapshotPath, "utf8");
      const parsed = JSON.parse(raw);
      if (!parsed || !Array.isArray(parsed.files)) {
        return null;
      }
      return parsed;
    } catch {
      return null;
    }
  }

  async hasSnapshot() {
    const latestId = await this._loadLatestSnapshotId();
    return Boolean(latestId);
  }

  async restoreLatest() {
    const latestId = await this._loadLatestSnapshotId();
    if (!latestId) {
      return {
        restored: false,
        message: "No rollback snapshot available."
      };
    }
    return this.restoreById(latestId);
  }

  async restoreById(snapshotId) {
    const snapshot = await this._loadSnapshotById(snapshotId);
    if (!snapshot) {
      return {
        restored: false,
        message: `Snapshot not found: ${snapshotId}`
      };
    }

    for (const file of snapshot.files) {
      const absolutePath = path.resolve(this._workspaceRoot, file.path);
      if (!isInside(this._workspaceRoot, absolutePath)) {
        throw new Error(`Refusing to restore path outside workspace: ${file.path}`);
      }

      if (file.content === null) {
        await fs.rm(absolutePath, { force: true });
        continue;
      }

      await fs.mkdir(path.dirname(absolutePath), { recursive: true });
      await fs.writeFile(absolutePath, String(file.content), "utf8");
    }

    return {
      restored: true,
      message: `Restored ${snapshot.files.length} file(s) from ${snapshotId}.`
    };
  }
}

module.exports = {
  RollbackStore
};
