// Shared by the tabs tests: has the background page run one tabs.query(), and returns its reply.
async function contentScriptReady() {
    if (document.documentElement.dataset.contentScriptReady === "1") return;
    await new Promise(resolve => document.addEventListener("content-script-ready", resolve, { once: true }));
}

function askBackground(query) {
    return new Promise(resolve => {
        document.addEventListener("tabs-reply", () => resolve(JSON.parse(document.documentElement.dataset.tabsReply)), {
            once: true,
        });
        document.dispatchEvent(new CustomEvent("tabs-request", { detail: JSON.stringify({ query }) }));
    });
}
