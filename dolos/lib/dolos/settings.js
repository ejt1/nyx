'use strict';

const gui = require('gui');
const fs = require('fs');

let panel = null;

function createSettingsPanel() {
  if (panel) return panel;

  const config = globalThis.__nyx_config || {};
  const configPath = globalThis.__nyx_config_path;

  panel = new gui.Panel('Nyx Settings');

  const libPathInput = new gui.InputText('Library Path', 512, config.lib_path || '');
  const scriptsPathInput = new gui.InputText('Scripts Path', 512, config.scripts_path || '');
  const saveBtn = new gui.Button('Save');
  const statusText = new gui.Text('');

  panel.add(libPathInput);
  panel.add(scriptsPathInput);
  panel.add(new gui.Separator());
  panel.add(saveBtn);
  panel.add(statusText);

  saveBtn.on('click', () => {
    if (!configPath) {
      statusText.text = 'Error: no config path set';
      return;
    }

    const newConfig = Object.assign({}, config);
    const libPath = libPathInput.text;
    const scriptsPath = scriptsPathInput.text;

    if (libPath) {
      newConfig.lib_path = libPath;
    } else {
      delete newConfig.lib_path;
    }

    if (scriptsPath) {
      newConfig.scripts_path = scriptsPath;
    } else {
      delete newConfig.scripts_path;
    }

    try {
      fs.writeFileSync(configPath, JSON.stringify(newConfig, null, 2));
      statusText.text = 'Saved. Restart nyx to apply changes.';
    } catch (e) {
      statusText.text = 'Error: ' + e.message;
    }
  });

  return panel;
}

export { createSettingsPanel };
