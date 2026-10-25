(async () => {
    const output = [];
    const log = value => output.push(typeof value === "string" ? value : JSON.stringify(value));
    try {
        const local = browser.storage.local;
        await local.clear();
        await local.set({ a: 1, nil: null, nested: { b: [true, "value"] } });
        log(await local.get());
        log(await local.get(null));
        log(await local.get("a"));
        log(await local.get(["nil", "missing"]));
        log(await local.get({ a: 9, fallback: 2, nil: "default" }));
        log(await local.get([]));
        log(await local.get({}));
        log(await local.getBytesInUse(["a", "a"]));
        await local.remove(["a", "missing"]);
        log(await local.get("a"));
        log(await local.get("nil"));
        await local.remove("nil");
        await local.clear();
        log(await local.getBytesInUse(null));
        log(local.QUOTA_BYTES);
    } catch (error) {
        log(`ERROR: ${error.name}: ${error.message}`);
    }
    document.documentElement.dataset.storageOutput = JSON.stringify(output);
    document.dispatchEvent(new Event("storage-finished"));
})();
