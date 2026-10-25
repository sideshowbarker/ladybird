const changes = [];
browser.storage.onChanged.addListener((change, area) => changes.push([change, area]));
browser.runtime.onMessage.addListener(message => {
    if (message === "reset-storage-events") {
        changes.length = 0;
        return Promise.resolve(true);
    }
    if (message === "storage-events") return Promise.resolve(changes);
});
