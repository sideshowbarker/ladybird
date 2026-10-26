// Answers from the page the manifest names.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    sendResponse(`page at ${location.pathname}`);
});
