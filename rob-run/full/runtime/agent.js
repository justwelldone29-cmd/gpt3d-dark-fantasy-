// Experimental Windows x64 sidecar. Resolves metadata from the running process.
'use strict';

let api, mod, hooks = [];
let settings = { enabled: false, fovDegrees: 10, maxDistance: 200 };
let state = { installed: false, redirects: 0, candidates: 0, errors: 0, lastError: null };
let owner = null, snapshot = null, mainThread = null;
let menuModule = null, bridge = null, lastBridgePoll = 0;
const CACHE_MS = 100, MAX_AGE_MS = 250, SELF_RADIUS = 2;
const report = (kind, fields) => send(Object.assign({ kind }, fields));
const num = n => typeof n === 'number' ? n : n.toNumber();

function bind(name, result, args) {
    const address = mod.findExportByName('il2cpp_' + name);
    if (!address || address.isNull()) throw new Error('Missing export: il2cpp_' + name);
    return new NativeFunction(address, result, args);
}

function initApi() {
    if (Process.platform !== 'windows' || Process.arch !== 'x64' || Process.pointerSize !== 8)
        throw new Error('Only Windows x64 native ABI is supported');
    api = {
        domain: bind('domain_get', 'pointer', []),
        assemblies: bind('domain_get_assemblies', 'pointer', ['pointer', 'pointer']),
        image: bind('assembly_get_image', 'pointer', ['pointer']),
        klass: bind('class_from_name', 'pointer', ['pointer', 'pointer', 'pointer']),
        objectClass: bind('object_get_class', 'pointer', ['pointer']),
        parent: bind('class_get_parent', 'pointer', ['pointer']),
        methods: bind('class_get_methods', 'pointer', ['pointer', 'pointer']),
        methodName: bind('method_get_name', 'pointer', ['pointer']),
        paramCount: bind('method_get_param_count', 'uint32', ['pointer']),
        paramType: bind('method_get_param', 'pointer', ['pointer', 'uint32']),
        paramName: bind('method_get_param_name', 'pointer', ['pointer', 'uint32']),
        returnType: bind('method_get_return_type', 'pointer', ['pointer']),
        flags: bind('method_get_flags', 'uint32', ['pointer', 'pointer']),
        typeName: bind('type_get_name', 'pointer', ['pointer']),
        free: bind('free', 'void', ['pointer']),
        classType: bind('class_get_type', 'pointer', ['pointer']),
        typeObject: bind('type_get_object', 'pointer', ['pointer']),
        field: bind('class_get_field_from_name', 'pointer', ['pointer', 'pointer']),
        fieldOffset: bind('field_get_offset', 'int32', ['pointer']),
        fieldType: bind('field_get_type', 'pointer', ['pointer']),
        valueSize: bind('class_value_size', 'int32', ['pointer', 'pointer']),
        invoke: bind('runtime_invoke', 'pointer', ['pointer', 'pointer', 'pointer', 'pointer']),
        unbox: bind('object_unbox', 'pointer', ['pointer']),
        arrayLength: bind('array_length', 'size_t', ['pointer']),
        pin: bind('gchandle_new', 'uint32', ['pointer', 'uint8']),
        pinned: bind('gchandle_get_target', 'pointer', ['uint32']),
        unpin: bind('gchandle_free', 'void', ['uint32']),
        attach: bind('thread_attach', 'pointer', ['pointer']),
        detach: bind('thread_detach', 'void', ['pointer'])
    };
}

function text(p) { return p.isNull() ? '' : p.readUtf8String(); }
function typeName(p) {
    const result = api.typeName(p);
    try { return text(result); } finally { if (!result.isNull()) api.free(result); }
}

