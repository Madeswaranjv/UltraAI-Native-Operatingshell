"use strict";

const { safeLog } = require("../logging");

function requireFetch() {
  if (typeof fetch !== "function") {
    throw new Error("Global fetch is unavailable in this VS Code runtime.");
  }
  return fetch;
}

function parseJsonSafe(text) {
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

class BaseProvider {
  constructor(providerConfig, outputChannel) {
    this.config = providerConfig;
    this.output = outputChannel;
  }

  log(message) {
    safeLog(this.output, `[provider] ${message}`, "[provider]");
  }

  async postJson(url, headers, body, timeoutMs) {
    const impl = requireFetch();
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs || 180000);
    try {
      const response = await impl(url, {
        method: "POST",
        headers: Object.assign({ "content-type": "application/json" }, headers || {}),
        body: JSON.stringify(body),
        signal: controller.signal
      });
      const text = await response.text();
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}: ${text}`);
      }
      const parsed = parseJsonSafe(text);
      if (!parsed) {
        throw new Error("Provider returned non-JSON response.");
      }
      return parsed;
    } finally {
      clearTimeout(timer);
    }
  }

  async generate(modelName, messages, generationOptions) {
    throw new Error("generate() must be implemented by subclasses");
  }
  
  async checkHealth() {
    throw new Error("checkHealth() must be implemented by subclasses");
  }
}

module.exports = { BaseProvider, parseJsonSafe };
