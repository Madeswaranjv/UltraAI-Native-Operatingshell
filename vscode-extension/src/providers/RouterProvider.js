"use strict";

const { OpenAIProvider } = require("./OpenAIProvider");
const { AnthropicProvider } = require("./AnthropicProvider");
const { GeminiProvider } = require("./GeminiProvider");
const { OllamaProvider } = require("./OllamaProvider");
const { OpenAICompatibleProvider } = require("./OpenAICompatibleProvider");
const { ModelMetricsStore } = require("../ModelMetricsStore");

/**
 * Factory: create a provider instance from its type string and config block.
 * openai_compatible covers: openrouter, lmstudio, groq, together, mistral, azure_openai,
 * and any custom base_url provider.
 * Legacy types (openai, anthropic, gemini) remain supported but are secondary.
 */
function createProvider(type, config, output) {
  switch (type) {
    case "openai_compatible":
    case "openrouter":
    case "lmstudio":
    case "groq":
    case "together":
    case "mistral":
    case "azure_openai":
      return new OpenAICompatibleProvider(config, output);
    case "ollama":
      return new OllamaProvider(config, output);
    case "openai":
      return new OpenAIProvider(config, output);
    case "anthropic":
      return new AnthropicProvider(config, output);
    case "gemini":
      return new GeminiProvider(config, output);
    default:
      throw new Error(`Unsupported provider type: '${type}'. Valid types: openai_compatible, ollama, openai, anthropic, gemini.`);
  }
}

class RouterProvider {
  constructor(configLoader, outputChannel) {
    this.configLoader = configLoader;
    this.output = outputChannel;
    this.metricsStore = new ModelMetricsStore(configLoader.workspaceRoot);
  }

  log(message) {
    if (this.output) {
      this.output.appendLine(`[router] ${message}`);
    }
  }

  async resolveRoute(mode) {
    const fullConfig = await this.configLoader.getConfig();
    if (!fullConfig || !fullConfig.providers) {
      throw new Error("Invalid models.json: Missing 'providers' block.");
    }

    const defaultProviderKey = fullConfig.default_provider;
    if (!defaultProviderKey || !fullConfig.providers[defaultProviderKey]) {
      throw new Error(`Default provider '${defaultProviderKey}' not found in models.json.`);
    }

    const defaultProvider = fullConfig.providers[defaultProviderKey];

    if (defaultProvider.type === "router" || defaultProvider.type === "hybrid") {
      const routes = defaultProvider.routes || {};
      const primaryRouteStr = routes[mode] || "auto";
      return this._buildDynamicChain(fullConfig, mode, primaryRouteStr);
    }

    const anyModel = defaultProvider.models ? Object.values(defaultProvider.models)[0] || null : null;
    return {
      chain: [{
        providerKey: defaultProviderKey,
        providerConfig: defaultProvider,
        modelName: anyModel,
        score: 100
      }],
      rankedLog: `single_provider=${defaultProviderKey}`
    };
  }

  async _buildDynamicChain(fullConfig, mode, primaryRouteStr) {
    await this.metricsStore.init();
    const metrics = this.metricsStore.getAllMetrics();
    const candidates = [];

    for (const [provKey, provConfig] of Object.entries(fullConfig.providers)) {
      if (provConfig.type === "router" || provConfig.type === "hybrid") continue;
      if (!provConfig.models) continue;

      for (const [role, modelName] of Object.entries(provConfig.models)) {
        const routeStr = `${provKey}.${role}`;
        const metricKey = `${provKey}.${modelName}`;
        const stat = metrics[metricKey] || { success: 0, fail: 0, err429: 0, err404: 0, latencyMs: 0 };
        
        const total = stat.success + stat.fail;
        const health = total > 0 ? stat.success / total : 1.0;
        
        let score = health * 100;
        
        // Latency penalty
        if (stat.latencyMs > 2000) score -= (stat.latencyMs / 1000);
        
        // Error penalties
        score -= (stat.err429 * 5);
        score -= (stat.err404 * 10);
        
        // Provider & Mode preferences
        if (provConfig.type === "ollama") score += 15; // Prefer local
        if (mode === "fixBug" && role === "coder") score += 10;
        if (mode === "explain" && provConfig.free_only) score += 15;
        if (mode === "heavy" && role === "heavy") score += 20;

        candidates.push({
          routeStr,
          providerKey: provKey,
          providerConfig: provConfig,
          modelName,
          role,
          score: Math.max(0, Math.round(score))
        });
      }
    }

    // Sort by score descending
    candidates.sort((a, b) => b.score - a.score);

    const chain = [];
    const seen = new Set();
    const rankedLogParts = [];

    if (primaryRouteStr && primaryRouteStr !== "auto") {
      const match = candidates.find(c => c.routeStr === primaryRouteStr);
      if (match) {
        chain.push(match);
        seen.add(match.routeStr);
      } else {
        // Construct ad-hoc specific route if not found in models
        const parts = primaryRouteStr.split(".");
        if (parts.length === 2 && fullConfig.providers[parts[0]]) {
          const pConfig = fullConfig.providers[parts[0]];
          const pModel = pConfig.models && pConfig.models[parts[1]] ? pConfig.models[parts[1]] : parts[1];
          const adhoc = {
            routeStr: primaryRouteStr,
            providerKey: parts[0],
            providerConfig: pConfig,
            modelName: pModel,
            role: parts[1],
            score: 999
          };
          chain.push(adhoc);
          seen.add(primaryRouteStr);
        }
      }
    }

    for (const c of candidates) {
      rankedLogParts.push(`${c.routeStr} score=${c.score}`);
      if (!seen.has(c.routeStr)) {
        chain.push(c);
        seen.add(c.routeStr);
      }
    }

    return { chain, rankedLog: `[${rankedLogParts.join(", ")}]` };
  }