function klass(ns, name) {
    const countPtr = Memory.alloc(8); countPtr.writeU64(0);
    const domain = api.domain();
    if (domain.isNull()) throw new Error('IL2CPP is not initialized');
    const assemblies = api.assemblies(domain, countPtr);
    const count = num(countPtr.readU64());
    if (assemblies.isNull() || count < 1 || count > 4096) throw new Error('Invalid assembly list');
    const namespace = Memory.allocUtf8String(ns), className = Memory.allocUtf8String(name);
    for (let i = 0; i < count; i++) {
        const image = api.image(assemblies.add(i * 8).readPointer());
        if (image.isNull()) continue;
        const found = api.klass(image, namespace, className);
        if (!found.isNull()) return found;
    }
    throw new Error('Missing class: ' + ns + '.' + name);
}

function signature(method) {
    const parameters = [];
    for (let i = 0; i < api.paramCount(method); i++) parameters.push({
        name: text(api.paramName(method, i)), type: typeName(api.paramType(method, i)) });
    const flagsPtr = Memory.alloc(4); flagsPtr.writeU32(0);
    return { method, name: text(api.methodName(method)), parameters,
        returnType: typeName(api.returnType(method)), flags: api.flags(method, flagsPtr) };
}

function method(klass, name, parameterTypes, returnType, isStatic) {
    for (let current = klass; !current.isNull(); current = api.parent(current)) {
        const iter = Memory.alloc(8); iter.writePointer(NULL);
        const matches = [];
        for (;;) {
            const mi = api.methods(current, iter);
            if (mi.isNull()) break;
            if (text(api.methodName(mi)) !== name || api.paramCount(mi) !== parameterTypes.length) continue;
            const s = signature(mi);
            if (s.parameters.every((p, i) => parameterTypes[i] === null || p.type === parameterTypes[i]) &&
                (returnType === null || s.returnType === returnType) &&
                (isStatic === undefined || !!(s.flags & 0x10) === isStatic)) matches.push(s);
        }
        if (matches.length === 1) return matches[0];
        if (matches.length > 1) throw new Error('Ambiguous method: ' + name);
    }
    throw new Error('Missing or incompatible method: ' + name);
}

const getters = new Map(), offsets = new Map();
function get(obj, name, returnType = null) {
    if (obj.isNull()) throw new Error('Null object for ' + name);
    const k = api.objectClass(obj), key = k + ':' + name + ':' + returnType;
    if (!getters.has(key)) getters.set(key, method(k, name, [], returnType, false).method);
    return invoke(getters.get(key), obj);
}

function offset(k, name, expectedType = null) {
    const key = k + ':' + name;
    if (!offsets.has(key)) {
        let found = NULL;
        const fieldName = Memory.allocUtf8String(name);
        for (let current = k; !current.isNull() && found.isNull(); current = api.parent(current))
            found = api.field(current, fieldName);
        if (found.isNull()) throw new Error('Missing field: ' + name);
        offsets.set(key, { offset: api.fieldOffset(found), type: typeName(api.fieldType(found)) });
    }
    const field = offsets.get(key);
    if (field.offset < 16 || (expectedType !== null && field.type !== expectedType))
        throw new Error('Incompatible field: ' + name);
    return field.offset;
}

function invoke(mi, obj = NULL, values = []) {
    const params = values.length ? Memory.alloc(values.length * 8) : NULL;
    values.forEach((value, i) => params.add(i * 8).writePointer(value));
    const exception = Memory.alloc(8); exception.writePointer(NULL);
    const result = api.invoke(mi, obj, params, exception);
    if (!exception.readPointer().isNull()) throw new Error('Managed exception during runtime invoke');
    return result;
}

function vector(address) {
    const value = [address.readFloat(), address.add(4).readFloat(), address.add(8).readFloat()];
    if (!AimMath.valid(value)) throw new Error('Invalid vector');
    return value;
}
function vectorMemory(value) {
    const p = Memory.alloc(12);
    value.forEach((x, i) => p.add(i * 4).writeFloat(x));
    return p;
}
function bool(boxed) { return !boxed.isNull() && api.unbox(boxed).readU8() !== 0; }
function modelPosition(model) {
    const transform = get(model, 'get_Transform');
    const k = api.objectClass(transform);
    return ['_x', '_y', '_z'].map(name => {
        const data = transform.add(offset(k, name)).readPointer();
        if (data.isNull()) throw new Error('Missing model coordinate');
        return data.add(offset(api.objectClass(data), '_value', 'System.Single')).readFloat();
    });
}
function position(transform, getPosition) {
    if (transform.isNull()) throw new Error('Missing transform');
    return vector(api.unbox(invoke(getPosition, transform)));
}

