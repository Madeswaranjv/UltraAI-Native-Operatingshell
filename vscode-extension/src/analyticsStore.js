"use strict";

const fs = require("node:fs/promises");
const path = require("node:path");

function createDefaultAnalytics() {
  return {
    version: 1,
    updated_at: new Date().toISOString(),
    tasks_started: 0,
    tasks_completed: 0,
    tasks_failed: 0,
    patch_accepted_count: 0,
    rollback_count: 0,
    total_latency_ms: 0,
    total_retries: 0,
    top_commands: {}
  };
}

class AnalyticsStore {
  constructor(storageDir, enabled) {
    this._storageDir = storageDir;
    this._enabled = Boolean(enabled);
    this._filePath = path.join(storageDir, "analytics.json");
    this._state = createDefaultAnalytics();
  }

  async init() {
    if (!this._enabled) {
      return;
    }
    await fs.mkdir(this._storageDir, { recursive: true });
    try {
      const raw = await fs.readFile(this._filePath, "utf8");
      const parsed = JSON.parse(raw);
      this._state = Object.assign(createDefaultAnalytics(), parsed || {});
    } catch {
      await this._flush();
    }
  }

  async _flush() {
    if (!this._enabled) {
      return;
    }
    this._state.updated_at = new Date().toISOString();
    await fs.writeFile(this._filePath, JSON.stringify(this._state, null, 2), "utf8");
  }

  async recordTaskStart(mode) {
    if (!this._enabled) {
      return;
    }
    this._state.tasks_started += 1;
    if (mode) {
      this._state.top_commands[mode] = (this._state.top_commands[mode] || 0) + 1;
    }
    await this._flush();
  }

  async recordTaskComplete(details) {
    if (!this._enabled) {
      return;
    }
    if (details && details.success) {
      this._state.tasks_completed += 1;
    } else {
      this._state.tasks_failed += 1;
    }
    if (details && Number.isFinite(details.latencyMs)) {
      this._state.total_latency_ms += Math.max(0, Math.floor(details.latencyMs));
    }
    if (details && Number.isFinite(details.retries)) {
      this._state.total_retries += Math.max(0, Math.floor(details.retries));
    }
    await this._flush();
  }

  async recordPatchAccepted() {
    if (!this._enabled) {
      return;
    }
    this._state.patch_accepted_count += 1;
    await this._flush();
  }

  async recordRollback() {
    if (!this._enabled) {
      return;
    }
    this._state.rollback_count += 1;
    await this._flush();
  }

  snapshot() {
    return Object.assign({}, this._state);
  }
}

module.exports = {
  AnalyticsStore
};
