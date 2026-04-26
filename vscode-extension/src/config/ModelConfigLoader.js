"use strict";

const fs   = require("node:fs/promises");
const fsSync = require("node:fs");
const path = require("node:path");

class ModelConfigLoader {
  constructor(workspaceRoot) {
    this.workspaceRoot = workspaceRoot;
    this.configPath = workspaceRoot ? path.join(workspaceRoot, ".ultra", "models.json") : null;
    this.envPath    = workspaceRoot ? path.join(workspaceRoot, ".env") : null;
    this.cachedConfig = null;
    this.cachedEnv    = {};
    this.lastMtime    = 0;
    this._watcher     = null;
  }

  // ─── Env Loading ─────────────────────────────────────────────────────────

  async loadEnv() {
    if (!this.envPath) return;
    try {
      const content = await fs.readFile(this.envPath, "utf8");
      this.cachedEnv = {};
      const lines = content.split(/\r?\n/);
      for (const line of lines) {
        const trimmed = line.trim();
        if (!trimmed || trimmed.startsWith("#")) continue;
        const eqIdx = trimmed.indexOf("=");
        if (eqIdx !== -1) {
          const k = trimmed.substring(0, eqIdx).trim();
          let v   = trimmed.substring(eqIdx + 1).trim();
          if (v.startsWith('"') && v.endsWith('"')) v = v.slice(1, -1);
          else if (v.startsWith("'") && v.endsWith("'")) v = v.slice(1, -1);
          this.cachedEnv[k] = v;
        }
      }
    } catch {
      this.cachedEnv = {};
    }
  }

  // ─── Env Substitution ────────────────────────────────────────────────────

  substituteEnv(value) {
    if (typeof value !== "string") return value;
    return value.replace(/\$\{([^}]+)\}/g, (match, p1) => {
      if (this.cachedEnv[p1] !== undefined) return this.cachedEnv[p1];
      if (process.env[p1]     !== undefined) return process.env[p1];
      return match; // leave unresolved placeholder as-is
    });
  }

  deepSubstitute(obj) {
    if (obj === null || typeof obj !== "object") {
      return this.substituteEnv(obj);
    }
    if (Array.isArray(obj)) {
      return obj.map((item) => this.deepSubstitute(item));
    }
    const result = {};
    for (const [k, v] of Object.entries(obj)) {
      result[k] = this.deepSubstitute(v);
    }
    return result;
  }

  // ─── Config Loading (mtime-based hot-reload cache) ───────────────────────

  async loadConfig() {
    if (!this.configPath) {
      throw new Error("No workspace root provided for ModelConfigLoader.");
    }

    let stat;
    try {
      stat = await fs.stat(this.configPath);
    } catch (e) {
      throw new Error(
        `models.json not found at ${this.configPath}. ` +
        `Create .ultra/models.json to configure ULTRA providers. (${e.message})`
      );
    }

    // Return cached copy if file hasn't changed
    if (this.cachedConfig && stat.mtimeMs === this.lastMtime) {
      return this.cachedConfig;
    }

    this.lastMtime = stat.mtimeMs;
    await this.loadEnv();

    let rawJson;
    try {
      const content = await fs.readFile(this.configPath, "utf8");
      rawJson = JSON.parse(content);
    } catch (e) {
      throw new Error(
        `Failed to parse models.json at ${this.configPath}: ${e.message}. ` +
        `Check for syntax errors (missing commas, unclosed braces, trailing commas).`
      );
    }

    this.cachedConfig = this.deepSubstitute(rawJson);
    return this.cachedConfig;
  }

  async getConfig() {
    return await this.loadConfig();
  }

  // ─── Hot Reload Watcher ───────────────────────────────────────────────────

  /**
   * Watch models.json for changes and call `callback(config)` whenever
   * the file is modified. Debounced to 300ms to avoid rapid-fire events.
   *
   * @param {(config: object) => void} callback
   * @returns {() => void} unwatch function
   */
  watch(callback) {
    if (!this.configPath) return () => {};
    if (this._watcher) {
      this._watcher.close();
      this._watcher = null;
    }

    let debounceTimer = null;

    try {
      this._watcher = fsSync.watch(this.configPath, (eventType) => {
        if (eventType !== "change" && eventType !== "rename") return;

        clearTimeout(debounceTimer);
        debounceTimer = setTimeout(async () => {
          // Invalidate cache
          this.lastMtime = 0;
          this.cachedConfig = null;

          try {
            const config = await this.loadConfig();
            if (typeof callback === "function") {
              callback(null, config);
            }
          } catch (e) {
            if (typeof callback === "function") {
              callback(e, null);
            }
          }
        }, 300);
      });

      this._watcher.on("error", () => {
        // Silently ignore watcher errors (e.g. file deleted)
      });
    } catch {
      // Watcher not available (e.g. network drives) — hot reload gracefully disabled
    }

    return () => {
      clearTimeout(debounceTimer);
      if (this._watcher) {
        this._watcher.close();
        this._watcher = null;
      }
    };
  }

  /**
   * Stop the file watcher if running.
   */
  unwatch() {
    if (this._watcher) {
      this._watcher.close();
      this._watcher = null;
    }
  }
  /**
   * Updates a specific route for a mode and saves it to models.json.
   */
  async updateRoute(mode, routeDest) {
    const fullConfig = await this.getConfig();
    if (!fullConfig || !fullConfig.providers) {
      throw new Error("Invalid config: no providers block found.");
    }
    const defaultProvKey = fullConfig.default_provider;
    const routerProv = fullConfig.providers[defaultProvKey];
    if (!routerProv || (routerProv.type !== "router" && routerProv.type !== "hybrid")) {
      throw new Error(`Default provider '${defaultProvKey}' is not a router.`);
    }

    if (!routerProv.routes) {
      routerProv.routes = {};
    }
    routerProv.routes[mode] = routeDest;

    await this.saveConfig(fullConfig);
  }

  /**
   * Safely stringifies and writes configuration back to models.json.
   * This bypasses the deepSubstitute logic to save raw config.
   */
  async saveConfig(newConfig) {
    if (!this.configPath) {
      throw new Error("No config path found to save models.json.");
    }
    const content = JSON.stringify(newConfig, null, 2);
    await fs.writeFile(this.configPath, content, "utf8");
  }
}

module.exports = { ModelConfigLoader };
