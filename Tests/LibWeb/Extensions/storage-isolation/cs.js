(async () => {
    const output = [];
    const log = value => output.push(typeof value === "string" ? value : JSON.stringify(value));
    try {
        const local = browser.storage.local;
        await local.clear();
        if (!document.documentElement.dataset.otherStorageReady)
            await new Promise(resolve => document.addEventListener("other-storage-ready", resolve, { once: true }));
        log(await local.get("otherOnly"));
        await local.set({ ownOnly: 2 });
        const checked = new Promise(resolve =>
            document.addEventListener("other-storage-checked", resolve, { once: true })
        );
        document.dispatchEvent(new Event("check-other-storage"));
        await checked;
        log(document.documentElement.dataset.otherStorageResult);
        log(await local.get());
    } catch (error) {
        log(`ERROR: ${error.name}: ${error.message}`);
    }
    document.documentElement.dataset.storageOutput = JSON.stringify(output);
    document.dispatchEvent(new Event("storage-finished"));
})();
