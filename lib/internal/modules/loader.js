'use strict';

// User-land ES Module Loader
// Uses BuiltinModule from realm for loading builtins

const {
  BuiltinModule,
} = require('internal/bootstrap/realm');

const {
  ModuleWrap,
  setImportModuleDynamicallyCallback,
  setInitializeImportMetaObjectCallback,
  kUninstantiated,
  kInstantiating,
  kInstantiated,
  kEvaluating,
  kEvaluated,
  kErrored,
} = internalBinding('module_wrap');

const fs = require('fs');

const packages = require('internal/modules/package');

const moduleCache = new Map();

let libPath_ = null;

function setLibPath(p) {
  libPath_ = p.replace(/\\/g, '/');
  if (!libPath_.endsWith('/')) libPath_ += '/';
}

class Loader {
  constructor() {
    this.moduleCache = moduleCache;
  }

  // Synchronous import for static module loading
  importSync(specifier, referrer) {
    const resolved = this.resolve(specifier, referrer);
    return this.getModuleNamespaceSync(resolved);
  }

  // Async import for dynamic import()
  importAsync(specifier, referrer) {
    return new Promise((resolve, reject) => {
      try {
        const resolved = this.resolve(specifier, referrer);
        const namespace = this.getModuleNamespaceSync(resolved);
        resolve(namespace);
      } catch (err) {
        reject(err);
      }
    });
  }

  resolve(specifier, referrer) {
    // Handle different specifier types
    if (specifier.startsWith('./') || specifier.startsWith('../')) {
      // Relative import
      return this.resolveRelative(specifier, referrer);
    } else if (specifier.startsWith('/') || /^[a-zA-Z]:/.test(specifier)) {
      // Absolute path
      return specifier;
    } else if (BuiltinModule.isBuiltin(specifier)) {
      // Built-in module (handles both 'nyx:fs' and 'fs')
      // Normalize to 'nyx:' URL format
      if (specifier.startsWith('nyx:')) {
        return specifier;
      }
      return 'nyx:' + specifier;
    } else {
      // Bare specifier - package import
      return this.resolvePackage(specifier, referrer);
    }
  }

  resolveRelative(specifier, referrer) {
    // Get directory of referrer
    const lastSlash = Math.max(referrer.lastIndexOf('/'), referrer.lastIndexOf('\\'));
    const dir = lastSlash >= 0 ? referrer.substring(0, lastSlash + 1) : '';

    // Normalize the path
    let path = dir + specifier;

    // Add .js extension if missing
    if (!path.endsWith('.js') && !path.endsWith('.mjs') && !path.endsWith('.json')) {
      path = path + '.js';
    }

    return this.normalizePath(path);
  }

  resolvePackage(specifier, referrer) {
    // Use package registry to resolve bare specifiers
    const resolved = packages.resolvePackage(specifier, referrer);
    if (!resolved) {
      throw new Error('Cannot resolve package "' + specifier + '" from ' + referrer);
    }

    return resolved;
  }

  normalizePath(path) {
    // Handle . and .. in path
    const parts = path.replace(/\\/g, '/').split('/');
    const result = [];

    for (const part of parts) {
      if (part === '..') {
        result.pop();
      } else if (part !== '.' && part !== '') {
        result.push(part);
      }
    }

    return result.join('/');
  }

  getModuleNamespaceSync(resolved) {
    let wrap = this.moduleCache.get(resolved);

    if (!wrap) {
      wrap = this.loadModuleSync(resolved);
    }

    if (wrap.getStatus() < kInstantiated) {
      this.instantiateSync(wrap);
    }

    if (wrap.getStatus() < kEvaluated) {
      this.evaluateSync(wrap);
    }

    return wrap.getNamespace();
  }

  loadModuleSync(url) {
    // Check cache first
    if (this.moduleCache.has(url)) {
      return this.moduleCache.get(url);
    }

    let wrap;

    if (BuiltinModule.isBuiltin(url)) {
      const normalizedId = BuiltinModule.normalizeRequirableId(url);

      if (!normalizedId) {
        throw new Error('Cannot require built-in module: ' + url);
      }

      if (libPath_ && !normalizedId.startsWith('internal/')) {
        const diskPath = libPath_ + normalizedId + '.js';
        if (fs.existsSync(diskPath)) {
          wrap = this.loadCjsFromDisk(url, diskPath);
          this.moduleCache.set(url, wrap);
          return wrap;
        }
      }

      const mod = BuiltinModule.map.get(normalizedId);
      if (!mod) {
        throw new Error('Built-in module not found: ' + url);
      }

      // Compile for public use and get ESM facade
      mod.compileForPublicLoader();
      wrap = mod.getESMFacade();
    } else {
      // Load from filesystem as ES module
      const source = fs.readFileSync(url, 'utf8');
      wrap = new ModuleWrap(url, source, 0, 0);
    }

    this.moduleCache.set(url, wrap);
    return wrap;
  }

