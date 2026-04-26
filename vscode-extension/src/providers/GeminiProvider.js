"use strict";

const { BaseProvider } = require("./BaseProvider");

class GeminiProvider extends BaseProvider {
  async generate(modelName, messages, options) {
    if (!this.config.api_key) {
      throw new Error("Gemini provider requires 'api_key' in models.json.");
    }
    
    // Convert generic chat messages to Gemini's format
    let systemInstruction = null;
    const contents = [];
    
    for (const msg of messages) {
      if (msg.role === "system") {
        systemInstruction = {
          role: "system",
          parts: [{ text: msg.content }]
        };
      } else {
        contents.push({
          role: msg.role === "assistant" ? "model" : "user",
          parts: [{ text: msg.content }]
        });
      }
    }

    const body = {
      contents,
      generationConfig: {
        temperature: options.temperature || 0.2,
        maxOutputTokens: options.maxTokens || 2048
      }
    };
    
    if (systemInstruction) {
      body.systemInstruction = systemInstruction;
    }

    const timeout = this.config.timeout || 180000;
    const url = `https://generativelanguage.googleapis.com/v1beta/models/${modelName}:generateContent?key=${this.config.api_key}`;
    const payload = await this.postJson(url, {}, body, timeout);

    if (payload.candidates && payload.candidates.length > 0) {
      const candidate = payload.candidates[0];
      if (candidate.content && candidate.content.parts && candidate.content.parts.length > 0) {
        return candidate.content.parts.map(p => p.text || "").join("");
      }
    }
    return "";
  }

  async checkHealth() {
    try {
      const implFetch = typeof fetch === "function" ? fetch : require("node-fetch");
      const url = `https://generativelanguage.googleapis.com/v1beta/models?key=${this.config.api_key}`;
      const res = await implFetch(url);
      if (res.ok) {
        return { provider: "gemini", ok: true, message: "OK Gemini auth valid" };
      }
      return { provider: "gemini", ok: false, message: `Auth failed: ${res.status}` };
    } catch (e) {
      return { provider: "gemini", ok: false, message: e.message };
    }
  }
}

module.exports = { GeminiProvider };