function validateConfig(next) {
    const result = Object.assign({}, settings, next);
    if (typeof result.enabled !== 'boolean' || !Number.isFinite(result.fovDegrees) ||
        result.fovDegrees < 0.1 || result.fovDegrees > 90 || !Number.isFinite(result.maxDistance) ||
        result.maxDistance < 1 || result.maxDistance > 500) throw new Error('Invalid configuration');
    return result;
}
function connectBridge() {
    if (!menuModule) throw new Error('The new internal menu DLL has not been loaded');
    const native = (name, result, args) => {
        const p = menuModule.findExportByName(name);
        if (!p || p.isNull()) throw new Error('Incompatible menu DLL: ' + name);
        return new NativeFunction(p, result, args);
    };
    bridge = {
        read: native('RobRunSilent_ReadConfig', 'int', ['pointer', 'uint32']),
        write: native('RobRunSilent_Configure', 'int', ['int', 'float', 'float']),
        status: native('RobRunSilent_ReportStatus', 'void', ['uint32', 'uint64', 'uint32', 'pointer'])
    };
}
function reportBridge(phase, message) {
    if (bridge) bridge.status(phase, state.redirects, state.candidates, Memory.allocUtf8String(message));
}
function pollBridge() {
    if (!bridge || Date.now() - lastBridgePoll < 100) return;
    lastBridgePoll = Date.now();
    const cfg = Memory.alloc(24);
    if (bridge.read(cfg, 24) !== 1 || cfg.readU32() !== 1 || cfg.add(4).readU32() !== 24)
        throw new Error('Incompatible menu configuration layout');
    const next = validateConfig({ enabled: cfg.add(8).readU32() !== 0,
        fovDegrees: cfg.add(12).readFloat(), maxDistance: cfg.add(16).readFloat() });
    if (next.enabled !== settings.enabled || next.fovDegrees !== settings.fovDegrees || next.maxDistance !== settings.maxDistance) {
        if (next.enabled) state.lastError = null;
        settings = next;
        owner = snapshot = null;
        state.candidates = 0;
    }
    reportBridge(state.lastError ? 2 : 1, state.lastError ||
        (settings.enabled ? 'Silent Aim enabled' : 'Ready - enable in Silent Aim tab'));
}

function fault(error) {
    settings.enabled = false;
    owner = snapshot = null;
    state.candidates = 0;
    state.errors++;
    state.lastError = String(error.stack || error);
    try { reportBridge(2, state.lastError); } catch (_) {}
    report('error', { error: state.lastError });
}

function stop() {
    settings.enabled = false;
    hooks.forEach(h => h.detach()); hooks = [];
    Interceptor.flush();
    owner = snapshot = null;
    state.candidates = 0;
    mainThread = null;
    state.installed = false;
    try { reportBridge(0, 'Launcher disconnected'); } catch (_) {}
}

