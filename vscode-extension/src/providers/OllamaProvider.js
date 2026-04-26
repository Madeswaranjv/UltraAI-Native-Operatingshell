"use strict";

const { BaseProvider } = require("./BaseProvider");

class OllamaProvider extends BaseProvider {
  async generate(modelName, messages, options) {
    const base = String(this.config.endpoint || "http://127.0.0.1:11434").replace(/\/$/, "");
    const body = {
      model: modelName,
      stream: false,
      messages,
      options: {
        temperature: options.temperature || 0.2,
        num_predict: options.maxTokens || 2048
      }
    };
    
    const headers = {};
    if (this.config.api_key) {
      headers["authorization"] = `Bearer ${this.config.api_key}`;
    }

    const timeout = this.config.timeout || 180000;
    const payload = await this.postJson(`${base}/api/chat`, headers, body, timeout);
    
    return payload && payload.message && typeof payload.message.content === "string"
      ? payload.message.content
      : "";
  }

  async checkHealth() {
    try {
      const base = String(this.config.endpoint || "http://127.0.0.1:11434").replace(/\/$/, "");
      const implFetch = typeof fetch === "function" ? fetch : require("node-fetch");
      
      const headers = {};
      if (this.config.api_key) {
        headers["authorization"] = `Bearer ${this.config.api_key}`;
      }

      const res = await implFetch(`${base}/api/tags`, { headers });
      if (res.ok) {
        return { provider: "ollama", ok: true, message: "OK Ollama running" };
      }
      return { provider: "ollama", ok: false, message: `Failed: ${res.status}` };
    } catch (e) {
      return { provider: "ollama", ok: false, message: `Unreachable: ${e.message}` };
    }
  }
}

module.exports = { OllamaProvider };
