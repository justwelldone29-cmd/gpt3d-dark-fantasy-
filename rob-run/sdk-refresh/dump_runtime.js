// Read runtime IL2CPP metadata through its public exports. No file patches.
'use strict';

setImmediate(function () {
    let thread = null;
    let api = null;
    let failure = null;
    let total = 0;
    try {
        const mod = Process.getModuleByName('GameAssembly.dll');
        const bind = (name, ret, args) => {
            const address = mod.findExportByName('il2cpp_' + name);
            if (!address || address.isNull()) throw new Error('Missing export: il2cpp_' + name);
            return new NativeFunction(address, ret, args);
        };
        api = {
            domain: bind('domain_get', 'pointer', []),
            assemblies: bind('domain_get_assemblies', 'pointer', ['pointer', 'pointer']),
            image: bind('assembly_get_image', 'pointer', ['pointer']),
            imageName: bind('image_get_name', 'pointer', ['pointer']),
            classCount: bind('image_get_class_count', 'size_t', ['pointer']),
            classAt: bind('image_get_class', 'pointer', ['pointer', 'uint32']),
            name: bind('class_get_name', 'pointer', ['pointer']),
            namespace: bind('class_get_namespace', 'pointer', ['pointer']),
            parent: bind('class_get_parent', 'pointer', ['pointer']),
            classToken: bind('class_get_type_token', 'uint32', ['pointer']),
            fields: bind('class_get_fields', 'pointer', ['pointer', 'pointer']),
            fieldName: bind('field_get_name', 'pointer', ['pointer']),
            fieldOffset: bind('field_get_offset', 'int32', ['pointer']),
            fieldType: bind('field_get_type', 'pointer', ['pointer']),
            fieldFlags: bind('field_get_flags', 'uint32', ['pointer']),
            typeName: bind('type_get_name', 'pointer', ['pointer']),
            free: bind('free', 'void', ['pointer']),
            methods: bind('class_get_methods', 'pointer', ['pointer', 'pointer']),
            methodName: bind('method_get_name', 'pointer', ['pointer']),
            paramCount: bind('method_get_param_count', 'uint32', ['pointer']),
            paramType: bind('method_get_param', 'pointer', ['pointer', 'uint32']),
            paramName: bind('method_get_param_name', 'pointer', ['pointer', 'uint32']),
            returnType: bind('method_get_return_type', 'pointer', ['pointer']),
            methodFlags: bind('method_get_flags', 'uint32', ['pointer', 'pointer']),
            methodToken: bind('method_get_token', 'uint32', ['pointer']),
            attach: bind('thread_attach', 'pointer', ['pointer']),
            detach: bind('thread_detach', 'void', ['pointer'])
        };
        const text = p => p.isNull() ? '' : p.readUtf8String();
        const number = n => typeof n === 'number' ? n : n.toNumber();
        const type = p => {
            if (p.isNull()) return '';
            const name = api.typeName(p);
            try { return text(name); } finally { if (!name.isNull()) api.free(name); }
        };
        const fullName = k => {
            if (k.isNull()) return '';
            const ns = text(api.namespace(k));
            return (ns ? ns + '.' : '') + text(api.name(k));
        };
        const domain = api.domain();
        if (domain.isNull()) throw new Error('IL2CPP domain is not initialized yet');
        thread = api.attach(domain);
        if (thread.isNull()) throw new Error('Could not attach the metadata-reading thread');
        const countPtr = Memory.alloc(Process.pointerSize);
        countPtr.writePointer(NULL);
        const assemblies = api.assemblies(domain, countPtr);
        const count = Process.pointerSize === 8 ? number(countPtr.readU64()) : countPtr.readU32();
        if (assemblies.isNull() || count === 0 || count > 4096) throw new Error('Invalid assembly list');
        send({ kind: 'begin', schema: 2, pid: Process.id, arch: Process.arch,
            pointer_size: Process.pointerSize, module_path: mod.path, module_size: mod.size });
        let batch = [];
        const flush = () => { if (batch.length) { send({ kind: 'batch', records: batch }); batch = []; } };
        const flagsPtr = Memory.alloc(4);
        for (let i = 0; i < count; i++) {
            const assembly = assemblies.add(i * Process.pointerSize).readPointer();
            const image = api.image(assembly);
            if (image.isNull()) continue;
            const imageName = text(api.imageName(image));
            const classes = number(api.classCount(image));
            if (!Number.isSafeInteger(classes) || classes < 0 || classes > 1000000)
                throw new Error('Invalid class count in ' + imageName);
            for (let c = 0; c < classes; c++) {
                const klass = api.classAt(image, c);
                if (klass.isNull()) continue;
                const fields = [], methods = [], signatures = [];
                const iter = Memory.alloc(Process.pointerSize);
                iter.writePointer(NULL);
                for (;;) {
                    const field = api.fields(klass, iter);
                    if (field.isNull()) break;
                    fields.push([text(api.fieldName(field)), api.fieldOffset(field),
                        type(api.fieldType(field)), api.fieldFlags(field)]);
                }
                iter.writePointer(NULL);
                for (;;) {
                    const method = api.methods(klass, iter);
                    if (method.isNull()) break;
                    const name = text(api.methodName(method));
                    const argc = api.paramCount(method);
                    let rva = 0;
                    const address = method.readPointer();
                    if (!address.isNull() && address.compare(mod.base) >= 0 &&
                        address.compare(mod.base.add(mod.size)) < 0)
                        rva = address.sub(mod.base).toUInt32();
                    methods.push([name, argc, rva]);
                    const params = [];
                    for (let p = 0; p < argc; p++) params.push({
                        name: text(api.paramName(method, p)), type: type(api.paramType(method, p)) });
                    flagsPtr.writeU32(0);
                    const flags = api.methodFlags(method, flagsPtr);
                    signatures.push({ name: name, rva: rva, token: api.methodToken(method),
                        flags: flags, implementation_flags: flagsPtr.readU32(),
                        return_type: type(api.returnType(method)), parameters: params });
                }
                batch.push({ a: imageName, n: fullName(klass), p: fullName(api.parent(klass)),
                    token: api.classToken(klass), f: fields, m: methods, signatures: signatures });
                total++;
                if (batch.length >= 50) flush();
            }
            flush();
            send({ kind: 'progress', assembly: imageName, classes: total });
        }
    } catch (error) {
        failure = error.stack || String(error);
    } finally {
        try {
            if (thread && !thread.isNull() && api) api.detach(thread);
        } catch (error) { failure = failure || error.stack || String(error); }
    }
    send(failure ? { kind: 'failure', error: failure } : { kind: 'done', classes: total });
});
