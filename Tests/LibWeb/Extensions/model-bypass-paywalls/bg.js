// A model of Bypass Paywalls Clean's default path, with code of its own. It acts only on URLs that carry "/bpc-", so the
// other tests' pages load as they would without it.
const ours = url => url.includes("/bpc-");
const fileName = url => new URL(url).pathname.split("/").pop();

// It runs only as a self-hosted install whose manifest names its update URL, as the real one checks.
const updateUrl = browser.runtime.getManifest().browser_specific_settings?.gecko?.update_url ?? null;
let installType = null;
chrome.management.getSelf(info => (installType = info.installType));

// The page a request was made for: its originUrl, then its Referer, then its initiator.
function initiatingPage(details) {
    if (details.originUrl) return details.originUrl;
    const referer = details.requestHeaders.find(header => header.name === "Referer");
    return referer?.value ?? details.initiator ?? "";
}

const extraInfoSpec = ["blocking", "requestHeaders"];
if (browser.webRequest.OnBeforeSendHeadersOptions.EXTRA_HEADERS) extraInfoSpec.push("extraHeaders");
browser.webRequest.onBeforeSendHeaders.addListener(
    details => {
        if (!updateUrl || !ours(details.url)) return;
        const name = fileName(details.url);
        if (name === "paywall.js" && initiatingPage(details).endsWith("/target.html")) return { cancel: true };
        if (name !== "article") return;
        // Header names are compared as they're sent.
        const requestHeaders = details.requestHeaders.map(header => {
            if (header.name === "Referer") return { name: header.name, value: "https://www.google.com/" };
            if (header.name === "Cookie") return { name: header.name, value: "" };
            return header;
        });
        return { requestHeaders };
    },
    // The real one listens over *://*/*; this one hears only its own URLs, so the other tests' requests never wait on it.
    { urls: ["*://*/*/bpc-*"] },
    extraInfoSpec
);

// Once a page it acts on has loaded, it injects a library and a cleanup script in turn, then tells the page.
browser.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
    if (changeInfo.status !== "complete" || !tab.url || !ours(tab.url) || !tab.url.endsWith("/target.html")) return;
    browser.tabs.executeScript(tabId, { file: "lib.js" }, () => {
        if (browser.runtime.lastError) return;
        browser.tabs.executeScript(
            tabId,
            {
                code: `bpcLibrary.removeAll(document, ".paywall");
                    browser.runtime.onMessage.addListener(message => {
                        document.documentElement.dataset.bpcMessage = message.type;
                    });
                    true;`,
            },
            results => {
                if (browser.runtime.lastError || results?.[0] !== true) return;
                browser.tabs.sendMessage(tabId, { type: "cleaned" });
            }
        );
    });
});

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "state") return sendResponse({ updateUrl: updateUrl !== null, installType });
    if (message.type === "update-check") {
        fetch(message.url).then(
            async response => sendResponse({ status: response.status, body: await response.text() }),
            error => sendResponse({ failed: error.name })
        );
        return true;
    }
});
