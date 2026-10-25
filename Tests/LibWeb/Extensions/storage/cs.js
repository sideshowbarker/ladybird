(async () => {
    const output = [];
    const log = value => output.push(typeof value === "string" ? value : JSON.stringify(value));
    try {
        const local = browser.storage.local;
        await local.clear();
        await browser.runtime.sendMessage("reset-storage-events");
        const globalEvents = [],
            localEvents = [];
        browser.storage.onChanged.addListener((changes, area) => globalEvents.push([changes, area]));
        local.onChanged.addListener(function (changes) {
            localEvents.push([changes, arguments.length]);
        });
        await local.set({ key: null });
        await local.set({ key: null });
        await local.set({ key: 2 });
        await local.remove("key");
        await local.clear();
        await local.get();
        log(globalEvents);
        log(localEvents);
        log(await browser.runtime.sendMessage("storage-events"));
    } catch (error) {
        log(`ERROR: ${error.name}: ${error.message}`);
    }
    document.documentElement.dataset.storageOutput = JSON.stringify(output);
    document.dispatchEvent(new Event("storage-finished"));
})();
