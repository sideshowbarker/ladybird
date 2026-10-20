// The content script relays what the page asks for to the background page, and reports replies into the DOM.
document.addEventListener("extension-request", async event => {
    const { type } = event.detail;
    let result;
    try {
        result = JSON.stringify(await browser.runtime.sendMessage({ type }));
    } catch (error) {
        result = `error: ${error.name}`;
    }
    document.documentElement.dataset[type] = result;
    document.dispatchEvent(new CustomEvent("extension-reply", { detail: { type } }));
});
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "from-background")
        sendResponse(
            `hi from ${sender.url.endsWith("_generated_background_page.html") ? "the background" : "elsewhere"}`
        );
});
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
