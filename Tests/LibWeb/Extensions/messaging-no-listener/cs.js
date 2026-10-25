(async () => {
    try {
        await browser.runtime.sendMessage(null);
        document.documentElement.dataset.noReceiver = "unexpected success";
    } catch (error) {
        document.documentElement.dataset.noReceiver = error.message;
    }
    await new Promise(resolve =>
        browser.runtime.sendMessage(null, () => {
            document.documentElement.dataset.callbackError = browser.runtime.lastError?.message ?? "unexpected success";
            resolve();
        })
    );
    document.documentElement.dataset.contentScriptReady = "1";
    document.dispatchEvent(new CustomEvent("content-script-ready"));
})();
