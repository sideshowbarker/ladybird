// Answers for the test pages' requests, chosen by the name their URL's path ends in. Only the pages' own echo URLs,
// which carry a "wr-" token, wait on these listeners.
const filter = { urls: ["*://*/*wr-*"] };
const named = (details, name) => new URL(details.url).pathname.endsWith(`/${name}`);

browser.webRequest.onBeforeRequest.addListener(
    details => {
        if (named(details, "cancel.js")) return { cancel: true };
        if (named(details, "promise-cancel.js"))
            return new Promise(resolve => setTimeout(() => resolve({ cancel: true }), 200));
        if (named(details, "throws.js")) throw new Error("This listener gives no answer");
        if (named(details, "rejects.js")) return Promise.reject(new Error("Neither does this one"));
        const redirects = {
            "redirect-to-echo.js": details.url.replace("redirect-to-echo.js", "redirected.js"),
            "redirect-page.html": details.url.replace("redirect-page.html", "redirected-page.html"),
            "redirect-to-resource.js": browser.runtime.getURL("noop.js"),
            "redirect-to-private.js": browser.runtime.getURL("private.js"),
            "redirect-to-javascript.js": "javascript:window.javascriptRan = true",
            "redirect-to-data.gif": "data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==",
        };
        for (const [name, redirectUrl] of Object.entries(redirects)) {
            if (named(details, name)) return { redirectUrl };
        }
    },
    filter,
    ["blocking"]
);

// A listener that removes itself and adds others while it runs still answers for the request it heard.
const removesItself = () => {
    browser.webRequest.onBeforeRequest.removeListener(removesItself);
    for (let i = 0; i < 64; ++i)
        browser.webRequest.onBeforeRequest.addListener(() => {}, { urls: ["*://*/*wr-never-matches/*"] }, ["blocking"]);
    return { cancel: true };
};
browser.webRequest.onBeforeRequest.addListener(removesItself, { urls: ["*://*/*/removes-itself.js"] }, ["blocking"]);

// Rewrites the headers the way a paywall bypass does: the referrer becomes a search engine, and the cookie goes.
browser.webRequest.onBeforeSendHeaders.addListener(
    details => {
        if (!named(details, "rewrite-headers.txt")) return;
        const requestHeaders = details.requestHeaders.map(header => {
            if (header.name === "Referer") return { name: header.name, value: "https://www.google.com/" };
            if (header.name === "Cookie") return { name: header.name, value: "" };
            return header;
        });
        return { requestHeaders };
    },
    filter,
    ["blocking", "requestHeaders"]
);

browser.webRequest.onHeadersReceived.addListener(
    details => {
        if (named(details, "strip-cookie.txt"))
            return {
                responseHeaders: details.responseHeaders.filter(header => header.name.toLowerCase() !== "set-cookie"),
            };
        if (named(details, "add-csp.html"))
            return {
                responseHeaders: [
                    ...details.responseHeaders,
                    { name: "Content-Security-Policy", value: "script-src 'none'" },
                ],
            };
    },
    filter,
    ["blocking", "responseHeaders"]
);
