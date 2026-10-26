// Adds a fragment to its own URL while it starts, which mustn't make its startup count as a failure.
history.replaceState(null, "", "#started");
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "hello") sendResponse(`hello from ${location.hash}`);
});
