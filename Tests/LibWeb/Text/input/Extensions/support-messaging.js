// Shared by the messaging tests: asks the content script to send one message and waits for its reply.
async function contentScriptReady() {
    if (document.documentElement.dataset.contentScriptReady === "1") return;
    await new Promise(resolve => document.addEventListener("content-script-ready", resolve, { once: true }));
}

function askBackground(type) {
    return new Promise(resolve => {
        document.addEventListener("extension-reply", event => {
            if (event.detail.type === type) resolve(document.documentElement.dataset[type]);
        });
        document.dispatchEvent(new CustomEvent("extension-request", { detail: { type } }));
    });
}