  // Link a module by resolving all its dependencies
  linkModule(wrap, seen) {
    const url = wrap.url;
    if (seen.has(url)) {
      return;
    }
    seen.add(url);

    // Get module requests (dependencies)
    const requests = wrap.getModuleRequests();
    const resolvedModules = [];

    for (let i = 0; i < requests.length; i++) {
      const request = requests[i];
      const specifier = request.specifier;

      // Resolve the specifier
      const resolvedUrl = this.resolve(specifier, url);

      // Load the dependency
      const depWrap = this.loadModuleSync(resolvedUrl);

      // Recursively link the dependency
      this.linkModule(depWrap, seen);

      resolvedModules.push(depWrap);
    }

    // Link with resolved modules
    wrap.link(resolvedModules);
  }

  instantiateSync(wrap) {
    // First, link all dependencies recursively
    this.linkModule(wrap, new Set());

    // Then instantiate
    wrap.instantiate();
  }

  loadCjsFromDisk(url, diskPath) {
    const source = fs.readFileSync(diskPath, 'utf8');
    debugLog('Loaded from disk: ' + diskPath);

    // wrap function(exports, require, module, internalBinding)
    const module = { exports: {} };
    const requireFn = (id) => {
      if (id.startsWith('./') || id.startsWith('../')) {
        const lastSlash = Math.max(diskPath.lastIndexOf('/'), diskPath.lastIndexOf('\\'));
        const dir = lastSlash >= 0 ? diskPath.substring(0, lastSlash + 1) : '';
        let resolved = dir + id;
        if (!resolved.endsWith('.js')) resolved += '.js';
        resolved = this.normalizePath(resolved);
        const depSource = fs.readFileSync(resolved, 'utf8');
        const depModule = { exports: {} };
        const depFn = new Function('exports', 'require', 'module', 'internalBinding', depSource);
        depFn(depModule.exports, requireFn, depModule, internalBinding);
        return depModule.exports;
      }
      const builtinMod = BuiltinModule.map.get(id);
      if (builtinMod) {
        return builtinMod.compileForInternalLoader();
      }
      throw new Error('Cannot require "' + id + '" from disk module ' + diskPath);
    };

    const fn = new Function('exports', 'require', 'module', 'internalBinding', source);
    fn(module.exports, requireFn, module, internalBinding);

    const exports = module.exports;
    const exportKeys = Object.keys(exports);
    if (!exportKeys.includes('default')) {
      exportKeys.push('default');
    }

    const wrap = new ModuleWrap(url, exportKeys, function () {
      for (let i = 0; i < exportKeys.length; i++) {
        const key = exportKeys[i];
        if (key === 'default') {
          this.setExport('default', exports);
        } else {
          this.setExport(key, exports[key]);
        }
      }
    });
    wrap.instantiate();
    wrap.evaluate();
    return wrap;
  }

  evaluateSync(wrap) {
    // Use evaluateSync for synchronous evaluation
    // This will throw if the module has top-level await
    return wrap.evaluateSync();
  }
}

// Global loader instance
const loader = new Loader();

// Set up import.meta callback
setInitializeImportMetaObjectCallback((id, meta, wrap) => {
  // Set import.meta.url
  meta.url = wrap.url;
});

// Set up dynamic import callback
setImportModuleDynamicallyCallback((specifier, referrer) => {
  return loader.importAsync(specifier, referrer);
});

// Execute all runtime packages
function runRuntimes() {
  const runtimes = packages.getRuntimePackages();
  debugLog('Found ' + runtimes.length + ' runtime package(s)');

  for (let i = 0; i < runtimes.length; i++) {
    const pkg = runtimes[i];
    debugLog('Executing runtime: ' + pkg.name + ' (' + pkg.main + ')');
    try {
      loader.getModuleNamespaceSync(pkg.main);
    } catch (e) {
      debugLog('Error executing runtime "' + pkg.name + '": ' + e.message);
      debugLog(e.stack);
    }
  }
}

module.exports = {
  loader,
  runRuntimes,
  setLibPath,

  import: (specifier, referrer) => loader.importAsync(specifier, referrer),
  resolve: (specifier, referrer) => loader.resolve(specifier, referrer),
  load: (url) => loader.loadModuleSync(url),
};
