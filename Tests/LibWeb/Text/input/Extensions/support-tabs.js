// Shared by the tabs tests, whose content scripts relay what the page asks for to the background page.
async function contentScriptReady() {
    if (document.documentElement.dataset.contentScriptReady === "1") return;
    await new Promise(resolve => document.addEventListener("content-script-ready", resolve, { once: true }));
}

// Has the content script send one message to the background page, and returns the reply.
function sendToBackground(message) {
    return new Promise(resolve => {
        document.addEventListener("tabs-reply", () => resolve(JSON.parse(document.documentElement.dataset.tabsReply)), {
            once: true,
        });
        document.dispatchEvent(new CustomEvent("tabs-request", { detail: JSON.stringify(message) }));
    });
}

function askBackground(query) {
    return sendToBackground({ query });
}
