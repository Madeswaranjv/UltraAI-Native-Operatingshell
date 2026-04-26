"use strict";

class WorkspaceTaskQueue {
  constructor() {
    this._states = new Map();
  }

  _stateFor(workspaceKey) {
    const key = workspaceKey || "__global__";
    if (!this._states.has(key)) {
      this._states.set(key, {
        chain: Promise.resolve(),
        patchTaskRunning: false
      });
    }
    return this._states.get(key);
  }

  async run(workspaceKey, options, task) {
    const state = this._stateFor(workspaceKey);
    const patchTask = Boolean(options && options.patchTask);

    if (patchTask && state.patchTaskRunning) {
      throw new Error("A patching task is already running for this workspace.");
    }

    const wrapped = async () => {
      if (patchTask) {
        state.patchTaskRunning = true;
      }
      try {
        return await task();
      } finally {
        if (patchTask) {
          state.patchTaskRunning = false;
        }
      }
    };

    const next = state.chain.then(wrapped, wrapped);
    state.chain = next.catch(() => undefined);
    return next;
  }
}

module.exports = {
  WorkspaceTaskQueue
};
