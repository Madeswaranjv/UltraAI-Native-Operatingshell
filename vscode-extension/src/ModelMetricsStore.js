"use strict";

const fs = require("node:fs/promises");
const path = require("node:path");

class ModelMetricsStore {
  constructor(workspaceRoot) {
    this.workspaceRoot = workspaceRoot;
    this.filePath = workspaceRoot ? path.join(workspaceRoot, ".ultra", "metrics.json") : null;
    this.metrics = {}; // provider.model -> { latencyMs: avg, success: N, fail: N, err429: N, err404: N }
    this.initialized = false;
  }

  async init() {
    if (!this.filePath) return;
    try {
      const data = await fs.readFile(this.filePath, "utf8");
      this.metrics = JSON.parse(data);
    } catch {
      this.metrics = {};
    }
    this.initialized = true;
  }

  async flush() {
    if (!this.filePath || !this.initialized) return;
    try {
      await fs.mkdir(path.dirname(this.filePath), { recursive: true });
      await fs.writeFile(this.filePath, JSON.stringify(this.metrics, null, 2), "utf8");
    } catch (e) {
      console.error(`[metrics-store] flush error: ${e.message}`);
    }
  }

  _getOrCreate(provider, model) {
    const key = `${provider}.${model}`;
    if (!this.metrics[key]) {
      this.metrics[key] = {
        latencyMs: 0,
        success: 0,
        fail: 0,
        err429: 0,
        err404: 0
      };
    }
    return { key, stat: this.metrics[key] };
  }

  async recordSuccess(provider, model, latencyMs) {
    if (!this.initialized) await this.init();
    const { stat } = this._getOrCreate(provider, model);
    stat.success += 1;
    if (latencyMs > 0) {
      stat.latencyMs = stat.latencyMs === 0 ? latencyMs : Math.floor((stat.latencyMs * 0.7) + (latencyMs * 0.3));
    }
    await this.flush();
  }

  async recordFailure(provider, model, error) {
    if (!this.initialized) await this.init();
    const { stat } = this._getOrCreate(provider, model);
    stat.fail += 1;
    
    const errMsg = (error && error.message) || String(error);
    if (errMsg.includes("429")) {
      stat.err429 += 1;
    } else if (errMsg.includes("404")) {
      stat.err404 += 1;
    }
    await this.flush();
  }

  getMetrics(provider, model) {
    const key = `${provider}.${model}`;
    return this.metrics[key] || {
      latencyMs: 0,
      success: 0,
      fail: 0,
      err429: 0,
      err404: 0
    };
  }

  getAllMetrics() {
    return this.metrics;
  }
}

module.exports = { ModelMetricsStore };
