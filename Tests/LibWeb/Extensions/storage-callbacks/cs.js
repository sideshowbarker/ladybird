(async () => {
    const output = [];
    const log = value => output.push(typeof value === "string" ? value : JSON.stringify(value));
    try {
        const local = browser.storage.local;
        await local.clear();
        await new Promise(resolve => {
            const returned = local.set({ key: 2 }, () => {
                log("set callback");
                resolve();
            });
            log(returned === undefined);
        });
        await new Promise(resolve =>
            local.get("key", items => {
                log(items);
                resolve();
            })
        );
        await new Promise(resolve =>
            local.get(items => {
                log(items);
                resolve();
            })
        );
        await new Promise(resolve =>
            local.getBytesInUse("key", size => {
                log(size);
                resolve();
            })
        );
        await new Promise(resolve =>
            local.set({ huge: "x".repeat(local.QUOTA_BYTES) }, function () {
                log(arguments.length);
                log(browser.runtime.lastError.message);
                resolve();
            })
        );
        log(browser.runtime.lastError === undefined);
        try {
            await local.set({ huge: "x".repeat(local.QUOTA_BYTES) });
        } catch (error) {
            log(error.message);
        }
        const cyclic = {};
        cyclic.self = cyclic;
        try {
            await local.set({ key: 3, cyclic });
        } catch (error) {
            log(error instanceof TypeError);
        }
        log(await local.get("key"));
        await new Promise(resolve => local.remove("key", resolve));
        await new Promise(resolve => local.clear(resolve));
        log(await local.get());
    } catch (error) {
        log(`ERROR: ${error.name}: ${error.message}`);
    }
    document.documentElement.dataset.storageOutput = JSON.stringify(output);
    document.dispatchEvent(new Event("storage-finished"));
})();
