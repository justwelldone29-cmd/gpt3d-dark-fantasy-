'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
function agent(platform = 'windows', arch = 'x64') {
    let hooks = 0;
    const context = vm.createContext({
        Process: { platform, arch, pointerSize: 8,
            getModuleByName() { return { path: 'fixture.dll', findExportByName() { return null; } }; } },
        rpc: {}, send() {},
        Interceptor: { attach() { hooks++; }, flush() {} }
    });
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../agent.js'), 'utf8'), context);
    context.rpc.exports.moduleinfo();
    return { rpc: context.rpc.exports, hookCount: () => hooks };
}
test('rejects unsupported native ABI before installing hooks', () => {
    const instance = agent('linux');
    assert.throws(() => instance.rpc.start({}), /Windows x64/);
    assert.equal(instance.hookCount(), 0);
});
test('rejects missing metadata API before installing hooks', () => {
    const instance = agent();
    assert.throws(() => instance.rpc.start({}), /Missing export: il2cpp_domain_get/);
    assert.equal(instance.hookCount(), 0);
});
test('rejects invalid user settings before reading native state', () => {
    for (const config of [{ fovDegrees: NaN }, { fovDegrees: 180 }, { maxDistance: Infinity }, { enabled: 1 }]) {
        const instance = agent();
        assert.throws(() => instance.rpc.start(config), /Invalid configuration/);
        assert.equal(instance.hookCount(), 0);
    }
});
test('a rejected configuration does not modify the existing settings', () => {
    const instance = agent();
    instance.rpc.configure({ fovDegrees: 6 });
    assert.throws(() => instance.rpc.configure({ fovDegrees: 1000 }), /Invalid configuration/);
    assert.equal(instance.rpc.status().fovDegrees, 6);
    assert.equal(instance.rpc.status().enabled, false);
});
