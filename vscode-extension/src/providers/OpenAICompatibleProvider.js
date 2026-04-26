"use strict";

const { OpenAIProvider } = require("./OpenAIProvider");

/**
 * Masks an API key for safe logging.
 * "sk-or-v1-abc123xyz789" → "sk-or-****789"
 */
function maskKey(key) {
  if (!key || typeof key !== "string") return "(no key)";
  const last4 = key.slice(-4);
  const prefix = key.startsWith("sk-or") ? "sk-or" : key.slice(0, 5);
  return `${prefix}-****${last4}`;
}

class OpenAICompatibleProvider extends OpenAIProvider {
  constructor(config, outputChannel) {
    super(config, outputChannel);

    // Apply default base_url for known provider types
    if (!this.config.base_url) {
      if (this.config.type === "lmstudio") {
        this.config.base_url = "http://localhost:1234/v1";
      } else if (this.config.type === "openrouter" || this.config.type === "openai_compatible") {
        // openai_compatible requires explicit base_url in config
        if (!this.config.base_url) {
          throw new Error(`base_url is required for provider type ${this.config.type}`);
        }
      } else if (this.config.type === "groq") {
        this.config.base_url = "https://api.groq.com/openai/v1";
      } else if (this.config.type === "together") {
        this.config.base_url = "https://api.together.xyz/v1";
      } else if (this.config.type === "mistral") {
        this.config.base_url = "https://api.mistral.ai/v1";
      } else if (this.config.type === "azure_openai") {
        throw new Error(`base_url is required for azure_openai provider`);
      } else {
        throw new Error(`base_url is required for provider type ${this.config.type}`);
      }
    }
  }

  /**
   * Returns true if this provider is configured as OpenRouter
   * (detected by base_url containing openrouter.ai).
   */
  _isOpenRouter() {
    return String(this.config.base_url || "").includes("openrouter.ai");
  }

  /**
   * Enforces free_only mode: if enabled, all model IDs must end with ":free".
   */
  _assertFreeModel(modelName) {
    if (this.config.free_only && !String(modelName).endsWith(":free")) {
      throw new Error(
        `free_only=true but requested model '${modelName}' is not a free model (must end with ':free').`
      );
    }
  }

  /**
   * Builds extra headers for OpenRouter requests.
   */
  _openRouterHeaders() {
    return {
      "HTTP-Referer": "https://github.com/ultra-infinity/ultra",
      "X-Title": "ULTRA VSCode Extension"
    };
  }

  async generate(modelName, messages, options) {
    this._assertFreeModel(modelName);

    const baseUrl = (this.config.base_url || "").replace(/\/$/, "");
    const key = this.config.api_key || "";

    if (!key) {
      throw new Error(`openai_compatible provider requires 'api_key' in models.json (base_url: ${baseUrl}).`);
    }

    this.log(`[openai_compatible] key=${maskKey(key)} model=${modelName} base_url=${baseUrl}`);

    const body = {
      model: modelName,
      messages,
      temperature: options.temperature || 0.2,
      max_tokens: options.maxTokens || 2048
    };

    const extraHeaders = this._isOpenRouter() ? this._openRouterHeaders() : {};
    const timeout = this.config.timeout || 180000;

    const payload = await this.postJson(
      `${baseUrl}/chat/completions`,
      Object.assign({ authorization: `Bearer ${key}` }, extraHeaders),
      body,
      timeout
    );

    const choice = payload && Array.isArray(payload.choices) ? payload.choices[0] : null;
    if (!choice || !choice.message) return "";
    if (typeof choice.message.content === "string") return choice.message.content;
    if (Array.isArray(choice.message.content)) {
      return choice.message.content.map((item) => item.text || "").join("\n");
    }
    return "";
  }

