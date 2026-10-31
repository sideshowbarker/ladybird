// Tries to keep 12 MiB in storage.local, over the quota an extension without unlimitedStorage has.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    browser.storage.local.set({ big: "x".repeat(12 * 1024 * 1024) }).then(
        () => sendResponse({ stored: true }),
        error => sendResponse({ rejected: error.message })
    );
    return true;
});
