"use strict";

function stringifyMessage(message) {
  if (typeof message === "string") {
    return message;
  }
  if (message instanceof Error) {
    return message.stack || message.message;
  }
  return String(message);
}

function safeLog(outputChannel, message, fallbackLabel) {
  const line = stringifyMessage(message);
  try {
    if (outputChannel && typeof outputChannel.appendLine === "function") {
      outputChannel.appendLine(line);
      return true;
    }
  } catch (error) {
    const failure = error && error.message ? error.message : String(error);
    console.log(`${fallbackLabel || "[ULTRA-LOG]"} output append failed: ${failure}`);
  }

  console.log(line);
  return false;
}

module.exports = {
  safeLog
};