  async generate(mode, messages, onEvent) {
    const route = await this.resolveRoute(mode);
    const chain = route.chain;
    
    this.log(`mode=${mode} selection=auto ranked_candidates=${route.rankedLog}`);

    let lastError = null;
    let attempt = 0;

    for (const link of chain) {
      if (!link.modelName) {
        this.log(`mode=${mode} SKIP provider=${link.providerKey} reason=no_model`);
        continue;
      }

      const isFallback = attempt > 0;
      this.log(`mode=${mode} route=${link.routeStr || link.providerKey} model=${link.modelName} fallback_used=${isFallback}`);

      if (isFallback && typeof onEvent === "function") {
        onEvent("fallback", `Falling back to ${link.providerKey} / ${link.modelName}…`);
      }

      const provider = createProvider(link.providerConfig.type, link.providerConfig, this.output);
      const startMs = Date.now();

      try {
        const options = {
          temperature: link.providerConfig.temperature || 0.2,
          maxTokens: link.providerConfig.context_window || 2048
        };
        const response = await provider.generate(link.modelName, messages, options);
        const latencyMs = Date.now() - startMs;

        this.log(`mode=${mode} chosen=${link.routeStr || link.providerKey} latency=${latencyMs}ms status=ok fallback_used=${isFallback}`);
        await this.metricsStore.recordSuccess(link.providerKey, link.modelName, latencyMs);

        return {
          content: response,
          provider: link.providerKey,
          model: link.modelName,
          latencyMs,
          fallbackUsed: isFallback
        };
      } catch (err) {
        const latencyMs = Date.now() - startMs;
        this.log(`mode=${mode} route=${link.routeStr || link.providerKey} model=${link.modelName} latency=${latencyMs}ms status=error error=${err.message}`);
        await this.metricsStore.recordFailure(link.providerKey, link.modelName, err);
        lastError = err;
      }
      attempt++;
    }

    throw new Error(`All providers in fallback chain failed for mode '${mode}'. Last error: ${lastError ? lastError.message : "No valid routes configured."}`);
  }

  async checkHealth() {
    try {
      const fullConfig = await this.configLoader.getConfig();
      if (!fullConfig || !fullConfig.providers) {
        return [{ provider: "config", ok: false, message: "Invalid models.json: missing 'providers'." }];
      }

      const defaultKey = fullConfig.default_provider;
      const defaultProv = fullConfig.providers[defaultKey] || {};
      const routeTable = (defaultProv.type === "router" || defaultProv.type === "hybrid")
        ? this._buildRouteTable(fullConfig, defaultProv)
        : {};

      const results = [];
      results.push({
        provider: "config",
        ok: true,
        message: `Config loaded from .ultra/models.json (default_provider: ${defaultKey})`
      });

      for (const [key, config] of Object.entries(fullConfig.providers)) {
        if (config.type === "router" || config.type === "hybrid") continue;

        try {
          const provider = createProvider(config.type, config, this.output);
          if ((config.type === "openai_compatible" || config.type === "openrouter") && typeof provider.checkHealthDetailed === "function") {
            const detailed = await provider.checkHealthDetailed(routeTable);
            for (const r of detailed) {
              results.push({ provider: key, ok: r.ok, message: r.message });
            }
          } else {
            const res = await provider.checkHealth();
            results.push({ provider: key, ok: res.ok, message: res.message });
          }
        } catch (e) {
          results.push({ provider: key, ok: false, message: e.message });
        }
      }

      for (const [mode, dest] of Object.entries(routeTable)) {
        results.push({ provider: `route:${mode}`, ok: true, message: `${mode} → ${dest}` });
      }

      return results;
    } catch (e) {
      return [{ provider: "config", ok: false, message: e.message }];
    }
  }

  _buildRouteTable(fullConfig, routerConfig) {
    const table = {};
    const routes = routerConfig.routes || {};
    for (const [mode, routeStr] of Object.entries(routes)) {
      if (routeStr === "auto") {
        table[mode] = "Auto";
        continue;
      }
      const dotIdx = routeStr.indexOf(".");
      if (dotIdx === -1) {
        table[mode] = routeStr;
        continue;
      }
      const provKey = routeStr.slice(0, dotIdx);
      const modelRole = routeStr.slice(dotIdx + 1);
      const provConfig = fullConfig.providers[provKey];
      const modelName = provConfig && provConfig.models && provConfig.models[modelRole]
        ? provConfig.models[modelRole]
        : modelRole;
      table[mode] = `${routeStr} (${modelName})`;
    }
    return table;
  }
}

module.exports = { RouterProvider, createProvider };
