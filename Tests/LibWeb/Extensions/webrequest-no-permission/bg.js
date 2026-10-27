// Tells its content script whether this background page has webRequest, which it lacks the permission for.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message === "has webRequest?") sendResponse(typeof browser.webRequest);
});