function start(config) {
    if (state.installed) throw new Error('Already installed');
    settings = validateConfig(config);
    initApi();
    connectBridge();
    if (bridge.write(settings.enabled ? 1 : 0, settings.fovDegrees, settings.maxDistance) !== 1)
        throw new Error('Menu rejected the initial configuration');
    const domain = api.domain();
    if (domain.isNull()) throw new Error('IL2CPP is not initialized');
    const thread = api.attach(domain);
    if (thread.isNull()) throw new Error('IL2CPP thread attach failed');
    try {
        const vectorClass = klass('System.Numerics', 'Vector3');
        if (api.valueSize(vectorClass, Memory.alloc(4)) !== 12) throw new Error('Unexpected Vector3 ABI');
        const shotsClass = klass('Models.Models.ShotArgs', 'ShotArgsModel');
        const set = method(shotsClass, 'Set', [null, null, null, null], 'System.Void', false);
        const named = name => set.parameters.findIndex(p => p.name.toLowerCase() === name);
        const originIndex = named('position'), directionIndex = named('direction');
        if (originIndex < 0 || directionIndex < 0 || originIndex === directionIndex ||
            set.parameters[originIndex].type !== 'System.Numerics.Vector3' ||
            set.parameters[directionIndex].type !== 'System.Numerics.Vector3')
            throw new Error('ShotArgsModel.Set does not have the expected position/direction value parameters');
        const updaterClass = klass('Game.Battle.EntityModel.Player', 'PlayerShotsUpdater');
        const update = method(updaterClass, 'Update', [], 'System.Void', false);
        const entryUpdate = method(klass('Start', 'UnityEntry'), 'Update', [], 'System.Void', false);
        const viewClass = klass('Game.Battle.EntityModel.BattleCharacter.Views', 'BattleCharacterEntityView');
        const headOffset = offset(viewClass, '<HeadPosition>k__BackingField', 'UnityEngine.Transform');
        const component = klass('UnityEngine', 'Component');
        const getTransform = method(component, 'get_transform', [], 'UnityEngine.Transform', false).method;
        const getGameObject = method(component, 'get_gameObject', [], 'UnityEngine.GameObject', false).method;
        const active = method(klass('UnityEngine', 'GameObject'), 'get_activeInHierarchy', [], 'System.Boolean', false).method;
        const getPosition = method(klass('UnityEngine', 'Transform'), 'get_position', [], 'UnityEngine.Vector3', false).method;
        const findAll = method(klass('UnityEngine', 'Resources'), 'FindObjectsOfTypeAll', ['System.Type'], null, true).method;
        const viewType = api.typeObject(api.classType(viewClass));
        const raycast = method(klass('UnityEngine', 'Physics'), 'Raycast', [
            'UnityEngine.Vector3', 'UnityEngine.Vector3', 'System.Single', 'System.Int32',
            'UnityEngine.QueryTriggerInteraction'], 'System.Boolean', true).method;
        const address = s => {
            const p = s.method.readPointer(), range = Process.findRangeByAddress(p);
            if (p.isNull() || p.compare(mod.base) < 0 || p.compare(mod.base.add(mod.size)) >= 0 ||
                !range || !range.protection.includes('x')) throw new Error('Invalid method code pointer: ' + s.name);
            return p;
        };
        const setAddress = address(set), updateAddress = address(update), entryAddress = address(entryUpdate);
        if (new Set([setAddress.toString(), updateAddress.toString(), entryAddress.toString()]).size !== 3)
            throw new Error('Unexpected shared hook implementations');
        function refresh(updater) {
            owner = null;
            const k = api.objectClass(updater);
            const entities = updater.add(offset(k, '_battleEntitiesModel')).readPointer();
            const player = get(entities, 'get_Player');
            if (player.isNull()) { snapshot = null; return; }
            const playerId = api.unbox(get(player, 'get_Id', 'System.Int32')).readS32();
            if (updater.add(offset(k, '_id', 'System.Int32')).readS32() !== playerId) return;
            const server = get(player, 'get_ServerModel');
            const deadModel = get(server, 'get_IsDead');
            if (bool(get(deadModel, 'get_Value', 'System.Boolean'))) { snapshot = null; return; }
            const shots = updater.add(offset(k, '_shotArgsModel', 'Models.Models.ShotArgs.ShotArgsModel')).readPointer();
            if (shots.isNull()) return;
            owner = { model: shots.toString(), time: Date.now() };
            if (snapshot && snapshot.owner === owner.model && Date.now() - snapshot.time < CACHE_MS) return;
            const self = modelPosition(server);
            if (!AimMath.valid(self)) throw new Error('Invalid local-player position');
            const array = invoke(findAll, NULL, [viewType]);
            if (array.isNull()) { snapshot = null; return; }
            const handle = api.pin(array, 0);
            if (handle === 0) throw new Error('Could not root the view array');
            const points = [];
            try {
                const length = num(api.arrayLength(api.pinned(handle)));
                if (length > 4096) throw new Error('Unexpected character-view count');
                for (let i = 0; i < length; i++) {
                    const view = api.pinned(handle).add(32 + i * 8).readPointer();
                    if (view.isNull()) continue;
                    try {
                        if (!bool(invoke(active, invoke(getGameObject, view)))) continue;
                        const root = position(invoke(getTransform, view), getPosition);
                        if (AimMath.length(AimMath.subtract(root, self)) < SELF_RADIUS) continue;
                        const head = view.add(headOffset).readPointer();
                        if (!head.isNull()) points.push(position(head, getPosition));
                    } catch (_) { /* Inactive or destroyed view: omit this candidate. */ }
                }
            } finally { api.unpin(handle); }
            snapshot = { points, owner: owner.model, time: Date.now() };
            state.candidates = points.length;
        }
        function clearLine(origin, target) {
            const distance = Memory.alloc(4); distance.writeFloat(Math.max(0, target.distance - 0.5));
            const mask = Memory.alloc(4); mask.writeS32(-1);
            const triggers = Memory.alloc(4); triggers.writeS32(1); // QueryTriggerInteraction.Ignore
            return !bool(invoke(raycast, NULL, [vectorMemory(origin), vectorMemory(target.direction), distance, mask, triggers]));
        }
        hooks.push(Interceptor.attach(entryAddress, {
            onEnter() {
                mainThread = Process.getCurrentThreadId();
                try { pollBridge(); } catch (error) { fault(error); }
            }
        }));
        hooks.push(Interceptor.attach(updateAddress, {
            onEnter(args) {
                if (!settings.enabled || Process.getCurrentThreadId() !== mainThread) return;
                try { refresh(args[0]); } catch (error) { fault(error); }
            }
        }));
        hooks.push(Interceptor.attach(setAddress, {
            onEnter(args) {
                if (!settings.enabled || Process.getCurrentThreadId() !== mainThread || !owner || !snapshot ||
                    owner.model !== args[0].toString() || snapshot.owner !== owner.model ||
                    Date.now() - owner.time > MAX_AGE_MS || Date.now() - snapshot.time > MAX_AGE_MS) return;
                try {
                    const origin = vector(args[originIndex + 1]);
                    const forward = vector(args[directionIndex + 1]);
                    for (const target of AimMath.rank(origin, forward, snapshot.points, settings)) {
                        if (!clearLine(origin, target)) continue;
                        this.replacement = vectorMemory(target.direction);
                        args[directionIndex + 1] = this.replacement;
                        state.redirects++;
                        break;
                    }
                } catch (error) { fault(error); }
            },
            onLeave() { this.replacement = null; }
        }));
        Interceptor.flush();
        state.installed = true;
        report('installed', { method: 'ShotArgsModel.Set', rva: setAddress.sub(mod.base).toString(),
            signature: { parameters: set.parameters, returnType: set.returnType }, settings });
        return Object.assign({}, state, settings);
    } catch (error) {
        stop();
        throw error;
    } finally { api.detach(thread); }
}

rpc.exports = {
    loadmenu(path) {
        if (state.installed) throw new Error('Stop the runtime before loading another menu');
        menuModule = Process.findModuleByName('RobRunInternal.dll');
        if (!menuModule) menuModule = Module.load(path);
        connectBridge();
        return { name: menuModule.name, path: menuModule.path };
    },
    moduleinfo() {
        mod = Process.getModuleByName('GameAssembly.dll');
        return { path: mod.path, arch: Process.arch, platform: Process.platform };
    },
    start,
    configure(config) {
        const next = validateConfig(config);
        if (bridge && bridge.write(next.enabled ? 1 : 0, next.fovDegrees, next.maxDistance) !== 1)
            throw new Error('Menu rejected the requested configuration');
        settings = next;
        if (settings.enabled) state.lastError = null;
        owner = snapshot = null;
        state.candidates = 0;
        return Object.assign({}, state, settings);
    },
    status() { return Object.assign({}, state, settings); },
    stop
};
