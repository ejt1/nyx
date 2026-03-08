'use strict';

const fs = require('fs');
const path = require('path');
const process = internalBinding('process');
const packages = require('internal/modules/package');
const loader = require('internal/modules/loader');

const cwd = process.nyxCwd();
let config = {};

if (cwd) {
  const configPath = path.join(cwd, 'config.json');
  try {
    const content = fs.readFileSync(configPath, 'utf8');
    config = JSON.parse(content);
    debugLog('Loaded config from ' + configPath);
  } catch (e) {
    // fallback to defaults
  }
}

if (config.lib_path) {
  loader.setLibPath(config.lib_path);
  debugLog('Library path set to: ' + config.lib_path);
}

const scriptsPath = config.scripts_path || (cwd ? path.join(cwd, 'scripts') : '');
if (scriptsPath) {
  process.setScriptsRoot(scriptsPath);
}

globalThis.__nyx_config = config;
globalThis.__nyx_config_path = cwd ? path.join(cwd, 'config.json') : '';

packages.scanPackages();
loader.runRuntimes();