  async checkHealth() {
    const baseUrl = (this.config.base_url || "").replace(/\/$/, "");
    const key = this.config.api_key || "";
    const maskedKey = maskKey(key);
    const providerLabel = this._isOpenRouter() ? "openrouter" : "openai_compatible";
    const results = [];

    // 1. Reachability check
    try {
      const implFetch = typeof fetch === "function" ? fetch : require("node-fetch");
      const res = await implFetch(`${baseUrl}/models`, {
        headers: Object.assign(
          { authorization: `Bearer ${key}` },
          this._isOpenRouter() ? this._openRouterHeaders() : {}
        )
      });

      if (res.status === 401 || res.status === 403) {
        results.push({
          provider: providerLabel,
          ok: false,
          message: `Auth invalid: ${res.status} (key: ${maskedKey})`
        });
        return results[0]; // Auth is broken, no point going further
      }

      results.push({
        provider: providerLabel,
        ok: true,
        message: `OK ${this._isOpenRouter() ? "OpenRouter" : "API"} reachable (key: ${maskedKey})`
      });

      // 2. Free-model enforcement check
      if (this.config.free_only) {
        const models = await res.json().catch(() => null);
        const freeModels = models && Array.isArray(models.data)
          ? models.data.filter((m) => String(m.id || "").endsWith(":free"))
          : [];

        if (freeModels.length > 0) {
          results.push({
            provider: providerLabel,
            ok: true,
            message: `OK free models available (${freeModels.length} found, e.g. ${freeModels[0].id})`
          });
        } else {
          results.push({
            provider: providerLabel,
            ok: true, // Not a fatal error — could be a non-free provider
            message: `free_only=true but no :free models found in catalog`
          });
        }
      }
    } catch (e) {
      results.push({
        provider: providerLabel,
        ok: false,
        message: `Unreachable: ${e.message}`
      });
    }

    // Return first result as the canonical health object (router calls checkHealth() expecting one object)
    return results[0];
  }

  /**
   * Returns all detailed health check results (reachability + auth + free-model check + route table).
   * Used by RouterProvider.checkHealth() for the enhanced health panel.
   */
  async checkHealthDetailed(routeTable) {
    const baseUrl = (this.config.base_url || "").replace(/\/$/, "");
    const key = this.config.api_key || "";
    const maskedKey = maskKey(key);
    const providerLabel = this._isOpenRouter() ? "openrouter" : "openai_compatible";
    const results = [];

    try {
      const implFetch = typeof fetch === "function" ? fetch : require("node-fetch");
      const res = await implFetch(`${baseUrl}/models`, {
        headers: Object.assign(
          { authorization: `Bearer ${key}` },
          this._isOpenRouter() ? this._openRouterHeaders() : {}
        )
      });

      // Reachability
      results.push({
        provider: providerLabel,
        ok: res.ok || res.status === 401 || res.status === 403,
        message: `${this._isOpenRouter() ? "OpenRouter" : "API"} reachable`
      });

      // Auth
      if (res.status === 401 || res.status === 403) {
        results.push({
          provider: providerLabel,
          ok: false,
          message: `Auth invalid: HTTP ${res.status} (key: ${maskedKey})`
        });
      } else {
        results.push({
          provider: providerLabel,
          ok: true,
          message: `Auth valid (key: ${maskedKey})`
        });

        // Free model catalog check
        if (this.config.free_only) {
          try {
            const catalog = await res.clone().json();
            const freeModels = Array.isArray(catalog.data)
              ? catalog.data.filter((m) => String(m.id || "").endsWith(":free"))
              : [];
            results.push({
              provider: providerLabel,
              ok: freeModels.length > 0,
              message: freeModels.length > 0
                ? `${freeModels.length} free model(s) available`
                : `free_only=true but no :free models in catalog`
            });
          } catch (_) {
            results.push({ provider: providerLabel, ok: true, message: "free model check skipped (could not parse catalog)" });
          }
        }
      }
    } catch (e) {
      results.push({
        provider: providerLabel,
        ok: false,
        message: `Unreachable: ${e.message}`
      });
    }

    // Route table summary
    if (routeTable && Object.keys(routeTable).length > 0) {
      const lines = Object.entries(routeTable)
        .map(([mode, dest]) => `${mode} → ${dest}`)
        .join(", ");
      results.push({
        provider: providerLabel,
        ok: true,
        message: `Active routes: ${lines}`
      });
    }

    return results;
  }
}

module.exports = { OpenAICompatibleProvider, maskKey };
