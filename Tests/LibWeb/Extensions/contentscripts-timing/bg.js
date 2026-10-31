// For each navigation to a page named timing-target.html, registers scriptlets for it as the navigation begins, and
// holds the page's own request for 100ms meanwhile. The scriptlets match the page alone, and its host's origin, which
// is what an about:blank or data: frame of the page matches as.
browser.webNavigation.onBeforeNavigate.addListener(details => {
    if (!details.url.includes("timing-target.html")) return;
    const { hostname, pathname } = new URL(details.url);
    const common = {
        matches: [`*://${hostname}${pathname}`, `*://${hostname}/`],
        allFrames: true,
        matchOriginAsFallback: true,
        runAt: "document_start",
    };
    browser.contentScripts.register({ ...common, world: "MAIN", js: [{ code: "window.scriptletRan = true;" }] });
    browser.contentScripts.register({
        ...common,
        js: [{ code: "document.documentElement.dataset.isolatedRan = '1';" }],
    });
});

browser.webRequest.onBeforeRequest.addListener(
    details => {
        if (!details.url.includes("timing-target.html")) return;
        return new Promise(resolve => setTimeout(() => resolve({}), 100));
    },
    { urls: ["<all_urls>"], types: ["main_frame", "sub_frame"] },
    ["blocking"]
);
