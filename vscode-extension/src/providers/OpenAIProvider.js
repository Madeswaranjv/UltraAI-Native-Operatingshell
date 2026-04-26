"use strict";

const { BaseProvider } = require("./BaseProvider");

class OpenAIProvider extends BaseProvider {
  async generate(modelName, messages, options) {
    if (!this.config.api_key) {
      throw new Error("OpenAI provider requires 'api_key' in models.json.");
    }
    const baseUrl = (this.config.base_url || "https://api.openai.com/v1").replace(/\/$/, "");
    const body = {
      model: modelName,
      messages,
      temperature: options.temperature || 0.2,
      max_tokens: options.maxTokens || 2048
    };
    
    const timeout = this.config.timeout || 180000;
    const payload = await this.postJson(
      `${baseUrl}/chat/completions`,
      { authorization: `Bearer ${this.config.api_key}` },
      body,
      timeout
    );
    
    const choice = payload && Array.isArray(payload.choices) ? payload.choices[0] : null;
    if (!choice || !choice.message) {
      return "";
    }
    if (typeof choice.message.content === "string") {
      return choice.message.content;
    }
    if (Array.isArray(choice.message.content)) {
      return choice.message.content.map((item) => item.text || "").join("\n");
    }
    return "";
  }

  async checkHealth() {
    try {
      const baseUrl = (this.config.base_url || "https://api.openai.com/v1").replace(/\/$/, "");
      const impl = require("node:http").request; // Actually we can use fetch, but we need a simple GET
      // For health, let's just make a very cheap request to models list
      const implFetch = typeof fetch === "function" ? fetch : require("node-fetch");
      const res = await implFetch(`${baseUrl}/models`, {
        headers: { authorization: `Bearer ${this.config.api_key}` }
      });
      if (res.ok) {
        return { provider: "openai", ok: true, message: "OK OpenAI reachable" };
      }
      return { provider: "openai", ok: false, message: `Auth failed or unreachable: ${res.status}` };
    } catch (e) {
      return { provider: "openai", ok: false, message: e.message };
    }
  }
}

module.exports = { OpenAIProvider };
