(async () => {
    const output = [];
    const log = value => output.push(typeof value === "string" ? value : JSON.stringify(value));
    try {
        const local = browser.storage.local;
        await local.clear();
        await local.set(JSON.parse('{"__proto__":{"polluted":true},"constructor":7}'));
        const result = await local.get();
        log(Object.hasOwn(result, "__proto__"));
        log(result.__proto__);
        log(Object.getPrototypeOf(result) === Object.prototype);
        log({}.polluted === undefined);
        const defaults = await local.get(JSON.parse('{"__proto__":0,"absent":4}'));
        log(Object.hasOwn(defaults, "__proto__"));
        log(defaults.__proto__);
        log(defaults.absent);
    } catch (error) {
        log(`ERROR: ${error.name}: ${error.message}`);
    }
    document.documentElement.dataset.storageOutput = JSON.stringify(output);
    document.dispatchEvent(new Event("storage-finished"));
})();
