"use strict";

const { BaseProvider } = require("./BaseProvider");

class AnthropicProvider extends BaseProvider {
  async generate(modelName, messages, options) {
    if (!this.config.api_key) {
      throw new Error("Anthropic provider requires 'api_key' in models.json.");
    }

    const systemMessage = messages.find((m) => m.role === "system");
    const userMessages = messages.filter((m) => m.role !== "system");
    const body = {
      model: modelName,
      max_tokens: options.maxTokens || 2048,
      temperature: options.temperature || 0.2,
      system: systemMessage ? systemMessage.content : "",
      messages: userMessages.map((message) => ({
        role: message.role === "assistant" ? "assistant" : "user",
        content: message.content
      }))
    };

    const timeout = this.config.timeout || 180000;
    const payload = await this.postJson(
      "https://api.anthropic.com/v1/messages",
      {
        "x-api-key": this.config.api_key,
        "anthropic-version": "2023-06-01"
      },
      body,
      timeout
    );

    if (!payload || !Array.isArray(payload.content)) {
      return "";
    }
    return payload.content
      .map((block) => (block && typeof block.text === "string" ? block.text : ""))
      .join("\n");
  }

  async checkHealth() {
    try {
      const implFetch = typeof fetch === "function" ? fetch : require("node-fetch");
      const res = await implFetch("https://api.anthropic.com/v1/messages", {
        method: "POST",
        headers: {
          "x-api-key": this.config.api_key,
          "anthropic-version": "2023-06-01",
          "content-type": "application/json"
        },
        body: JSON.stringify({
          model: "claude-3-haiku-20240307",
          max_tokens: 1,
          messages: [{role: "user", content: "hi"}]
        })
      });
      // 400 means auth is valid but request is bad, which is fine for health. 401/403 means auth invalid
      if (res.status === 401 || res.status === 403) {
         return { provider: "anthropic", ok: false, message: `Auth invalid: ${res.status}` };
      }
      return { provider: "anthropic", ok: true, message: "OK Anthropic auth valid" };
    } catch (e) {
      return { provider: "anthropic", ok: false, message: e.message };
    }
  }
}

module.exports = { AnthropicProvider };
