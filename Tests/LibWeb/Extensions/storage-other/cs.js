(async () => {
    await browser.storage.local.clear();
    await browser.storage.local.set({ otherOnly: 1 });
    document.addEventListener("check-other-storage", async () => {
        document.documentElement.dataset.otherStorageResult = JSON.stringify(await browser.storage.local.get());
        document.dispatchEvent(new Event("other-storage-checked"));
    });
    document.documentElement.dataset.otherStorageReady = "1";
    document.dispatchEvent(new Event("other-storage-ready"));
})();
