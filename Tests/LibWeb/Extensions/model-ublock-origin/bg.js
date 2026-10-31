// A model of uBlock Origin's default path, with code of its own. Its listeners hear only URLs that carry "/ubo-", where
// the real one hears every URL, so the other tests' requests never wait on it.
const ourURLs = ["*://*/*/ubo-*", "ws://*/*/ubo-*", "wss://*/*/ubo-*"];
const transparentGif = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
const secret = Math.random().toString(36).slice(2);
const cosmetic = { code: "#ad-banner { display: none !important; }", cssOrigin: "user", matchAboutBlank: true };
const ours = details => details.url.includes("/ubo-");
const fileName = details => new URL(details.url).pathname.split("/").pop();

// The filters load from storage.local at startup, as compiled lists do; until they have, requests wait.
let filters = null;
const filtersLoaded = (async () => {
    await browser.storage.local.set({
        filters: { cancel: ["ad.js"], pixel: ["pixel.gif"], neuter: ["tracker.js"], styleBlocked: ["target.html"] },
    });
    filters = (await browser.storage.local.get("filters")).filters;
})();

let filteredFrameId = null;
// The badge counts the distinct URLs blocked in each tab, so a resource the page requests twice counts once.
const blockedPerTab = new Map();
function countBlocked(details) {
    const blocked = blockedPerTab.get(details.tabId) ?? new Set();
    blocked.add(details.url);
    blockedPerTab.set(details.tabId, blocked);
    browser.browserAction.setBadgeText({ text: String(blocked.size), tabId: details.tabId });
}

function decide(details) {
    const name = fileName(details);
    if (details.type === "script" && filters.cancel.includes(name)) {
        countBlocked(details);
        return { cancel: true };
    }
    if (details.type === "image" && filters.pixel.includes(name)) {
        countBlocked(details);
        return { redirectUrl: transparentGif };
    }
    if (details.type === "script" && filters.neuter.includes(name)) {
        countBlocked(details);
        return { redirectUrl: browser.runtime.getURL(`noop.js?secret=${secret}`) };
    }
}

browser.webRequest.onBeforeRequest.addListener(
    details => {
        if (!ours(details)) return;
        return filters ? decide(details) : filtersLoaded.then(() => decide(details));
    },
    { urls: ourURLs },
    ["blocking"]
);

// Guards the neutered resources' secret, as the real one does; requests for extension URLs never reach it.
browser.webRequest.onBeforeRequest.addListener(
    details => (details.url.includes(`secret=${secret}`) ? undefined : { cancel: true }),
    { urls: [browser.runtime.getURL("/*")] },
    ["blocking"]
);

// A filtered document gets a policy appended to its response headers.
browser.webRequest.onHeadersReceived.addListener(
    details => {
        if (!ours(details) || !filters?.styleBlocked.includes(fileName(details))) return;
        return {
            responseHeaders: [
                ...details.responseHeaders,
                { name: "Content-Security-Policy", value: "style-src 'none'" },
            ],
        };
    },
    { urls: ourURLs, types: ["main_frame", "sub_frame"] },
    ["blocking", "responseHeaders"]
);

// The headers each request went out with, kept until it ends.
const sentHeaders = new Map();
let documentIP = null;
browser.webRequest.onSendHeaders.addListener(
    details => {
        if (ours(details)) sentHeaders.set(details.requestId, details.requestHeaders);
    },
    { urls: ourURLs },
    ["requestHeaders", "extraHeaders"]
);
for (const event of ["onBeforeRedirect", "onCompleted", "onErrorOccurred"])
    browser.webRequest[event].addListener(details => sentHeaders.delete(details.requestId), { urls: ourURLs });
browser.webRequest.onResponseStarted.addListener(
    details => {
        if (ours(details)) documentIP = details.ip;
    },
    { urls: ourURLs, types: ["main_frame", "sub_frame"] }
);

// Scriptlets for the page a navigation is about to load, in its own world and in the extension's.
browser.webNavigation.onBeforeNavigate.addListener(details => {
    if (!details.url.includes("/ubo-")) return;
    const { hostname, pathname } = new URL(details.url);
    const common = {
        matches: [`*://${hostname}${pathname}`],
        allFrames: true,
        matchOriginAsFallback: true,
        runAt: "document_start",
    };
    browser.contentScripts.register({ ...common, world: "MAIN", js: [{ code: "window.uboScriptlet = true;" }] });
    browser.contentScripts.register({
        ...common,
        js: [{ code: "document.documentElement.dataset.uboIsolated = '1';" }],
    });
});

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const reply = promise => {
        promise.then(sendResponse, error => sendResponse({ failed: error.message }));
        return true;
    };
    // The cosmetic filters go into the frame that asks for them; the test later removes them from its frame.
    if (message.type === "cosmetic") {
        if (sender.url.includes("/ubo-")) filteredFrameId = sender.frameId;
        return reply(
            browser.tabs.insertCSS(sender.tab.id, { ...cosmetic, frameId: sender.frameId, runAt: "document_start" })
        );
    }
    if (message.type === "uncosmetic")
        return reply(browser.tabs.removeCSS(sender.tab.id, { ...cosmetic, frameId: filteredFrameId }));
    if (message.type === "records") return sendResponse({ open: sentHeaders.size, ip: documentIP !== null });
    if (message.type === "badge") return reply(browser.browserAction.getBadgeText({ tabId: sender.tab.id }));
    if (message.type === "privacy")
        return reply(
            (async () => {
                const setting = browser.privacy.network.peerConnectionEnabled;
                await setting.set({ value: false });
                const details = await setting.get({});
                await setting.clear({});
                return details;
            })()
        );
});
